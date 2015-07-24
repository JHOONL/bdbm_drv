/*
The MIT License (MIT)

Copyright (c) 2014-2015 CSAIL, MIT

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#if defined (KERNEL_MODE)
#include <linux/module.h>
#include <linux/slab.h>

#elif defined (USER_MODE)
#include <stdio.h>
#include <stdint.h>

#else
#error Invalid Platform (KERNEL_MODE or USER_MODE)
#endif

#include "debug.h"
#include "platform.h"
#include "params.h"
#include "bdbm_drv.h"
#include "llm_mq.h"
#include "uthread.h"

#include "queue/queue.h"
#include "queue/prior_queue.h"
#include "utils/utime.h"


/* NOTE: This serializes all of the requests from the host file system; 
 * it is useful for debugging */
/*#define ENABLE_SEQ_DBG*/

/* NOTE: This is just a quick fix to support RMW; 
 * it must be improved later for better performance */
#define QUICK_FIX_FOR_RWM	


/* llm interface */
struct bdbm_llm_inf_t _llm_mq_inf = {
	.ptr_private = NULL,
	.create = llm_mq_create,
	.destroy = llm_mq_destroy,
	.make_req = llm_mq_make_req,
	.flush = llm_mq_flush,
	.end_req = llm_mq_end_req,
};

/* private */
struct bdbm_llm_mq_private {
	uint64_t nr_punits;
	bdbm_mutex* punit_locks;
	struct bdbm_prior_queue_t* q;

	/* for debugging */
#if defined(ENABLE_SEQ_DBG)
	bdbm_mutex dbg_seq;
#endif	

	/* for thread management */
	bdbm_thread_t* llm_thread;
};

int __llm_mq_thread (void* arg)
{
	struct bdbm_drv_info* bdi = (struct bdbm_drv_info*)arg;
	struct bdbm_llm_mq_private* p = (struct bdbm_llm_mq_private*)BDBM_LLM_PRIV(bdi);
	uint64_t loop;

	for (;;) {
		/* give a chance to other processes if Q is empty */
		if (bdbm_prior_queue_is_all_empty (p->q)) {
			if (bdbm_thread_schedule (p->llm_thread) == SIGKILL) {
				break;
			}
		}

		/* send reqs until Q becomes empty */
		for (loop = 0; loop < p->nr_punits; loop++) {
			struct bdbm_prior_queue_item_t* qitem = NULL;
			struct bdbm_llm_req_t* ptr_req = NULL;

			/* if pu is busy, then go to the next pnit */
			if (!bdbm_mutex_try_lock (&p->punit_locks[loop]))
				continue;

			if ((ptr_req = (struct bdbm_llm_req_t*)bdbm_prior_queue_dequeue (p->q, loop, &qitem)) == NULL) {
				bdbm_mutex_unlock (&p->punit_locks[loop]);
				continue;
			}
			ptr_req->ptr_qitem = qitem;

			pmu_update_q (bdi, ptr_req);

			if (bdi->ptr_dm_inf->make_req (bdi, ptr_req)) {
				bdbm_mutex_unlock (&p->punit_locks[loop]);

				/* TODO: I do not check whether it works well or not */
				bdi->ptr_llm_inf->end_req (bdi, ptr_req);
				bdbm_warning ("oops! make_req failed");
			}
		}
	}

	return 0;
}

void __llm_mq_create_fail (struct bdbm_llm_mq_private* p)
{
	if (!p) 
		return;
	if (p->punit_locks)
		bdbm_free_atomic (p->punit_locks);
	if (p->q)
		bdbm_prior_queue_destroy (p->q);
	bdbm_free_atomic (p);
}

