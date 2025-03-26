/**
 * Copyright (C) NVIDIA CORPORATION Ltd. 2025.  ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#ifndef __LDO_DEV_H__
#define __LDO_DEV_H__

#include <flexio_dev.h>
#include <dpaintrin.h>
#include <flexio_dev_queue_types.h>
#include <flexio_dev_queue_access.h>
#include <flexio_dev_endianity.h>
#include <stddef.h>

#include "ldo.h"
#include "ldo_structs.h"

// Send WQE sizes (in octowords)
enum {
        LDO_MLX5_CTRL_SEG_DS_WOD                    = 0x3,
        LDO_MLX5_CTRL_SEG_DS_RDMA_WRITE             = 0x3,
        LDO_MLX5_CTRL_SEG_DS_ATOMIC_FETCH_AND_ADD   = 0x4
};

// Send WQE opcodes modifiers
enum {
    LDO_MLX5_CTRL_SEG_OPC_MOD_WAIT_ON_DATA = 0x1,
};

// wait-on-data operations
enum {
    LDO_WAIT_ON_DATA_OP_ALWAYS_TRUE     = 0x0,
    LDO_WAIT_ON_DATA_OP_EQUAL           = 0x1,
    LDO_WAIT_ON_DATA_OP_BIGGER          = 0x2,
    LDO_WAIT_ON_DATA_OP_SMALLER         = 0x3,
    LDO_WAIT_ON_DATA_OP_CYCLIC_BIGGER   = 0x4,
    LDO_WAIT_ON_DATA_OP_CYCLIC_SMALLER  = 0x5,
};

// Wait-on-Data segment PRM layout
struct dpa_wqe_prm_wod_seg {
        uint32_t op_inv;           // Compare operation
        uint32_t mkey;             // Mkey
        uint64_t va_63_3_fail_act; // Mkey's virtual address and fail operation
        uint64_t data;             // Actual data for comparison
        uint64_t dmask;            // Mask for data
};

// Error type CQE
struct dpa_mlx5_err_cqe64 {
        uint8_t         rsvd0[32];
        uint32_t        srqn;
        //uint8_t         rsvd1[18];
        uint8_t         rsvd1[16];
        uint8_t         hw_error_syndrome;
        uint8_t         hw_syndrome_type;
        uint8_t         vendor_err_synd;
        uint8_t         syndrome;
        uint32_t        s_wqe_opcode_qpn;
        uint16_t        wqe_counter;
        uint8_t         signature;
        uint8_t         op_own;
};

struct dpa_sqe_bb {
        uint8_t         raw[64];
};

struct dpa_cqe64 {
        uint64_t        raw_8B[8];
};

extern struct ldo_dev_worker *g_workers;
extern uint8_t g_workers_finished[LDO_L2V(LDO_WORKER_LOG_CMDQ_DEPTH)];

#define LDO_POLL_TIME_REMAIN(_ts)             \
        (LDO_WORKER_POLL_QUOTA_USEC > \
        (__dpa_thread_time() - (_ts)))

void ldo_dev_process_host_cmd(struct flexio_dev_thread_ctx *dtctx,
                              struct ldo_dev_worker *worker,
                              uint64_t t_start,
                              int *hangup);

void ldo_dev_process_trigger(struct flexio_dev_thread_ctx *dtctx,
                             struct ldo_dev_worker *worker,
                             uint64_t t_start,
                             int *hangup);

int ldo_dev_advance_cq(struct flexio_dev_cqe64 *cq_ring,
                       uint32_t *cq_idx /* in / out */,
                       uint16_t cq_idx_mask,
                       uint32_t *hw_owner_bit, /* in / out */
                       uint32_t *cq_dbr,
                       int wait_for_cqe);

void ldo_dev_wait_host_cmd(struct ldo_dev_worker *worker);

void ldo_dev_process_a2a_comp(struct flexio_dev_thread_ctx *dtctx,
                              struct ldo_dev_worker *worker,
                              uint64_t t_start,
                              int *hangup);

void ldo_dev_activation_host_cmd(struct flexio_dev_thread_ctx *dtcx,
                                 struct ldo_dev_worker *worker);

void ldo_dev_activation_trigger(struct flexio_dev_thread_ctx *dtcx,
                                struct ldo_dev_worker *worker);

void ldo_dev_activation_a2a_comp(struct flexio_dev_thread_ctx *dtcx,
                                 struct ldo_dev_worker *worker);

struct ldo_cmd* ldo_dev_get_cmd_ptr(struct flexio_dev_thread_ctx *dtctx,
                                    struct ldo_dev_worker *worker);

void ldo_dev_format_a2a_wqes(uint64_t qp_wq_buf_base_daddr,
                             uint32_t *qpn_base_daddr,
                             uint32_t *atomic_rkey_base_daddr,
                             uint64_t *atomic_raddr_base_daddr,
                             int local_rank_index,
                             int remote_rank,
                             int myrank,
                             int op_index,
                             uint32_t sq_pi,
                             uint32_t a2a_rkey,
                             uint64_t a2a_raddr,
                             uint32_t a2a_lkey,
                             uint64_t a2a_laddr,
                             int msg_size,
                             uint32_t dump_fill_lkey);

void ldo_dev_hangup_host_cmd(struct flexio_dev_thread_ctx *dtctx,
                             struct ldo_dev_worker *worker);

void ldo_dev_hangup_trigger(struct flexio_dev_thread_ctx *dtctx,
                            struct ldo_dev_worker *worker);

void ldo_dev_hangup_a2a_comp(struct flexio_dev_thread_ctx *dtctx,
                             struct ldo_dev_worker *worker);

void ldo_dev_advance_trig_cq(struct ldo_dev_worker *worker);

void ldo_dev_format_trig_wod(struct ldo_dev_worker *worker,
                             uint64_t loc,
                             uint32_t lkey,
                             uint8_t op,
                             uint8_t inv,
                             uint32_t ce,
                             uint64_t data,
                             uint64_t mask);

#endif // __LDO_DEV_H__