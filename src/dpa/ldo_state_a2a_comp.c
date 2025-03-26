/**
 * Copyright (C) NVIDIA CORPORATION Ltd. 2025.  ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

 #include "ldo_dev.h"

static inline uint64_t ldo_get_cmp_haddr(
		struct ldo_dev_worker *worker,
		uint32_t cmd_index)
{
	uint64_t haddr;

	haddr = worker->db.host_cmd_info_addr + 
		(sizeof(struct ldo_cmd_info) * cmd_index);

	return haddr;
}

static void ldo_dev_complete_coll(struct flexio_dev_thread_ctx *dtctx,
			struct ldo_dev_worker *worker,
			uint32_t cmd_index,
			uint64_t cmp_count)
{
	uint64_t *dev_cmd_info_ptr;
	uint64_t *dev_cmp_ptr;

	flexio_dev_window_mkey_config(dtctx,
				      worker->db.host_cmd_info_mkey);

	flexio_dev_window_ptr_acquire(dtctx,
			ldo_get_cmp_haddr(worker, cmd_index),
			(flexio_uintptr_t *)&dev_cmd_info_ptr);

	*dev_cmd_info_ptr = LDO_DEV_CMD_COMPLETED;

	flexio_dev_window_mkey_config(dtctx,
				      worker->db.host_cmp_mkey);
	flexio_dev_window_ptr_acquire(dtctx,
				      worker->db.host_cmp_addr,
				      (flexio_uintptr_t *)&dev_cmp_ptr);

	*dev_cmp_ptr = cmp_count;

	__dpa_thread_window_writeback();
}

static inline void ldo_dev_poll_until_host_cmd(
				struct flexio_dev_thread_ctx *dtctx,
				struct ldo_dev_worker *worker,
				uint64_t t_start,
				int *hangup)
{
	int found = 0;
	struct ldo_cmd *dev_cmd_ptr;
	uint64_t curr_valid_count, expected_valid_count;

	dev_cmd_ptr = ldo_dev_get_cmd_ptr(dtctx, worker);

	expected_valid_count = 
		(worker->db.cmp_count >> LDO_WORKER_LOG_CMDQ_DEPTH) + 1;

	while(LDO_POLL_TIME_REMAIN(t_start)) {
		__dpa_thread_window_read_inv();
		worker->cmd = *dev_cmd_ptr;
		curr_valid_count = be64_to_cpu(worker->cmd.valid_count);
		if(curr_valid_count == expected_valid_count) {
			found = 1;
			break;
		}
	}

	if (!found)
		*hangup = 1;
}

void ldo_dev_process_a2a_comp(struct flexio_dev_thread_ctx *dtctx,
				struct ldo_dev_worker *worker,
				uint64_t t_start,
				int *hangup)
{
	uint8_t finished;
	uint32_t cmd_index;
	uint64_t cmp_count;

	cmd_index = worker->db.cmd_index;

	cmp_count = ++worker->db.cmp_count;

	finished = __atomic_fetch_add(&g_workers_finished[cmd_index],
				1, __ATOMIC_SEQ_CST);

	// Last thread sets completion flag
	if (finished == worker->db.num_workers-1) {
		/* reset the completion counter -
		 * host will not re-use this command until
		 * we mark it complete */
		g_workers_finished[cmd_index] = 0;

		ldo_dev_complete_coll(dtctx, worker,
					cmd_index, cmp_count);
	}

	// Ready for next command
	worker->db.cmd_index = (cmd_index+1) & 
				LDO_L2M(LDO_WORKER_LOG_CMDQ_DEPTH);

	ldo_dev_poll_until_host_cmd(dtctx, worker, t_start, hangup);
}

void ldo_dev_hangup_a2a_comp(struct flexio_dev_thread_ctx *dtctx,
					struct ldo_dev_worker *worker)
{
	uint16_t cq_idx_mask;
	uint32_t num_local_comp, coll_index, coll_wraps;
	uint32_t expected_remote_atomic_count;
	uint32_t cq_idx_last_cqe;
	struct dpa_cqe64 *cqe, *cq_ring;
	uint64_t remote_cmp_flag_daddr;
        struct ldo_dev_net_worker_db *net_db =
                (struct ldo_dev_net_worker_db *)
			worker->db.net_worker_db_daddr;

	num_local_comp = worker->db.remote_ranks;
	cq_ring = (struct dpa_cqe64 *) net_db->cq_ring;
	cq_idx_mask = LDO_L2M(LDO_LOG_NET_CQ_DEPTH);
        cq_idx_last_cqe = worker->db.net_cq_idx +
				num_local_comp - 1;
        cqe = &cq_ring[cq_idx_last_cqe & cq_idx_mask];

	coll_index = worker->db.cmp_count &
				LDO_L2M(LDO_LOG_CONCURRENT_COLLS);
	coll_wraps = worker->db.cmp_count >> LDO_LOG_CONCURRENT_COLLS;

	remote_cmp_flag_daddr = worker->db.remote_cmp_flag_daddr +
                        (coll_index * sizeof(uint64_t));

	expected_remote_atomic_count = (coll_wraps+1) *
					(worker->cmd.nranks - 1);

	if (num_local_comp > 0) {
                /* we are polling the last 8B of the last CQE */
                /* advance network CQ */
                worker->db.net_cq_idx += num_local_comp;

		ldo_dev_format_trig_wod(worker,
				remote_cmp_flag_daddr,
				worker->db.worker_mkey,
				LDO_WAIT_ON_DATA_OP_EQUAL,
				0, /* inv */
				MLX5_CTRL_SEG_CE_CQE_ON_FIRST_CQE_ERROR,
				expected_remote_atomic_count,
				0xffffffffffffffff);
                /* wait until 4B containing opcode/qpn of CQE is *not* zero */
		ldo_dev_format_trig_wod(worker,
				(uint64_t) &cqe->raw_8B[7],
				worker->db.worker_mkey,
				LDO_WAIT_ON_DATA_OP_EQUAL,	
				1, /* inv */
				MLX5_CTRL_SEG_CE_CQE_ALWAYS,	
				0,
				0xffffffff00000000);
        } else {
		ldo_dev_format_trig_wod(worker,
				remote_cmp_flag_daddr,
				worker->db.worker_mkey,
				LDO_WAIT_ON_DATA_OP_EQUAL,
				0, /* inv */
				MLX5_CTRL_SEG_CE_CQE_ALWAYS,
				expected_remote_atomic_count,
				0xffffffffffffffff);
        }

        __dpa_thread_memory_writeback();

	flexio_dev_qp_sq_ring_db(dtctx, worker->db.trig_sq_pi,
					worker->db.trig_qpn);
	flexio_dev_cq_arm(dtctx, worker->db.trig_cq_idx,
					worker->db.trig_cqn);
}

void ldo_dev_activation_a2a_comp(struct flexio_dev_thread_ctx *dtcx,
					struct ldo_dev_worker *worker)
{
	ldo_dev_advance_trig_cq(worker);
}