uint32_t llm_mq_create (struct bdbm_drv_info* bdi)
{
	struct bdbm_llm_mq_private* p;
	uint64_t loop;

	/* create a private info for llm_nt */
	if ((p = (struct bdbm_llm_mq_private*)bdbm_malloc_atomic
			(sizeof (struct bdbm_llm_mq_private))) == NULL) {
		bdbm_error ("bdbm_malloc_atomic failed");
		return -1;
	}

	/* get the total number of parallel units */
	p->nr_punits =
		bdi->ptr_bdbm_params->nand.nr_channels *
		bdi->ptr_bdbm_params->nand.nr_chips_per_channel;

	/* create queue */
	if ((p->q = bdbm_prior_queue_create (p->nr_punits, INFINITE_QUEUE)) == NULL) {
		bdbm_error ("bdbm_prior_queue_create failed");
		__llm_mq_create_fail (p);
		return -1;
	}

	/* create completion locks for parallel units */
	if ((p->punit_locks = (bdbm_mutex*)bdbm_malloc_atomic
			(sizeof (bdbm_mutex) * p->nr_punits)) == NULL) {
		bdbm_error ("bdbm_malloc_atomic failed");
		__llm_mq_create_fail (p);
		return -1;
	}
	for (loop = 0; loop < p->nr_punits; loop++) {
		bdbm_mutex_init (&p->punit_locks[loop]);
	}

	/* keep the private structures for llm_nt */
	bdi->ptr_llm_inf->ptr_private = (void*)p;

	/* create & run a thread */
	if ((p->llm_thread = bdbm_thread_create (
			__llm_mq_thread, bdi, "__llm_mq_thread")) == NULL) {
		bdbm_error ("kthread_create failed");
		__llm_mq_create_fail (p);
		return -1;
	}

#if defined(ENABLE_SEQ_DBG)
	bdbm_mutex_init (&p->dbg_seq);
#endif

	return 0;
}

/* NOTE: we assume that all of the host requests are completely served.
 * the host adapter must be first closed before this function is called.
 * if not, it would work improperly. */
void llm_mq_destroy (struct bdbm_drv_info* bdi)
{
	uint64_t loop;
	struct bdbm_llm_mq_private* p = (struct bdbm_llm_mq_private*)BDBM_LLM_PRIV(bdi);

	/* wait until Q becomes empty */
	while (!bdbm_prior_queue_is_all_empty (p->q)) {
		bdbm_thread_msleep (1);
	}

	/* kill kthread */
	bdbm_thread_stop (p->llm_thread);

	for (loop = 0; loop < p->nr_punits; loop++)
		bdbm_mutex_lock (&p->punit_locks[loop]);

	/* release all the relevant data structures */
	bdbm_prior_queue_destroy (p->q);
	bdbm_free_atomic (p);
}

uint32_t llm_mq_make_req (struct bdbm_drv_info* bdi, struct bdbm_llm_req_t* llm_req)
{
	uint32_t ret;
	uint64_t punit_id;
	struct bdbm_llm_mq_private* p = (struct bdbm_llm_mq_private*)BDBM_LLM_PRIV(bdi);

#if defined(ENABLE_SEQ_DBG)
	bdbm_mutex_lock (&p->dbg_seq);
#endif

	/* obtain the elapsed time taken by FTL algorithms */
	pmu_update_sw (bdi, llm_req);

	/* get a parallel unit ID */
	punit_id = llm_req->phyaddr->channel_no * 
		BDBM_GET_NAND_PARAMS (bdi)->nr_chips_per_channel +
		llm_req->phyaddr->chip_no;

	while (bdbm_prior_queue_get_nr_items (p->q) >= 256) {
		/*yield ();*/
		bdbm_thread_yield ();
	}

	/* put a request into Q */
#if defined(QUICK_FIX_FOR_RWM)
	if (llm_req->req_type == REQTYPE_RMW_READ) {
		/* FIXME: this is a quick fix to support RMW; it must be improved later */
		/* step 1: put READ first */
		if ((ret = bdbm_prior_queue_enqueue (p->q, punit_id, llm_req->lpa, (void*)llm_req))) {
			bdbm_msg ("bdbm_prior_queue_enqueue failed");
		}

		/* step 2: put WRITE second with the same LPA */
		punit_id = llm_req->phyaddr_w.channel_no * 
			BDBM_GET_NAND_PARAMS (bdi)->nr_chips_per_channel +
			llm_req->phyaddr_w.chip_no;
		if ((ret = bdbm_prior_queue_enqueue (p->q, punit_id, llm_req->lpa, (void*)llm_req))) {
			bdbm_msg ("bdbm_prior_queue_enqueue failed");
		}
	} else {
		if ((ret = bdbm_prior_queue_enqueue (p->q, punit_id, llm_req->lpa, (void*)llm_req))) {
			bdbm_msg ("bdbm_prior_queue_enqueue failed");
		}
	}
#else
	if ((ret = bdbm_prior_queue_enqueue (p->q, punit_id, llm_req->lpa, (void*)llm_req))) {
		bdbm_msg ("bdbm_prior_queue_enqueue failed");
	}
#endif

	/* wake up thread if it sleeps */
	bdbm_thread_wakeup (p->llm_thread);

	return ret;
}

