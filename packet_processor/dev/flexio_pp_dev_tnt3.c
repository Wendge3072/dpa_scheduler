#include "flexio_pp_dev_utils.h"

#if WORKER_QUEUE_CYCLE_REPORT
static inline void
worker_cycle_report_reset(struct dpa_thread_context *thd_ctx)
{
	for (uint32_t q = 0; q < WORKER_QUEUES_PER_THREAD; q++) {
		thd_ctx->queue_cycle_sum[q] = 0;
		thd_ctx->queue_pkt_count[q] = 0;
	}
}

static inline void
worker_cycle_report_accumulate(struct dpa_thread_context *thd_ctx,
			       uint32_t queue_idx,
			       size_t cycle_delta)
{
	thd_ctx->queue_cycle_sum[queue_idx] += cycle_delta;
	thd_ctx->queue_pkt_count[queue_idx]++;
}

static inline void
worker_cycle_report_print(int thd_id,
			  struct dpa_thread_context *thd_ctx)
{
	for (uint32_t q = 0; q < WORKER_QUEUES_PER_THREAD; q++) {
		size_t pkt_count = thd_ctx->queue_pkt_count[q];
		size_t avg_cycle = pkt_count ? thd_ctx->queue_cycle_sum[q] / pkt_count : 0;

		flexio_dev_print("worker %d queue %u avg cycle per pkt: %zu pkts: %zu\n",
				 thd_id, q, avg_cycle, pkt_count);
	}
}
#endif

#if WORKER_QUEUE_CYCLE_REPORT
#define WORKER_CYCLE_REPORT_ACCUMULATE(_thd_ctx, _q, _cycle_delta) \
	worker_cycle_report_accumulate((_thd_ctx), (_q), (_cycle_delta))
#else
#define WORKER_CYCLE_REPORT_ACCUMULATE(_thd_ctx, _q, _cycle_delta) \

#endif

#define WORKER_DRAIN_QUEUE(_queue_fn) \
	do { \
		while (queue_cycles < WORKER_QUEUE_POLL_CYCLE_LIMIT && \
		       flexio_dev_cqe_get_owner(rq_queue->rq_cq_ctx.cqe) != \
		       rq_queue->rq_cq_ctx.cq_hw_owner_bit) { \
			if (__atomic_load_n(restricted, __ATOMIC_RELAXED)) { \
				break; \
			} \
			cycle_delta = __dpa_thread_cycles(); \
			packet_size = _queue_fn(dtctx, thd_ctx, rq_queue, tx_sq_ctx, tx_sq_number); \
			cycle_delta = __dpa_thread_cycles() - cycle_delta; \
			queue_cycles += cycle_delta; \
			WORKER_CYCLE_REPORT_ACCUMULATE(thd_ctx, q, cycle_delta); \
			__atomic_fetch_add(&sch_ctx->tenant_cycle_consumed[q], cycle_delta, \
					   __ATOMIC_RELAXED); \
			__atomic_fetch_add(&sch_ctx->tenant_bw_consumed[q], packet_size, \
					   __ATOMIC_RELAXED); \
			pkt_count++; \
			if (pkt_count >= WORKER_BATCH_SIZE) { \
				goto worker_sleep; \
			} \
		} \
	} while (0)


// #define WORKER_DRAIN_QUEUE(_queue_fn) \
// 	do { \
// 		while (queue_cycles < WORKER_QUEUE_POLL_CYCLE_LIMIT && \
// 		       flexio_dev_cqe_get_owner(rq_queue->rq_cq_ctx.cqe) != \
// 		       rq_queue->rq_cq_ctx.cq_hw_owner_bit) { \
// 			_queue_fn(dtctx, thd_ctx, rq_queue, tx_sq_ctx, tx_sq_number); \
// 			queue_cycles += 1400; \
// 			pkt_count++; \
// 			if (pkt_count >= WORKER_BATCH_SIZE) { \
// 				goto worker_sleep; \
// 			} \
// 		} \
// 	} while (0)

