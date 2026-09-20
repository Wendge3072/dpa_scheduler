#include "flexio_pp_dev_utils.h"

#if WORKER_QUEUE_CYCLE_REPORT
static inline void
worker_cycle_report_reset(struct dpa_thread_context *thd_ctx)
{
	thd_ctx->queue_cycle_sum[0] = 0;
	thd_ctx->queue_pkt_count[0] = 0;
}

static inline void
worker_cycle_report_accumulate(struct dpa_thread_context *thd_ctx,
			       size_t cycle_delta)
{
	thd_ctx->queue_cycle_sum[0] += cycle_delta;
	thd_ctx->queue_pkt_count[0]++;
}

static inline void
worker_cycle_report_print(int thd_id, struct dpa_thread_context *thd_ctx)
{
	size_t pkt_count = thd_ctx->queue_pkt_count[0];
	size_t avg_cycle = pkt_count ? thd_ctx->queue_cycle_sum[0] / pkt_count : 0;

	flexio_dev_print("worker %d avg cycle per pkt: %zu pkts: %zu\n",
			 thd_id, avg_cycle, pkt_count);
}

#define WORKER_CYCLE_REPORT_RESET(_ctx) worker_cycle_report_reset((_ctx))
#define WORKER_CYCLE_REPORT_ACCUMULATE(_ctx, _delta) \
	worker_cycle_report_accumulate((_ctx), (_delta))
#define WORKER_CYCLE_REPORT_PRINT(_id, _ctx) worker_cycle_report_print((_id), (_ctx))
#else
#define WORKER_CYCLE_REPORT_RESET(_ctx)
#define WORKER_CYCLE_REPORT_ACCUMULATE(_ctx, _delta)
#define WORKER_CYCLE_REPORT_PRINT(_id, _ctx)
#endif

#define WORKER_STATS_BATCH 64

static inline __attribute__((always_inline)) void
worker_stats_flush(struct dpa_sche_context *sch_ctx,
		   uint32_t tenant_id,
		   uint32_t forwarded,
		   uint32_t dropped,
		   uint64_t bytes)
{
	if (tenant_id >= sch_ctx->tenants_num || (!forwarded && !dropped)) {
		return;
	}
	if (forwarded) {
		__atomic_fetch_add(&sch_ctx->tenant_packets_forwarded[tenant_id],
				   forwarded, __ATOMIC_RELAXED);
		__atomic_fetch_add(&sch_ctx->tenant_bytes_forwarded[tenant_id],
				   bytes, __ATOMIC_RELAXED);
	}
	if (dropped) {
		__atomic_fetch_add(&sch_ctx->tenant_packets_dropped[tenant_id],
				   dropped, __ATOMIC_RELAXED);
	}
}

#if WORKER_TX_USE_PRIVATE_SQ
#define WORKER_SELECT_TX_QUEUE(_wakeup_queue, _rq_queue, _tx_ctx, _tx_num) \
	do { \
		(_tx_ctx) = &(_wakeup_queue)->sq_ctx; \
		(_tx_num) = (_tx_ctx)->sq_number; \
	} while (0)
#else
#define WORKER_SELECT_TX_QUEUE(_wakeup_queue, _rq_queue, _tx_ctx, _tx_num) \
	do { \
		(void)(_wakeup_queue); \
		(_tx_ctx) = &(_rq_queue)->sq_ctx; \
		(_tx_num) = (_tx_ctx)->sq_number; \
	} while (0)
#endif