void llm_mq_flush (struct bdbm_drv_info* bdi)
{
	struct bdbm_llm_mq_private* p = (struct bdbm_llm_mq_private*)BDBM_LLM_PRIV(bdi);

	while (bdbm_prior_queue_is_all_empty (p->q) != 1) {
		/*cond_resched ();*/
		bdbm_thread_yield ();
	}
}

void llm_mq_end_req (struct bdbm_drv_info* bdi, struct bdbm_llm_req_t* llm_req)
{
	uint64_t punit_id;
	struct bdbm_llm_mq_private* p = (struct bdbm_llm_mq_private*)BDBM_LLM_PRIV(bdi);
	struct bdbm_prior_queue_item_t* qitem = (struct bdbm_prior_queue_item_t*)llm_req->ptr_qitem;

	switch (llm_req->req_type) {
	case REQTYPE_RMW_READ:
		/* get a parallel unit ID */
		punit_id = llm_req->phyaddr->channel_no * 
			BDBM_GET_NAND_PARAMS (bdi)->nr_chips_per_channel +
			llm_req->phyaddr->chip_no;

		bdbm_mutex_unlock (&p->punit_locks[punit_id]);

		/* change its type to WRITE if req_type is RMW */
		llm_req->phyaddr = &llm_req->phyaddr_w;
		llm_req->req_type = REQTYPE_RMW_WRITE;

		punit_id = llm_req->phyaddr->channel_no * 
			BDBM_GET_NAND_PARAMS (bdi)->nr_chips_per_channel +
			llm_req->phyaddr->chip_no;

#ifdef QUICK_FIX_FOR_RWM
		bdbm_prior_queue_remove (p->q, qitem);
#else
		/* put it to Q again */
		if (bdbm_prior_queue_move (p->q, punit_id, qitem)) {
			bdbm_msg ("bdbm_prior_queue_enqueue failed");
			bdbm_bug_on (1);
		}
#endif

		pmu_inc (bdi, llm_req);

		/* wake up thread if it sleeps */
		bdbm_thread_wakeup (p->llm_thread);
		break;

	case REQTYPE_READ:
	case REQTYPE_READ_DUMMY:
	case REQTYPE_WRITE:
	case REQTYPE_RMW_WRITE:
	case REQTYPE_GC_READ:
	case REQTYPE_GC_WRITE:
	case REQTYPE_GC_ERASE:
	case REQTYPE_TRIM:
		/* get a parallel unit ID */
		punit_id = llm_req->phyaddr->channel_no * 
			BDBM_GET_NAND_PARAMS (bdi)->nr_chips_per_channel +
			llm_req->phyaddr->chip_no;

		bdbm_prior_queue_remove (p->q, qitem);

		/* complete a lock */
		bdbm_mutex_unlock (&p->punit_locks[punit_id]);

		/* update the elapsed time taken by NAND devices */
		pmu_update_tot (bdi, llm_req);
		pmu_inc (bdi, llm_req);

		/* finish a request */
		bdi->ptr_hlm_inf->end_req (bdi, llm_req);

#if defined(ENABLE_SEQ_DBG)
		bdbm_mutex_unlock (&p->dbg_seq);
#endif
		break;

	default:
		bdbm_error ("invalid req-type (%u)", llm_req->req_type);
		break;
	}
}