flexio_dev_event_handler_t flexio_pp_dev_32;
__dpa_global__ void flexio_pp_dev_32(uint64_t thread_arg)
{	
	struct host2dev_packet_processor_data_thd *data_from_host = (void *)thread_arg;
	int i = data_from_host->thd_id;
	struct offload_dispatch_info *thd_info = &offload_info[i];
	struct flexio_dev_thread_ctx *dtctx;	
	struct dpa_thread_context *thd_ctx = &(dpa_thds_ctx[i]);
	struct flexio_dpa_dev_queue *thd_queue = &(thd_ctx->queue);
	cq_ctx_t *wakeup_cq_ctx = &(thd_queue->rq_cq_ctx);
	struct flexio_dpa_dev_queue *rq_queues[WORKER_QUEUES_PER_THREAD];
	enum pp_workload_type workload_types[WORKER_QUEUES_PER_THREAD];
	register struct dpa_sche_context *sch_ctx;
	register uint32_t tenants_num;
	register struct flexio_dpa_dev_queue *rq_queue = NULL;
	register size_t pkt_count = 0;
	register size_t cycle_delta = 0;
	register size_t queue_cycles = 0;
	register size_t packet_size = 0;
	register sq_ctx_t *tx_sq_ctx;
	register uint32_t tx_sq_number;
	register uint8_t *restricted;
	register enum pp_workload_type workload_type;


	flexio_dev_get_thread_ctx(&dtctx);
	com_step_cq(wakeup_cq_ctx);

	if(!data_from_host->not_first_run){
		if (__atomic_load_n(&thd_info->status, __ATOMIC_ACQUIRE) == EU_OFF) {
			__atomic_store_n(&thd_info->status, EU_FREE, __ATOMIC_RELEASE);
		}
		spin_on_status(i, EU_HANG);
		data_from_host->not_first_run = 1;
	}

	sch_ctx = __atomic_load_n(&thd_info->sch_ctx, __ATOMIC_RELAXED);
	tenants_num = sch_ctx->tenants_num;
	for (uint32_t t = 0; t < tenants_num; t++) {
		rq_queues[t] = __atomic_load_n(&thd_info->assigned_queues[t],
					      __ATOMIC_RELAXED);
		workload_types[t] = sch_ctx->tenant_workload_type[t];
	}
#if WORKER_QUEUE_CYCLE_REPORT
	worker_cycle_report_reset(thd_ctx);
#endif

	for (;;) {
		for (register uint32_t q = 0; q < tenants_num; q++) {
			restricted = &sch_ctx->restrict_tenant[q];
			if (__atomic_load_n(restricted, __ATOMIC_RELAXED)) {
				continue;
			}

			rq_queue = rq_queues[q];
			workload_type = workload_types[q];
			queue_cycles = 0;

			if (flexio_dev_cqe_get_owner(rq_queue->rq_cq_ctx.cqe) ==
			    rq_queue->rq_cq_ctx.cq_hw_owner_bit) {
				continue;
			}

#if WORKER_TX_USE_PRIVATE_SQ
			tx_sq_ctx = &(thd_queue->sq_ctx);
			tx_sq_number = tx_sq_ctx->sq_number;
#else
			tx_sq_ctx = &(rq_queue->sq_ctx);
			tx_sq_number = tx_sq_ctx->sq_number;
#endif

			switch (workload_type) {
			case PP_WORKLOAD_NOF:
				WORKER_DRAIN_QUEUE(pp_queue_nof);
				break;
			case PP_WORKLOAD_CHECKSUM16:
				WORKER_DRAIN_QUEUE(pp_queue_checksum16);
				break;
			case PP_WORKLOAD_CHECKSUM_NRND:
				WORKER_DRAIN_QUEUE(pp_queue_checksum_nrnd);
				break;
			case PP_WORKLOAD_L2_REFLECTOR:
			default:
				WORKER_DRAIN_QUEUE(pp_queue);
				break;
			}
		}
	}

worker_sleep:
#if WORKER_QUEUE_CYCLE_REPORT
	worker_cycle_report_print(i, thd_ctx);
#endif
	__dpa_thread_fence(__DPA_MEMORY, __DPA_W, __DPA_W);
	flexio_dev_cq_arm(dtctx, wakeup_cq_ctx->cq_idx, wakeup_cq_ctx->cq_number);
	__atomic_store_n(&thd_info->status, EU_OFF, __ATOMIC_RELEASE);
	flexio_dev_thread_reschedule();
}

