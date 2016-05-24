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

#ifndef __BDBM_DEVICES__
#define __BDBM_DEVICES__

#if defined (USER_MODE)
#include <stdint.h>
#endif

#include "bdbm_drv.h"
#include "algo/abm.h"

//int bdbm_dm_init (bdbm_drv_info_t* bdi);
//bdbm_dm_inf_t* bdbm_dm_get_inf (bdbm_drv_info_t* bdi);
//void bdbm_dm_exit (bdbm_drv_info_t* bdi);
int bdbm_aggr_init (bdbm_drv_info_t* bdi, uint8_t volume);
bdbm_dm_inf_t* bdbm_aggr_get_inf (bdbm_drv_info_t* bdi);
void bdbm_aggr_exit (bdbm_drv_info_t* bdi);
#endif
