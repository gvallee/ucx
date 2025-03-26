/**
 * Copyright (C) NVIDIA CORPORATION Ltd. 2025.  ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#include "ldo_dev.h"

static inline void
ldo_dev_alltoall(struct flexio_dev_thread_ctx *dtctx,
                 struct ldo_dev_worker *worker,
                 int op_index)
{
    uint16_t cq_idx_mask;
    uint32_t qpn, cq_idx_last_cqe;
    struct dpa_cqe64 *cqe, *cq_ring;
    int i, remote_rank, start_rank, end_rank, rank_count;
    struct ldo_dev_net_worker_db *net_db = NULL;

    net_db = (struct ldo_dev_net_worker_db *) worker->db.net_worker_db_daddr;

    rank_count = worker->db.rank_count;
    start_rank = worker->db.start_rank;
    end_rank = start_rank + rank_count;

    for(i = 0, remote_rank = start_rank; remote_rank < end_rank; remote_rank++, i++)
    {
        qpn = net_db->qpn[i];
        if (qpn == -1) // self
            continue;
        ldo_dev_format_a2a_wqes(worker->db.net_qp_wq_buf_daddr,
                                net_db->qpn,
                                net_db->remote_cmp_mkey,
                                net_db->remote_cmp_daddr,
                                i,
                                remote_rank,
                                worker->cmd.my_rank,
                                op_index,
                                worker->db.g_op_sq_pi,
                                worker->cmd.recvbuf_rkey,
                                worker->cmd.recvbuf,
                                worker->cmd.sendbuf_lkey,
                                worker->cmd.sendbuf,
                                worker->cmd.msg_size,
                                worker->db.dump_fill_mkey);
    }

    worker->db.g_op_sq_pi += 2;

    cq_idx_mask = LDO_L2M(LDO_LOG_NET_CQ_DEPTH);
    cq_idx_last_cqe = worker->db.net_cq_idx + i;
    cq_ring = (struct dpa_cqe64 *) net_db->cq_ring;
    cqe = &cq_ring[cq_idx_last_cqe & cq_idx_mask];

    /* Reset the Opcode / QPN field of the CQE 
     * On completion the opcode (which will not be zero) will
     * be written here. This will be used by WOD to detect
     * when a new CQE is available. Using hw_ownership bit
     * in WOD is problematic, due to the fact that WOD works
     * at a byte granularity and it is cumbersome to predict
     * the value at the byte containing hw_ownership bit.
     */
    ((struct dpa_mlx5_err_cqe64 *) cqe)->s_wqe_opcode_qpn = 0;

    __dpa_thread_memory_writeback();

    for(i = 0; i < rank_count; i++)
    {
        qpn = net_db->qpn[i];
        if (qpn == -1) // self
                continue;
        flexio_dev_qp_sq_ring_db(dtctx, worker->db.g_op_sq_pi, qpn);
    }
}

static inline void
ldo_dev_poll_until_a2a_comp(struct flexio_dev_thread_ctx *dtctx,
                            struct ldo_dev_worker *worker,
                            int op_index,
                            uint32_t op_wraps,
                            uint64_t t_start,
                            int *hangup)
{
    uint32_t *cq_idx, *cq_dbr, *hw_owner_bit;
    int done, found_one, total_found, remote_cmp_done;
    struct flexio_dev_cqe64 *cqe, *cq_ring;
    struct ldo_dev_net_worker_db *net_db;
    volatile uint64_t *remote_cmp_flag_daddr;
    uint32_t expected_remote_atomic_count;

    done = found_one = total_found = remote_cmp_done = 0;

    net_db = (struct ldo_dev_net_worker_db *)
            worker->db.net_worker_db_daddr;

    cq_ring = (struct flexio_dev_cqe64 *) net_db->cq_ring;

    cq_idx = &worker->db.net_cq_idx;
    hw_owner_bit = &worker->db.net_cq_hw_owner_bit;

    remote_cmp_flag_daddr = (volatile uint64_t *)
        (worker->db.remote_cmp_flag_daddr + 
                (op_index * sizeof(uint64_t)));

    expected_remote_atomic_count = (op_wraps+1) *
            (worker->cmd.nranks - 1);

    while(LDO_POLL_TIME_REMAIN(t_start)) {
        /* reap send side completions */
        found_one = ldo_dev_advance_cq(
                    cq_ring,
                    cq_idx,
                    LDO_L2M(LDO_LOG_NET_CQ_DEPTH),
                    hw_owner_bit,
                    NULL,
                    0);

        total_found += found_one;

        /* check remote completions */
        if (*remote_cmp_flag_daddr == expected_remote_atomic_count) {
            remote_cmp_done = 1;
        }

        if(total_found == worker->db.remote_ranks &&
                remote_cmp_done) {
            done = 1;
            break;
        }

    }

    if (!done)
        *hangup = 1;
}

void
ldo_dev_process_trigger(struct flexio_dev_thread_ctx *dtctx,
                        struct ldo_dev_worker *worker,
                        uint64_t t_start,
                        int *hangup)
{
    int op_index = worker->db.cmp_count & LDO_L2M(LDO_LOG_CONCURRENT_COLLS);
    uint32_t op_wraps = worker->db.cmp_count >> LDO_LOG_CONCURRENT_COLLS;

    ldo_dev_alltoall(dtctx, worker, op_index);

    ldo_dev_poll_until_a2a_comp(dtctx,
                                worker,
                                op_index,
                                op_wraps,
                                t_start,
                                hangup);
}

void
ldo_dev_hangup_trigger(struct flexio_dev_thread_ctx *dtctx,
                       struct ldo_dev_worker *worker)
{
    ldo_dev_format_trig_wod(worker,
                            worker->cmd.trigger_loc,
                            worker->cmd.trigger_lkey,
                            LDO_WAIT_ON_DATA_OP_BIGGER,
                            1, /* inv */
                            MLX5_CTRL_SEG_CE_CQE_ALWAYS,	
                            cpu_to_be64(worker->cmd.trigger_threshold),
                            0xffffffffffffffff);

    __dpa_thread_memory_writeback();

    flexio_dev_qp_sq_ring_db(dtctx,
                             worker->db.trig_sq_pi,
                             worker->db.trig_qpn);
    flexio_dev_cq_arm(dtctx,
                      worker->db.trig_cq_idx,
                      worker->db.trig_cqn);
}

void
ldo_dev_activation_trigger(struct flexio_dev_thread_ctx *dtcx,
                           struct ldo_dev_worker *worker)
{
    ldo_dev_advance_trig_cq(worker);
}