// flexio_dev_event_handler_t flexio_pp_dev_32;
// __dpa_global__ void flexio_pp_dev_32(uint64_t thread_arg)
// {	
// 	struct host2dev_packet_processor_data_thd *data_from_host = (void *)thread_arg;
// 	int i = data_from_host->thd_id;
// 	struct offload_dispatch_info *thd_info = &offload_info[i];
// 	struct flexio_dev_thread_ctx *dtctx;	
// 	struct dpa_thread_context *thd_ctx = &(dpa_thds_ctx[i]);
// 	struct flexio_dpa_dev_queue *thd_queue = &(thd_ctx->queue);
// 	cq_ctx_t *wakeup_cq_ctx = &(thd_queue->rq_cq_ctx);
// 	struct flexio_dpa_dev_queue *rq_queues[WORKER_QUEUES_PER_THREAD];
// 	enum pp_workload_type workload_types[WORKER_QUEUES_PER_THREAD];
// 	register struct dpa_sche_context *sch_ctx;
// 	register struct flexio_dpa_dev_queue *rq_queue = NULL;
// 	register size_t pkt_count = 0;
// 	register size_t cycle_delta = 0;
// 	register size_t queue_cycles = 0;
// 	register size_t packet_size = 0;
// 	register sq_ctx_t *tx_sq_ctx;
// 	register uint32_t tx_sq_number;
// 	register uint8_t *restricted;
// 	register enum pp_workload_type workload_type;


// 	flexio_dev_get_thread_ctx(&dtctx);
// 	com_step_cq(wakeup_cq_ctx);

// 	if(!data_from_host->not_first_run){
// 		if (__atomic_load_n(&thd_info->status, __ATOMIC_ACQUIRE) == EU_OFF) {
// 			__atomic_store_n(&thd_info->status, EU_FREE, __ATOMIC_RELEASE);
// 		}
// 		spin_on_status(i, EU_HANG);
// 		data_from_host->not_first_run = 1;
// 	}

// 	for (uint32_t t = 0; t < WORKER_QUEUES_PER_THREAD; t++) {
// 		rq_queues[t] = __atomic_load_n(&thd_info->assigned_queues[t], __ATOMIC_RELAXED);
// 	}

// 	for (;;) {
// 		for (register uint32_t q = 0; q < WORKER_QUEUES_PER_THREAD; q++) {

// 			rq_queue = rq_queues[q];
// 			queue_cycles = 0;
// 			tx_sq_ctx = &(rq_queue->sq_ctx);
// 			tx_sq_number = tx_sq_ctx->sq_number;

// 			WORKER_DRAIN_QUEUE(pp_queue);
// 		}
// 	}

// worker_sleep:
// #if WORKER_QUEUE_CYCLE_REPORT
// 	worker_cycle_report_print(i, thd_ctx);
// #endif
// 	__dpa_thread_fence(__DPA_MEMORY, __DPA_W, __DPA_W);
// 	flexio_dev_cq_arm(dtctx, wakeup_cq_ctx->cq_idx, wakeup_cq_ctx->cq_number);
// 	__atomic_store_n(&thd_info->status, EU_OFF, __ATOMIC_RELEASE);
// 	flexio_dev_thread_reschedule();
// }

