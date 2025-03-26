/**
 * Copyright (C) NVIDIA CORPORATION Ltd. 2025.  ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#include "ldo_dev.h"

/*
 * RDMA Write WQE with completion suppressed
 */
static inline void ldo_dev_format_rdma_write_no_comp(
				union flexio_dev_sqe_seg *wqe_seg,
				uint32_t sq_pi,
				uint32_t qpn,
				uint32_t rkey,
				uint64_t raddr,
				uint32_t lkey,
				uint64_t laddr,
				int msg_size)
{
	uint32_t mod = 0;
	struct flexio_dev_wqe_ctrl_seg *ctrl;

        // Control segment
        ctrl = &wqe_seg->ctrl;

        ctrl->idx_opcode =
                cpu_to_be32((mod << 24) |
                        ((sq_pi & 0xffff) << 8) | 
				MLX5_CTRL_SEG_OPCODE_RDMA_WRITE);
        ctrl->qpn_ds = cpu_to_be32((qpn << 8) | 
				LDO_MLX5_CTRL_SEG_DS_RDMA_WRITE);
        ctrl->signature_fm_ce_se = 
		cpu_to_be32(MLX5_CTRL_SEG_CE_CQE_ON_FIRST_CQE_ERROR << 2);

        // RDMA segment
        wqe_seg++;
        flexio_dev_swqe_seg_rdma_set(wqe_seg, rkey, raddr);

        // Local data segment
        wqe_seg++;
        flexio_dev_swqe_seg_mem_ptr_data_set(wqe_seg,
                                                msg_size,
                                                lkey,
                                                laddr);
}

/*
 * Fetch-Add (1) Atomic WQE with completion
 * Fetch value ignored into dump-fill mkey
 */
static inline void ldo_dev_format_rdma_atomic_comp(
				union flexio_dev_sqe_seg *wqe_seg,
				uint32_t sq_pi,
				uint32_t qpn,
				uint32_t rkey,
				uint64_t raddr,
				uint32_t dump_fill_mkey)
{
	uint32_t mod = 0;
	struct flexio_dev_wqe_ctrl_seg *ctrl;

	ctrl = &wqe_seg->ctrl;
        ctrl->idx_opcode =
                cpu_to_be32((mod << 24) |
                        ((sq_pi & 0xffff) << 8) |
				MLX5_CTRL_SEG_OPCODE_ATOMIC_FETCH_AND_ADD);
        ctrl->qpn_ds = cpu_to_be32((qpn << 8) |
				LDO_MLX5_CTRL_SEG_DS_ATOMIC_FETCH_AND_ADD);
        ctrl->signature_fm_ce_se =
		cpu_to_be32(MLX5_CTRL_SEG_CE_CQE_ALWAYS << 2);

        // RDMA segment
        wqe_seg++;
        flexio_dev_swqe_seg_rdma_set(wqe_seg, rkey, raddr);

        // Atomic segment
        wqe_seg++;
        flexio_dev_swqe_seg_atomic_set(wqe_seg, 1, 0);

        // Local data segment (fetch value ignored due to dump/fill mkey)
        wqe_seg++;
        flexio_dev_swqe_seg_mem_ptr_data_set(wqe_seg, sizeof(uint64_t),
                                        dump_fill_mkey, 0ULL);

}

void ldo_dev_format_a2a_wqes(uint64_t qp_wq_buf_base_daddr,
					uint32_t *qpn_base_daddr,
					uint32_t *atomic_rkey_base_daddr,
					uint64_t *atomic_raddr_base_daddr,
					int local_rank_index,
					int remote_rank,
					int myrank,
					int coll_index,
					uint32_t sq_pi,
					uint32_t a2a_rkey,
					uint64_t a2a_raddr,
					uint32_t a2a_lkey,
					uint64_t a2a_laddr,
					int msg_size,
					uint32_t dump_fill_lkey)
{
	uint16_t sq_idx_mask;
	uint32_t qpn;
	uint32_t atomic_rkey;
	uint64_t data_raddr, data_laddr, atomic_raddr;
	struct dpa_sqe_bb *wq_ring;
	union flexio_dev_sqe_seg *wqe_seg;

	int wq_buf_size_per_rank = 
		LDO_L2V(LDO_LOG_NET_SQ_DEPTH + LDO_LOG_SWQE_BSIZE);

	wq_ring = (struct dpa_sqe_bb *)
			(qp_wq_buf_base_daddr +
				local_rank_index * wq_buf_size_per_rank);

	sq_idx_mask = LDO_L2M(LDO_LOG_NET_SQ_DEPTH);

	wqe_seg = (union flexio_dev_sqe_seg *)
				&wq_ring[sq_pi & sq_idx_mask];

	qpn = qpn_base_daddr[local_rank_index];

	// write to remote rank with myrank index
	data_raddr = a2a_raddr + (myrank * msg_size);

	// send using remote rank index in my buffer
	data_laddr = a2a_laddr + (remote_rank * msg_size);

	ldo_dev_format_rdma_write_no_comp(wqe_seg,
				sq_pi,
				qpn,
				a2a_rkey,
				data_raddr,
				a2a_lkey,
				data_laddr,
				msg_size);

	wqe_seg += 4;

	atomic_rkey = atomic_rkey_base_daddr[local_rank_index];
	atomic_raddr = atomic_raddr_base_daddr[local_rank_index] +
				coll_index * sizeof(uint64_t);

	ldo_dev_format_rdma_atomic_comp(
				wqe_seg,
				sq_pi+1,
				qpn,
				atomic_rkey,
				atomic_raddr,
				dump_fill_lkey);
}