#define PP_DEFINE_AFFINE_WORKER_HANDLER(_name, _queue_fn, _host_buffer) \
flexio_dev_event_handler_t _name; \
__dpa_global__ void _name(uint64_t thread_arg) \
{ \
	struct host2dev_packet_processor_data_thd *data_from_host = (void *)thread_arg; \
	register int thd_id = data_from_host->thd_id; \
	struct offload_dispatch_info *thd_info = &offload_info[thd_id]; \
	struct flexio_dev_thread_ctx *dtctx; \
	struct dpa_thread_context *thd_ctx = &dpa_thds_ctx[thd_id]; \
	struct flexio_dpa_dev_queue *wakeup_queue = &thd_ctx->queue; \
	cq_ctx_t *wakeup_cq_ctx = &wakeup_queue->rq_cq_ctx; \
	struct flexio_dpa_dev_queue *rq_queues[WORKER_QUEUES_PER_THREAD]; \
	register struct flexio_dpa_dev_queue *rq_queue; \
	register struct dpa_sche_context *sch_ctx; \
	register sq_ctx_t *tx_sq_ctx; \
	register uint32_t tx_sq_number; \
	register size_t pkt_count = 0; \
	register size_t queue_cycles = 0; \
	register size_t cycle_delta = 0; \
	register uint32_t packet_size = 0; \
	uint32_t tenant_id = MAX_TENANT_NUM; \
	uint8_t forwarded = 0; \
	uint32_t stats_tenant[WORKER_QUEUES_PER_THREAD]; \
	uint32_t stats_forwarded[WORKER_QUEUES_PER_THREAD] = {0}; \
	uint32_t stats_dropped[WORKER_QUEUES_PER_THREAD] = {0}; \
	uint64_t stats_bytes[WORKER_QUEUES_PER_THREAD] = {0}; \
	\
	flexio_dev_get_thread_ctx(&dtctx); \
	com_step_cq(wakeup_cq_ctx); \
	if (!data_from_host->not_first_run) { \
		if (__atomic_load_n(&thd_info->status, __ATOMIC_ACQUIRE) == EU_OFF) { \
			__atomic_store_n(&thd_info->status, EU_FREE, __ATOMIC_RELEASE); \
		} \
		spin_on_status(thd_id, EU_HANG); \
		data_from_host->not_first_run = 1; \
	} \
	\
	sch_ctx = __atomic_load_n(&thd_info->sch_ctx, __ATOMIC_ACQUIRE); \
	for (uint32_t q = 0; q < WORKER_QUEUES_PER_THREAD; q++) { \
		stats_tenant[q] = MAX_TENANT_NUM; \
		rq_queues[q] = __atomic_load_n(&thd_info->assigned_queues[q], \
						 __ATOMIC_ACQUIRE); \
	} \
	for (uint32_t q = 0; q < WORKER_QUEUES_PER_THREAD; q++) { \
		if ((_host_buffer) && \
		    pp_queue_acquire_host_buffer(dtctx, rq_queues[q], \
					 thd_ctx->window_id)) { \
			goto worker_sleep; \
		} \
	} \
	WORKER_CYCLE_REPORT_RESET(thd_ctx); \
	\
	for (;;) { \
		for (register uint32_t q = 0; q < WORKER_QUEUES_PER_THREAD; q++) { \
			rq_queue = rq_queues[q]; \
			queue_cycles = 0; \
			WORKER_SELECT_TX_QUEUE(wakeup_queue, rq_queue, tx_sq_ctx, \
					       tx_sq_number); \
			while (queue_cycles < WORKER_QUEUE_POLL_CYCLE_LIMIT && \
			       flexio_dev_cqe_get_owner(rq_queue->rq_cq_ctx.cqe) != \
			       rq_queue->rq_cq_ctx.cq_hw_owner_bit) { \
				cycle_delta = __dpa_thread_cycles(); \
				packet_size = _queue_fn(dtctx, thd_ctx, sch_ctx, rq_queue, \
							tx_sq_ctx, tx_sq_number, \
							&tenant_id, &forwarded); \
				cycle_delta = __dpa_thread_cycles() - cycle_delta; \
				queue_cycles += cycle_delta; \
				WORKER_CYCLE_REPORT_ACCUMULATE(thd_ctx, cycle_delta); \
				if (tenant_id < sch_ctx->tenants_num) { \
					if (stats_tenant[q] != tenant_id) { \
						worker_stats_flush(sch_ctx, stats_tenant[q], \
								   stats_forwarded[q], \
								   stats_dropped[q], \
								   stats_bytes[q]); \
						stats_tenant[q] = tenant_id; \
						stats_forwarded[q] = 0; \
						stats_dropped[q] = 0; \
						stats_bytes[q] = 0; \
					} \
					if (forwarded) { \
						__atomic_fetch_add(&sch_ctx->tenant_cycle_consumed[tenant_id], \
								   cycle_delta, __ATOMIC_RELAXED); \
						__atomic_fetch_add(&sch_ctx->tenant_bw_consumed[tenant_id], \
								   packet_size, __ATOMIC_RELAXED); \
						stats_forwarded[q]++; \
						stats_bytes[q] += packet_size; \
					} else { \
						stats_dropped[q]++; \
					} \
					if (stats_forwarded[q] + stats_dropped[q] >= \
					    WORKER_STATS_BATCH) { \
						worker_stats_flush(sch_ctx, stats_tenant[q], \
								   stats_forwarded[q], \
								   stats_dropped[q], \
								   stats_bytes[q]); \
						stats_forwarded[q] = 0; \
						stats_dropped[q] = 0; \
						stats_bytes[q] = 0; \
					} \
				} \
				pkt_count++; \
				if (pkt_count >= WORKER_BATCH_SIZE) { \
					goto worker_sleep; \
				} \
			} \
		} \
	} \
	\
worker_sleep: \
	for (uint32_t q = 0; q < WORKER_QUEUES_PER_THREAD; q++) { \
		worker_stats_flush(sch_ctx, stats_tenant[q], stats_forwarded[q], \
				   stats_dropped[q], stats_bytes[q]); \
	} \
	WORKER_CYCLE_REPORT_PRINT(thd_id, thd_ctx); \
	__dpa_thread_fence(__DPA_MEMORY, __DPA_W, __DPA_W); \
	flexio_dev_cq_arm(dtctx, wakeup_cq_ctx->cq_idx, wakeup_cq_ctx->cq_number); \
	__atomic_store_n(&thd_info->status, EU_OFF, __ATOMIC_RELEASE); \
	flexio_dev_thread_reschedule(); \
}

PP_DEFINE_AFFINE_WORKER_HANDLER(flexio_pp_dev_worker,
				pp_queue_workload_affine, 0)
PP_DEFINE_AFFINE_WORKER_HANDLER(flexio_pp_dev_worker_host,
				pp_queue_workload_affine_host, 1)

#undef PP_DEFINE_AFFINE_WORKER_HANDLER
#undef WORKER_SELECT_TX_QUEUE
#undef WORKER_CYCLE_REPORT_PRINT
#undef WORKER_CYCLE_REPORT_ACCUMULATE
#undef WORKER_CYCLE_REPORT_RESET
#undef WORKER_STATS_BATCH