flexio_dev_event_handler_t flexio_pp_dev_32_host;
__dpa_global__ void flexio_pp_dev_32_host(uint64_t thread_arg)
{
	struct host2dev_packet_processor_data_thd *data_from_host = (void *)thread_arg;
	int i = data_from_host->thd_id;
	struct offload_dispatch_info *thd_info = &offload_info[i];
	struct flexio_dev_thread_ctx *dtctx;
	struct dpa_thread_context *thd_ctx = &(dpa_thds_ctx[i]);
	struct flexio_dpa_dev_queue *thd_queue = &(thd_ctx->queue);
	cq_ctx_t *wakeup_cq_ctx = &(thd_queue->rq_cq_ctx);
	struct flexio_dpa_dev_queue *rq_queues[WORKER_QUEUES_PER_THREAD];
	enum pp_workload_type workload_types[WORKER_QUEUES_PER_THREAD];
	register struct dpa_sche_context *sch_ctx;
	register uint32_t tenants_num;
	register struct flexio_dpa_dev_queue *rq_queue = NULL;
	register size_t pkt_count = 0;
	register size_t cycle_delta = 0;
	register size_t queue_cycles = 0;
	register size_t packet_size = 0;
	register sq_ctx_t *tx_sq_ctx;
	register uint32_t tx_sq_number;
	register uint8_t *restricted;
	register enum pp_workload_type workload_type;


	flexio_dev_get_thread_ctx(&dtctx);
	com_step_cq(wakeup_cq_ctx);

	if(!data_from_host->not_first_run){
		if (__atomic_load_n(&thd_info->status, __ATOMIC_ACQUIRE) == EU_OFF) {
			__atomic_store_n(&thd_info->status, EU_FREE, __ATOMIC_RELEASE);
		}
		spin_on_status(i, EU_HANG);
		data_from_host->not_first_run = 1;
	}

	sch_ctx = __atomic_load_n(&thd_info->sch_ctx, __ATOMIC_RELAXED);
	tenants_num = sch_ctx->tenants_num;
	for (uint32_t t = 0; t < tenants_num; t++) {
		rq_queues[t] = __atomic_load_n(&thd_info->assigned_queues[t],
					      __ATOMIC_RELAXED);
		workload_types[t] = sch_ctx->tenant_workload_type[t];
	}
#if WORKER_QUEUE_CYCLE_REPORT
	worker_cycle_report_reset(thd_ctx);
#endif

	for (uint32_t q = 0; q < tenants_num; q++) {
		if (pp_queue_acquire_host_buffer(dtctx, rq_queues[q], thd_ctx->window_id)) {
			goto worker_sleep;
		}
	}

	for (;;) {
		for (register uint32_t q = 0; q < tenants_num; q++) {
			restricted = &sch_ctx->restrict_tenant[q];
			if (__atomic_load_n(restricted, __ATOMIC_RELAXED)) {
				continue;
			}

			rq_queue = rq_queues[q];
			workload_type = workload_types[q];
			queue_cycles = 0;

			if (flexio_dev_cqe_get_owner(rq_queue->rq_cq_ctx.cqe) ==
			    rq_queue->rq_cq_ctx.cq_hw_owner_bit) {
				continue;
			}

#if WORKER_TX_USE_PRIVATE_SQ
			tx_sq_ctx = &(thd_queue->sq_ctx);
			tx_sq_number = tx_sq_ctx->sq_number;
#else
			tx_sq_ctx = &(rq_queue->sq_ctx);
			tx_sq_number = tx_sq_ctx->sq_number;
#endif

			switch (workload_type) {
			case PP_WORKLOAD_NOF:
				WORKER_DRAIN_QUEUE(pp_queue_nof_host);
				break;
			case PP_WORKLOAD_CHECKSUM16:
				WORKER_DRAIN_QUEUE(pp_queue_checksum16_host);
				break;
			case PP_WORKLOAD_CHECKSUM_NRND:
				WORKER_DRAIN_QUEUE(pp_queue_checksum_nrnd_host);
				break;
			case PP_WORKLOAD_L2_REFLECTOR:
			default:
				WORKER_DRAIN_QUEUE(pp_queue_host);
				break;
			}
		}
	}

worker_sleep:
#if WORKER_QUEUE_CYCLE_REPORT
	worker_cycle_report_print(i, thd_ctx);
#endif
	__dpa_thread_fence(__DPA_MEMORY, __DPA_W, __DPA_W);
	flexio_dev_cq_arm(dtctx, wakeup_cq_ctx->cq_idx, wakeup_cq_ctx->cq_number);
	__atomic_store_n(&thd_info->status, EU_OFF, __ATOMIC_RELEASE);
	flexio_dev_thread_reschedule();
}

#undef WORKER_DRAIN_QUEUE
#undef WORKER_CYCLE_REPORT_ACCUMULATE
