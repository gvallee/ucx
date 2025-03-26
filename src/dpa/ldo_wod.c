/**
 * Copyright (C) NVIDIA CORPORATION Ltd. 2025.  ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#include "ldo_dev.h"

static inline void
format_wod_wqe(struct dpa_sqe_bb* wq_ring,
               uint16_t *sq_pi,
               uint32_t qpn,
               uint64_t addr,
               uint32_t mkey,
               uint8_t op,
               uint8_t inv,
               uint32_t ce,
               uint64_t data,
               uint64_t data_mask)
{
    uint16_t sq_idx_mask;
    union flexio_dev_sqe_seg *wqe;
    struct flexio_dev_wqe_ctrl_seg *ctrl;
    struct dpa_wqe_prm_wod_seg *wod_seg;

    sq_idx_mask = LDO_L2M(LDO_WORKER_LOG_SQ_DEPTH);
    wqe = (union flexio_dev_sqe_seg *) &wq_ring[*sq_pi & sq_idx_mask];

    // Control segment
    int ds_count = LDO_MLX5_CTRL_SEG_DS_WOD;

    ctrl = &wqe->ctrl;

    ctrl->idx_opcode = cpu_to_be32((LDO_MLX5_CTRL_SEG_OPC_MOD_WAIT_ON_DATA << 24) | 
                                   ((*sq_pi & 0xffff) << 8) | MLX5_CTRL_SEG_OPCODE_WAIT);
    ctrl->qpn_ds = cpu_to_be32((qpn << 8) | ds_count);
    ctrl->signature_fm_ce_se = cpu_to_be32(ce << 2);

    // WOD segment
    wod_seg = (struct dpa_wqe_prm_wod_seg *)(ctrl + 1);
    wod_seg->op_inv = cpu_to_be32((inv << 4) | ((op) & (0x0000000f)));
    wod_seg->mkey = cpu_to_be32(mkey);
    wod_seg->va_63_3_fail_act = cpu_to_be64(addr & (0xfffffffffffffff8)); // retry
    wod_seg->data = data;
    wod_seg->dmask = data_mask;

    (*sq_pi)++;
}

void
ldo_dev_format_trig_wod(struct ldo_dev_worker *worker,
                        uint64_t loc,
                        uint32_t lkey,
                        uint8_t op,
                        uint8_t inv,
                        uint32_t ce,
                        uint64_t data,
                        uint64_t mask)
{
    struct ldo_dev_trig_worker_db *trig_db = NULL;
    trig_db = (struct ldo_dev_trig_worker_db *) worker->db.trig_worker_db_daddr;

    format_wod_wqe((struct dpa_sqe_bb *) trig_db->trig_qp_ring,
                   &worker->db.trig_sq_pi,
                   worker->db.trig_qpn,
                   loc,
                   lkey,
                   op,
                   inv,
                   ce,
                   data,
                   mask);
}