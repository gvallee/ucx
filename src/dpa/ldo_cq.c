/**
 * Copyright (C) NVIDIA CORPORATION Ltd. 2025.  ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#include "ldo_dev.h"

/**
 * This function advances a CQ and optionally reads a CQE.
 * It should be called when the caller is sure that there is a
 * CQE to be read.
 * 
 * This function assumes that the caller will eventually call
 * a fence that will flush the value of CQ CI.
 */
int ldo_dev_advance_cq(struct flexio_dev_cqe64 *cq_ring,
			uint32_t *cq_idx,
			uint16_t cq_idx_mask,
			uint32_t *hw_owner_bit,
			uint32_t *cq_dbr,
			int wait_for_cqe)
{
	struct flexio_dev_cqe64 *cqe;
	struct dpa_mlx5_err_cqe64 *cqe_err;

	cqe = &cq_ring[*cq_idx & cq_idx_mask];

	if (wait_for_cqe) {
		while (flexio_dev_cqe_get_owner(cqe) == *hw_owner_bit);
	} else {
		if (flexio_dev_cqe_get_owner(cqe) == *hw_owner_bit)
			return 0;
	}

	if (flexio_dev_cqe_get_opcode(cqe) != 0x0) {
		cqe_err = (struct dpa_mlx5_err_cqe64 *)cqe;
		flexio_dev_print("Error type CQE! Opcode 0x%x "
				 "syndrome 0x%x vendor syndrome 0x%x "
				 "hw_err_syn 0x%x hw_syn_type 0x%x\n",
				 flexio_dev_cqe_get_opcode(cqe),
				 cqe_err->syndrome, cqe_err->vendor_err_synd,
				 cqe_err->hw_error_syndrome,
				 cqe_err->hw_syndrome_type);
		flexio_dev_thread_finish();
	}

	(*cq_idx)++;

	if (cq_dbr)
		flexio_dev_dbr_cq_set_ci(cq_dbr, *cq_idx);

	if (!(*cq_idx & cq_idx_mask))
		*hw_owner_bit = !(*hw_owner_bit);

	return 1;
}

void ldo_dev_advance_trig_cq(struct ldo_dev_worker *worker)
{
	struct ldo_dev_trig_worker_db *trig_db =
		(struct ldo_dev_trig_worker_db *)
			worker->db.trig_worker_db_daddr;

	uint32_t *cq_idx = &worker->db.trig_cq_idx;
	uint32_t *cq_dbr = &trig_db->trig_cq_dbr[0];
	uint32_t *hw_owner_bit = &worker->db.trig_cq_hw_owner_bit;
	struct flexio_dev_cqe64 *cq_ring =
		(struct flexio_dev_cqe64 *) trig_db->trig_cq_ring;

	ldo_dev_advance_cq(cq_ring,
			cq_idx,
			LDO_L2M(LDO_WORKER_LOG_CQ_DEPTH),
			hw_owner_bit,
			cq_dbr,
			1 /* wait for CQE */);

}