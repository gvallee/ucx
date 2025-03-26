/**
 * Copyright (C) NVIDIA CORPORATION Ltd. 2025.  ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#include "ldo_dev.h"

struct ldo_dev_worker *g_workers;
uint8_t g_workers_finished[LDO_L2V(LDO_WORKER_LOG_CMDQ_DEPTH)];

__dpa_rpc__ uint64_t ldo_dev_worker_setup(uint64_t worker_daddr,
			int num_workers)
{
	int i;
	union flexio_dev_sqe_seg *swqe;
	struct flexio_dev_thread_ctx *dtctx;
	struct ldo_dev_net_worker_db *net_db;

	flexio_dev_get_thread_ctx(&dtctx);

	g_workers = (struct ldo_dev_worker *) worker_daddr;

	/* Post initial WOD WQE and ARM CQs */
	for(i = 0; i < num_workers; i++) {
		ldo_dev_wait_host_cmd(&g_workers[i]);
	}

	__dpa_thread_memory_writeback();

	for(i = 0; i < num_workers; i++) {
		flexio_dev_qp_sq_ring_db(dtctx, g_workers[i].db.trig_sq_pi,
			g_workers[i].db.trig_qpn);
		flexio_dev_cq_arm(dtctx, 0, g_workers[i].db.trig_cqn);
	}

	for(i = 0; i < LDO_L2V(LDO_WORKER_LOG_CMDQ_DEPTH); i++)
		g_workers_finished[i] = 0;

	for(i = 0; i < num_workers; i++) {
		net_db = (struct ldo_dev_net_worker_db *)
				g_workers[i].db.net_worker_db_daddr;
	}

	flexio_dev_print("Welcome to the LDO thread infrastructure running on the DPA!\n");

	return 0;
}

static inline void ldo_dev_progress_state(struct flexio_dev_thread_ctx *dtctx,
					struct ldo_dev_worker *worker,
					int worker_rank,
					int state,
					uint64_t t_start,
					int *next_state,
					int *hangup)
{
	int ready;

	switch (state) {
	case LDO_WORKER_WAIT_HOST_CMD:

		ldo_dev_process_host_cmd(dtctx, worker,
						t_start, hangup);
		*next_state = LDO_WORKER_WAIT_TRIGGER;

		break;

	case LDO_WORKER_WAIT_TRIGGER:

		ldo_dev_process_trigger(dtctx, worker,
						t_start, hangup);

		*next_state = LDO_WORKER_WAIT_A2A_COMP;
		break;

	case LDO_WORKER_WAIT_A2A_COMP:

		ldo_dev_process_a2a_comp(dtctx, worker,
						t_start, hangup);
		*next_state = LDO_WORKER_WAIT_HOST_CMD;
		break;

	default:
		flexio_dev_print("[%s] Worker in erroneous state!\n",
					__func__);
		flexio_dev_thread_finish();
	}
}

static inline void ldo_dev_hangup(
			struct flexio_dev_thread_ctx *dtctx,
			struct ldo_dev_worker *worker,
			int state)
{
	switch(state) {
	case LDO_WORKER_WAIT_HOST_CMD:
		ldo_dev_hangup_host_cmd(dtctx, worker);
		break;
	case LDO_WORKER_WAIT_TRIGGER:
		ldo_dev_hangup_trigger(dtctx, worker);
		break;
	case LDO_WORKER_WAIT_A2A_COMP:
		ldo_dev_hangup_a2a_comp(dtctx, worker);
		break;
	default:
		flexio_dev_print("[%s] Worker in erroneous state!\n",
					__func__);
		flexio_dev_thread_finish();
	}

}

static inline void ldo_dev_activation(
			struct flexio_dev_thread_ctx *dtctx,
			struct ldo_dev_worker *worker,
			int state)
{
	switch (state) {
	case LDO_WORKER_WAIT_HOST_CMD:

		ldo_dev_activation_host_cmd(dtctx, worker);
		break;

	case LDO_WORKER_WAIT_TRIGGER:

		ldo_dev_activation_trigger(dtctx, worker);
		break;

	case LDO_WORKER_WAIT_A2A_COMP:

		ldo_dev_activation_a2a_comp(dtctx, worker);
		break;

	default:
		flexio_dev_print("[%s] Worker in erroneous state!\n",
					__func__);
		flexio_dev_thread_finish();
	}
}

__dpa_global__ void ldo_dev_worker(uint64_t worker_rank)
{
	int state, next_state;
	int hangup = 0, quit = 0;
	uint64_t t_start, t_remain;
	struct flexio_dev_thread_ctx *dtctx;
	struct ldo_dev_worker worker;

	flexio_dev_print("I am a new worker running on the DPA, i started after receiving a command from the host!\n");

	t_start = __dpa_thread_time();

	flexio_dev_get_thread_ctx(&dtctx);

	/* load state from heap */
	worker = g_workers[worker_rank];

	next_state = worker.db.worker_state;

	/* process event that activated the thread */
	ldo_dev_activation(dtctx, &worker, next_state);

	do {
		state = next_state;
		ldo_dev_progress_state(dtctx, &worker, worker_rank,
					state, t_start, &next_state, &hangup);
	} while(!hangup);

	ldo_dev_hangup(dtctx, &worker, next_state);

	/* store state into heap */
	worker.db.worker_state = next_state;
	g_workers[worker_rank] = worker;

	flexio_dev_thread_reschedule();
}