/**
 * Copyright (C) NVIDIA CORPORATION Ltd. 2025.  ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#include "ldo_dev.h"
#include <flexio_dev_err.h>

__dpa_rpc__ uint64_t ldo_context_setup(uint64_t arg)
{
	_Static_assert(sizeof(struct ldo_dev_worker) % 64 == 0,
		"The stream worker structure %u must be aligned to 64B\n");
	_Static_assert(sizeof(struct ldo_cmd) == LDO_L2V(LDO_LOG_CMD_SIZE),
		"The internal collective command structure must be 64B\n");
	_Static_assert(sizeof(struct ldo_dev_net_worker_db) % 64 == 0,
		"The net worker structure must be aligned to 64B\n");
	_Static_assert(sizeof(struct ldo_dev_trig_worker_db) % 64 == 0,
		"The triggered worker structure must be aligned to 64B\n");
	return arg + 1;
	return arg + 1;
}

__dpa_rpc__ uint64_t ldo_context_finalize(uint64_t remote_cmp_flag_daddr)
{
	int i;
	uint64_t *remote_flags = (uint64_t *) remote_cmp_flag_daddr;

	for(i = 0; i < LDO_L2V(LDO_LOG_CONCURRENT_COLLS); i++) {
		flexio_dev_print(" 0x%lx: %lu ", (uint64_t) &remote_flags[i], remote_flags[i]);
	}
	flexio_dev_print("\n");
	return 0;
}

__dpa_global__ void ldo_error_handler(uint64_t arg1, uint64_t arg2)
{
	struct flexio_dev_thread_ctx *tctx =
			(struct flexio_dev_thread_ctx *) arg1;

	flexio_dev_print("DPA Error handler! (err = %lu)\n",
				flexio_dev_get_errno(tctx));
	flexio_dev_thread_reschedule();
}