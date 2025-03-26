/**
 * Copyright (C) NVIDIA CORPORATION Ltd. 2025.  ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

 #include "ldo_dev.h"

struct ldo_cmd* ldo_dev_get_cmd_ptr(struct flexio_dev_thread_ctx *dtctx,
			struct ldo_dev_worker *worker)
{
	uint32_t cmd_index;
	uint64_t cmd_offset;
	struct ldo_cmd *cmd_ptr;

	flexio_dev_window_mkey_config(dtctx, worker->db.host_cmd_buf_mkey);

	cmd_index = worker->db.cmd_index;

	cmd_offset = (sizeof(struct ldo_cmd) * cmd_index);

	flexio_dev_window_ptr_acquire(dtctx,
				      cmd_offset,
				      (flexio_uintptr_t *)&cmd_ptr);

	return cmd_ptr;
}

static inline void ldo_dev_read_host_cmd(struct flexio_dev_thread_ctx *dtctx,
						struct ldo_dev_worker *worker)
{
	struct ldo_cmd *dev_cmd_ptr =
		ldo_dev_get_cmd_ptr(dtctx, worker);

	/* We need to invalidate the window because
	 * we may have read this circular command buffer before */
	__dpa_thread_window_read_inv();

	worker->cmd = *dev_cmd_ptr;
}

static inline void ldo_dev_poll_until_trigger(struct flexio_dev_thread_ctx *dtctx,
					struct ldo_dev_worker *worker,
					uint64_t t_start,
					int *hangup)
{
	int found = 0;
	uint64_t *trig_ptr_be, trig_val;

	flexio_dev_window_mkey_config(dtctx, worker->cmd.trigger_lkey);

	flexio_dev_window_ptr_acquire(dtctx, 0,
				      (flexio_uintptr_t *)&trig_ptr_be);

	while(LDO_POLL_TIME_REMAIN(t_start)) {
		__dpa_thread_window_read_inv();
		trig_val = be64_to_cpu(*trig_ptr_be);
		if(trig_val >= worker->cmd.trigger_threshold) {
			found = 1;
			break;
		}
	}

	if (!found)
		*hangup = 1;
}

void ldo_dev_process_host_cmd(struct flexio_dev_thread_ctx *dtctx,
                        struct ldo_dev_worker *worker, uint64_t t_start,
			int *hangup)
{
	/* Host command has been read into the stack
	 * either by ldo_dev_activation_host_cmd() or
	 * ldo_dev_process_a2a_comp() / activation_a2a_comp */

	ldo_dev_poll_until_trigger(dtctx, worker, t_start, hangup);
}

void ldo_dev_activation_host_cmd(struct flexio_dev_thread_ctx *dtctx,
					struct ldo_dev_worker *worker)
{
	ldo_dev_advance_trig_cq(worker);

	ldo_dev_read_host_cmd(dtctx, worker);
}

void ldo_dev_wait_host_cmd(struct ldo_dev_worker *worker)
{
	uint32_t cmd_index;
	uint64_t cmd_offset, valid_count, cmp_count;

	cmd_index = worker->db.cmd_index;
	cmp_count = worker->db.cmp_count;

	cmd_offset = (cmd_index * sizeof(struct ldo_cmd)) +
			offsetof(struct ldo_cmd, valid_count);

	valid_count = (cmp_count >> LDO_WORKER_LOG_CMDQ_DEPTH) + 1;

	/* Wait until valid_count is the next expected */
	ldo_dev_format_trig_wod(worker, cmd_offset,
				worker->db.host_cmd_buf_mkey,
				LDO_WAIT_ON_DATA_OP_EQUAL,
				0, /* inv */
				MLX5_CTRL_SEG_CE_CQE_ALWAYS,
				cpu_to_be64(valid_count),
				0xffffffffffffffff);	
}

void ldo_dev_hangup_host_cmd(struct flexio_dev_thread_ctx *dtctx,
					struct ldo_dev_worker *worker)
{
	ldo_dev_wait_host_cmd(worker);

	__dpa_thread_memory_writeback();

	flexio_dev_qp_sq_ring_db(dtctx, worker->db.trig_sq_pi,
					worker->db.trig_qpn);
	flexio_dev_cq_arm(dtctx, worker->db.trig_cq_idx,
					worker->db.trig_cqn);
}