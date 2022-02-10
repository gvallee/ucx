/**
* Copyright (C) NVIDIA CORPORATIONS. 2022.  ALL RIGHTS RESERVED.
*
* See file LICENSE for terms.
*/

#include "ib_mlx5_xgvmi_mkey.h"

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <stdlib.h>
#include <stdio.h>

ucs_status_t create_xgvmi_key(struct ibv_pd *pd, void *buf, int buf_size)
{
    struct ibv_mr *mr;

    // 32 bits token
	char access_key[] = "eShVkYp3s6v9y$B&E)H@McQfTjWnZq4t";

#ifndef IBVMR
	mr = create_cgmk_mkey(pd, buf, buf_size);
	if (!mr) {
		fprintf(stderr, "Error on mkey create action.\n");
		goto error_out;
	}
	printf("DEVX MKEY idx: %x\n", mr->lkey);
#else
	mr = ibv_reg_mr(pd, buff, buff_size, IBV_ACCESS_LOCAL_WRITE
			| IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE);
	if (!mr) {
		fprintf(stderr, "Couldn't allocate ibv mr.\n");
		goto clean_pd;
	}
	printf("MR lkey: %x\n", mr->lkey);
#endif
	// Allow cross gvmi on MKEY
	char mr_desc[256];
	size_t mr_desc_len;
	mr_desc_len = cgmk_mr_export(mr, access_key, sizeof(access_key),
			mr_desc, sizeof(mr_desc));
	if (!mr_desc_len) {
		fprintf(stderr, "Error on mr export.\n");
		goto error_out;
	}

error_out:
    return UCS_ERR_NO_MESSAGE;
}