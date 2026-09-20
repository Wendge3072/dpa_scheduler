#include "flexio_pp_dev_utils.h"

static inline void
sch_assign_workers(struct host2dev_packet_processor_data_sch *data_from_host,
		   struct dpa_sche_context *this_sch_ctx)
{
	int i = data_from_host->sch_id;

	for (uint32_t worker_idx = 0; worker_idx < data_from_host->threads_num_per_scheduler; worker_idx++) {
		uint32_t thd_id = i * data_from_host->threads_num_per_scheduler + worker_idx;
		struct offload_dispatch_info *thd_info = &offload_info[thd_id];

		__atomic_store_n(&thd_info->assigned_queue,
				 &this_sch_ctx->queues[worker_idx],
				 __ATOMIC_RELEASE);
		__atomic_store_n(&thd_info->sch_ctx, this_sch_ctx, __ATOMIC_RELAXED);
		thd_info->wakeup_cq_num = dpa_thds_ctx[thd_id].queue.rq_cq_ctx.cq_number;
	}
}

static inline void
sch_check_workers(struct flexio_dev_thread_ctx *dtctx,
		  int sch_id,
		  uint32_t threads_num_per_scheduler)
{
	for (uint32_t worker_idx = 0; worker_idx < threads_num_per_scheduler; worker_idx++) {
		uint32_t thd_id = sch_id * threads_num_per_scheduler + worker_idx;
		struct offload_dispatch_info *thd_info = &offload_info[thd_id];
		eu_status current_status = __atomic_load_n(&thd_info->status, __ATOMIC_ACQUIRE);

		if (!thd_info->wakeup_cq_num) {
			thd_info->wakeup_cq_num = dpa_thds_ctx[thd_id].queue.rq_cq_ctx.cq_number;
		}

		if (current_status == EU_OFF && thd_info->wakeup_cq_num) {
			flexio_dev_msix_send(dtctx, thd_info->wakeup_cq_num);
			__atomic_store_n(&thd_info->status, EU_HANG, __ATOMIC_RELEASE);
		} else if (current_status == EU_FREE) {
			__atomic_store_n(&thd_info->status, EU_HANG, __ATOMIC_RELEASE);
		}
	}
}

static inline void
sch_check_budget(struct dpa_sche_context *sch_ctx, uint32_t tenants_num)
{
	for (uint32_t t = 0; t < tenants_num; t++) {
		size_t current_cycle_used = 0;
		size_t current_bw_used = 0;
		uint8_t restriction = TENANT_RESTRICT_NONE;

		if (__atomic_load_n(&sch_ctx->restrict_tenant[t], __ATOMIC_RELAXED)) {
			continue;
		}

		current_cycle_used = __atomic_load_n(&sch_ctx->tenant_cycle_consumed[t], __ATOMIC_RELAXED);
		current_bw_used = __atomic_load_n(&sch_ctx->tenant_bw_consumed[t], __ATOMIC_RELAXED);
		if (current_cycle_used >= sch_ctx->tenant_cycle_budget[t]) {
			restriction = TENANT_RESTRICT_CYCLE;
		}
		if (current_bw_used >= sch_ctx->tenant_bw_budget[t]) {
			restriction = TENANT_RESTRICT_BW;
		}
		if (restriction) {
			__atomic_store_n(&sch_ctx->restrict_tenant[t], restriction,
					 __ATOMIC_RELAXED);
		}
	}
}

static inline __attribute__((always_inline)) void
sch_cycle_record_debt(size_t budget, size_t used, size_t *debt)
{
	size_t debt_now;
	size_t debt_next;

	if (used <= budget) {
		return;
	}

	debt_now = *debt;
	debt_next = debt_now + (used - budget);
	if (debt_next < debt_now) {
		debt_next = (size_t)-1;
	}
	*debt = debt_next;
}

static inline __attribute__((always_inline)) void
sch_cycle_apply_debt(size_t *budget, size_t *debt)
{
	size_t budget_now;
	size_t debt_now = *debt;
	size_t paid;

	if (!debt_now) {
		return;
	}

	budget_now = *budget;
	paid = budget_now < debt_now ? budget_now : debt_now;
	*budget = budget_now - paid;
	*debt = debt_now - paid;
}

static inline void
sch_apply_cycle_debt(struct dpa_sche_context *sch_ctx, uint32_t tenants_num)
{
	for (uint32_t t = 0; t < tenants_num; t++) {
		sch_cycle_apply_debt(&sch_ctx->tenant_cycle_budget[t],
				     &sch_ctx->tenant_cycle_debt[t]);
	}
}

#if SCH_ROLLOVER_WORK_CONSERVING

static inline __attribute__((always_inline)) size_t
sch_budget_settle(size_t quota, size_t cap, size_t used, size_t *budget)
{
	size_t carried = *budget > used ? *budget - used : 0;
	size_t headroom = cap - quota;

	if (carried > headroom) {
		*budget = cap;
		return carried - headroom;
	}

	*budget = quota + carried;
	return 0;
}

static inline __attribute__((always_inline)) size_t
sch_budget_receive(size_t *budget, size_t cap, size_t shared_budget)
{
	size_t borrow = 0;

	if (!shared_budget || *budget >= cap) {
		return shared_budget;
	}

	borrow = cap - *budget;
	if (borrow > shared_budget) {
		borrow = shared_budget;
	}
	*budget += borrow;
	return shared_budget - borrow;
}

#if SCH_ROLLOVER_WORK_CONSERVING == SCH_ROLLOVER_MODE_BUCKET
static inline void
sch_rollover_budget(struct dpa_sche_context *sch_ctx,
		    uint32_t tenants_num)
{
	register size_t cycle_pool = 0;
	register size_t bw_pool = 0;

	for (register uint32_t t = 0; t < tenants_num; t++) {
		register size_t cycle_used = 0;
		register size_t bw_used = 0;
		register size_t cycle_budget = sch_ctx->tenant_cycle_budget[t];

		cycle_used = __atomic_exchange_n(&sch_ctx->tenant_cycle_consumed[t], 0,
						__ATOMIC_RELAXED);
		bw_used = __atomic_exchange_n(&sch_ctx->tenant_bw_consumed[t], 0,
						__ATOMIC_RELAXED);
		sch_cycle_record_debt(cycle_budget, cycle_used,
				      &sch_ctx->tenant_cycle_debt[t]);
		cycle_pool += sch_budget_settle(sch_ctx->tenant_cycle_target[t],
						sch_ctx->tenant_cycle_budget_cap[t],
						cycle_used,
						&sch_ctx->tenant_cycle_budget[t]);
		bw_pool += sch_budget_settle(sch_ctx->tenant_bw_target[t],
					     sch_ctx->tenant_bw_budget_cap[t],
					     bw_used,
					     &sch_ctx->tenant_bw_budget[t]);
#if SCH_CYCLE_USAGE_REPORT
		sch_ctx->tenant_cycle_report_used[t] += cycle_used;
#endif
#if SCH_DRF_D_REPORT
		sch_ctx->tenant_d_report_cycle_used[t] += cycle_used;
		sch_ctx->tenant_d_report_bw_used[t] += bw_used;
		sch_ctx->tenant_d_report_periods[t]++;
#endif
	}

#if SCH_CYCLE_USAGE_REPORT
	sch_ctx->tenant_cycle_report_periods++;
#endif
	for (register uint32_t t = 0; t < tenants_num; t++) {
		cycle_pool = sch_budget_receive(&sch_ctx->tenant_cycle_budget[t],
						sch_ctx->tenant_cycle_budget_cap[t],
						cycle_pool);
		bw_pool = sch_budget_receive(&sch_ctx->tenant_bw_budget[t],
					     sch_ctx->tenant_bw_budget_cap[t],
					     bw_pool);
		__atomic_store_n(&sch_ctx->restrict_tenant[t],
				 TENANT_RESTRICT_NONE, __ATOMIC_RELAXED);
	}

	sch_apply_cycle_debt(sch_ctx, tenants_num);
}
#elif SCH_ROLLOVER_WORK_CONSERVING == SCH_ROLLOVER_MODE_DRF
static inline __attribute__((always_inline)) size_t
sch_budget_reclaim_over_target(size_t *budget, size_t target)
{
	size_t excess = 0;

	if (*budget <= target) {
		return 0;
	}

	excess = *budget - target;
	*budget = target;
	return excess;
}

static inline __attribute__((always_inline)) uint64_t
sch_min_u64(uint64_t a, uint64_t b)
{
	return a < b ? a : b;
}

static inline __attribute__((always_inline)) uint64_t
sch_ratio_q20(size_t amount, size_t target)
{
	if (!amount || !target) {
		return 0;
	}

	return ((uint64_t)amount << DRF_SHIFT) / target;
}

static inline __attribute__((always_inline)) size_t
sch_drf_alloc(uint64_t delta_q20, size_t target)
{
	return (size_t)((delta_q20 * target) >> DRF_SHIFT);
}

static inline void
sch_distribute_drf_pool(struct dpa_sche_context *sch_ctx,
			uint32_t tenants_num,
			register size_t cycle_pool,
			register size_t bw_pool)
{
	register size_t cycle_target_sum = 0;
	register size_t bw_target_sum = 0;
	register uint64_t common_delta_q20 = DRF_CAP_EXTRA_Q20;

	// 判断每个租户的主导资源，根据可以借用的资源上限计算公共最大资源借用比例
	for (register uint32_t t = 0; t < tenants_num; t++) {
		register size_t room = 0;
		register size_t target = 0;
		register uint8_t tenant_restriction = sch_ctx->restrict_tenant[t];

		if (!tenant_restriction) {
			continue;
		}

		if (tenant_restriction == TENANT_RESTRICT_CYCLE) {
			target = sch_ctx->tenant_cycle_target[t];
			if (sch_ctx->tenant_cycle_budget[t] < sch_ctx->tenant_cycle_budget_cap[t]) {
				room = sch_ctx->tenant_cycle_budget_cap[t] -
				       sch_ctx->tenant_cycle_budget[t];
			}
			cycle_target_sum += target;
		} else {
			target = sch_ctx->tenant_bw_target[t];
			if (sch_ctx->tenant_bw_budget[t] < sch_ctx->tenant_bw_budget_cap[t]) {
				room = sch_ctx->tenant_bw_budget_cap[t] -
				       sch_ctx->tenant_bw_budget[t];
			}
			bw_target_sum += target;
		}

		common_delta_q20 = sch_min_u64(common_delta_q20,
					       sch_ratio_q20(room, target));
	}

	// 根据资源池中的冗余资源数量和租户的借用需求，计算公共最大资源借用比例
	if (cycle_target_sum) {
		common_delta_q20 = sch_min_u64(common_delta_q20,
					       sch_ratio_q20(cycle_pool, cycle_target_sum));
	}
	if (bw_target_sum) {
		common_delta_q20 = sch_min_u64(common_delta_q20,
					       sch_ratio_q20(bw_pool, bw_target_sum));
	}
	if (!common_delta_q20) {
		return;
	}

	for (register uint32_t t = 0; t < tenants_num; t++) {
		register size_t alloc = 0;
		register size_t room = 0;
		register uint8_t tenant_restriction = sch_ctx->restrict_tenant[t];

		(void)room;

		if (!tenant_restriction) {
			continue;
		}

		if (tenant_restriction == TENANT_RESTRICT_CYCLE) {
#if assert_debug
			if (sch_ctx->tenant_cycle_budget[t] < sch_ctx->tenant_cycle_budget_cap[t]) {
				room = sch_ctx->tenant_cycle_budget_cap[t] -
				       sch_ctx->tenant_cycle_budget[t];
			}
#endif
			alloc = sch_drf_alloc(common_delta_q20,
					      sch_ctx->tenant_cycle_target[t]);
#if assert_debug
			if (alloc > room) {
				alloc = room;
			}
			if (alloc > cycle_pool) {
				alloc = cycle_pool;
			}
#endif
			sch_ctx->tenant_cycle_budget[t] += alloc;
			cycle_pool -= alloc;
		} else {
#if assert_debug
			if (sch_ctx->tenant_bw_budget[t] < sch_ctx->tenant_bw_budget_cap[t]) {
				room = sch_ctx->tenant_bw_budget_cap[t] -
				       sch_ctx->tenant_bw_budget[t];
			}
#endif
			alloc = sch_drf_alloc(common_delta_q20,
					      sch_ctx->tenant_bw_target[t]);
#if assert_debug
			if (alloc > room) {
				alloc = room;
			}
			if (alloc > bw_pool) {
				alloc = bw_pool;
			}
#endif
			sch_ctx->tenant_bw_budget[t] += alloc;
			bw_pool -= alloc;
		}
	}
}

static inline void
sch_rollover_budget(struct dpa_sche_context *sch_ctx,
		    uint32_t tenants_num)
{
	register size_t cycle_pool = 0;
	register size_t bw_pool = 0;
	register uint32_t active_count = 0;
	register uint32_t single_active_tenant = 0;

	for (register uint32_t t = 0; t < tenants_num; t++) {
		register size_t cycle_used = 0;
		register size_t bw_used = 0;
		register size_t cycle_budget = sch_ctx->tenant_cycle_budget[t];
		register uint8_t tenant_restriction = sch_ctx->restrict_tenant[t];

		cycle_used = __atomic_exchange_n(&sch_ctx->tenant_cycle_consumed[t], 0,
						__ATOMIC_RELAXED);
		bw_used = __atomic_exchange_n(&sch_ctx->tenant_bw_consumed[t], 0,
						__ATOMIC_RELAXED);
		sch_cycle_record_debt(cycle_budget, cycle_used,
				      &sch_ctx->tenant_cycle_debt[t]);
		cycle_pool += sch_budget_settle(sch_ctx->tenant_cycle_target[t],
						sch_ctx->tenant_cycle_budget_cap[t],
						cycle_used,
						&sch_ctx->tenant_cycle_budget[t]);
		bw_pool += sch_budget_settle(sch_ctx->tenant_bw_target[t],
						sch_ctx->tenant_bw_budget_cap[t],
						bw_used,
						&sch_ctx->tenant_bw_budget[t]);
#if SCH_CYCLE_USAGE_REPORT
		sch_ctx->tenant_cycle_report_used[t] += cycle_used;
#endif
#if SCH_DRF_D_REPORT
		sch_ctx->tenant_d_report_cycle_used[t] += cycle_used;
		sch_ctx->tenant_d_report_bw_used[t] += bw_used;
		sch_ctx->tenant_d_report_periods[t]++;
#endif
		if (tenant_restriction) {
			active_count++;
			single_active_tenant = t;
			if (tenant_restriction == TENANT_RESTRICT_CYCLE) {
				cycle_pool +=
					sch_budget_reclaim_over_target(&sch_ctx->tenant_cycle_budget[t],
								       sch_ctx->tenant_cycle_target[t]);
			} else {
				bw_pool +=
					sch_budget_reclaim_over_target(&sch_ctx->tenant_bw_budget[t],
								       sch_ctx->tenant_bw_target[t]);
			}
		}
	}

#if SCH_CYCLE_USAGE_REPORT
	sch_ctx->tenant_cycle_report_periods++;
#endif
	if (active_count == 1) {
		sch_budget_receive(&sch_ctx->tenant_cycle_budget[single_active_tenant],
				   sch_ctx->tenant_cycle_budget_cap[single_active_tenant],
				   cycle_pool);
		sch_budget_receive(&sch_ctx->tenant_bw_budget[single_active_tenant],
				   sch_ctx->tenant_bw_budget_cap[single_active_tenant],
				   bw_pool);
		__atomic_store_n(&sch_ctx->restrict_tenant[single_active_tenant],
				 TENANT_RESTRICT_NONE, __ATOMIC_RELAXED);
	} else if (active_count > 1) {
		sch_distribute_drf_pool(sch_ctx, tenants_num, cycle_pool, bw_pool);
		for (register uint32_t t = 0; t < tenants_num; t++) {
			if (sch_ctx->restrict_tenant[t]) {
				__atomic_store_n(&sch_ctx->restrict_tenant[t],
						 TENANT_RESTRICT_NONE, __ATOMIC_RELAXED);
			}
		}
	}

	sch_apply_cycle_debt(sch_ctx, tenants_num);
}
#else
#error "Unsupported SCH_ROLLOVER_WORK_CONSERVING mode"
#endif
#else
static inline void
sch_rollover_budget(struct dpa_sche_context *sch_ctx,
			  uint32_t tenants_num)
{
	for (uint32_t t = 0; t < tenants_num; t++) {
		size_t cycle_budget = sch_ctx->tenant_cycle_budget[t];
		size_t cycle_used =
			__atomic_exchange_n(&sch_ctx->tenant_cycle_consumed[t], 0,
						    __ATOMIC_RELAXED);

		__atomic_exchange_n(&sch_ctx->tenant_bw_consumed[t], 0, __ATOMIC_RELAXED);
		sch_cycle_record_debt(cycle_budget, cycle_used,
				      &sch_ctx->tenant_cycle_debt[t]);
		sch_ctx->tenant_cycle_budget[t] = sch_ctx->tenant_cycle_target[t];
		sch_ctx->tenant_bw_budget[t] = sch_ctx->tenant_bw_target[t];
		__atomic_store_n(&sch_ctx->restrict_tenant[t], TENANT_RESTRICT_NONE,
				 __ATOMIC_RELAXED);
#if SCH_CYCLE_USAGE_REPORT
		sch_ctx->tenant_cycle_report_used[t] += cycle_used;
#endif
	}
#if SCH_CYCLE_USAGE_REPORT
	sch_ctx->tenant_cycle_report_periods++;
#endif
	sch_apply_cycle_debt(sch_ctx, tenants_num);
}
#endif

// report functions 
#if SCH_CYCLE_USAGE_REPORT
static inline void
sch_report_cycle_usage(struct dpa_sche_context *sch_ctx,
		       int sch_id,
		       uint32_t tenants_num)
{
	size_t report_periods = sch_ctx->tenant_cycle_report_periods ?
				sch_ctx->tenant_cycle_report_periods : 1;

	for (uint32_t t = 0; t < tenants_num; t++) {
		flexio_dev_print("sch %d cycle report: tenant %u total_used %8zu\n",
				 sch_id, t,
				 sch_ctx->tenant_cycle_report_used[t] / report_periods);
		sch_ctx->tenant_cycle_report_used[t] = 0;
	}
	sch_ctx->tenant_cycle_report_periods = 0;
}
#endif

static inline void
sch_report_tenant_packets(struct dpa_sche_context *sch_ctx,
			  int sch_id, uint32_t tenants_num)
{
	for (uint32_t t = 0; t < tenants_num; t++) {
		uint64_t forwarded =
			__atomic_exchange_n(&sch_ctx->tenant_packets_forwarded[t], 0,
					    __ATOMIC_RELAXED);
		uint64_t dropped =
			__atomic_exchange_n(&sch_ctx->tenant_packets_dropped[t], 0,
					    __ATOMIC_RELAXED);
		uint64_t bytes =
			__atomic_exchange_n(&sch_ctx->tenant_bytes_forwarded[t], 0,
					    __ATOMIC_RELAXED);

		flexio_dev_print("sch %d tenant %u traffic: forwarded=%llu dropped=%llu bytes=%llu\n",
				 sch_id, t,
				 (unsigned long long)forwarded,
				 (unsigned long long)dropped,
				 (unsigned long long)bytes);
	}
}

#if SCH_LOOP_ITER_REPORT
static inline void
sch_report_loop_iters(struct dpa_sche_context *sch_ctx, int sch_id)
{
	size_t loop_periods = sch_ctx->sched_loop_report_periods;
	size_t loop_avg = loop_periods ?
				sch_ctx->sched_loop_report_total / loop_periods : 0;

	flexio_dev_print("sch %d loop report: periods=%4zu avg=%zu\n",
				sch_id, loop_periods, loop_avg);
	sch_ctx->sched_loop_report_periods = 0;
	sch_ctx->sched_loop_report_total = 0;
}
#endif

#if SCH_ROLLOVER_COST_REPORT
static inline void
sch_report_rollover_cost(struct dpa_sche_context *sch_ctx, int sch_id)
{
	size_t rollover_periods = sch_ctx->rollover_cost_report_periods;
	size_t rollover_avg_cycles = rollover_periods ?
						sch_ctx->rollover_cost_report_total_cycles /
						rollover_periods : 0;

	flexio_dev_print("sch %d rollover report: wc=%u periods=%4zu avg_cycles=%zu\n",
				sch_id, (unsigned)SCH_ROLLOVER_WORK_CONSERVING,
				rollover_periods, rollover_avg_cycles);
	sch_ctx->rollover_cost_report_periods = 0;
	sch_ctx->rollover_cost_report_total_cycles = 0;
}
#endif

#if SCH_DRF_D_REPORT
static inline void
sch_report_drf_d(struct dpa_sche_context *sch_ctx, int sch_id, uint32_t tenants_num)
{
	for (uint32_t t = 0; t < tenants_num; t++) {
		size_t periods = sch_ctx->tenant_d_report_periods[t];
		size_t cycle_target = sch_ctx->tenant_cycle_target[t] * periods;
		size_t bw_target = sch_ctx->tenant_bw_target[t] * periods;
		size_t cycle_avg_q20 =
			sch_ratio_q20(sch_ctx->tenant_d_report_cycle_used[t], cycle_target);
		size_t bw_avg_q20 =
			sch_ratio_q20(sch_ctx->tenant_d_report_bw_used[t], bw_target);
		size_t avg_q20 = cycle_avg_q20 > bw_avg_q20 ?
				 cycle_avg_q20 : bw_avg_q20;
		size_t avg_x1000 = (avg_q20 * 1000) >> DRF_SHIFT;

		flexio_dev_print("sch %d drf D report: tenant %u periods=%4zu avg_q20=%zu avg_x1000=%zu\n",
				 sch_id, t, periods, avg_q20, avg_x1000);
		sch_ctx->tenant_d_report_periods[t] = 0;
		sch_ctx->tenant_d_report_cycle_used[t] = 0;
		sch_ctx->tenant_d_report_bw_used[t] = 0;
	}
}
#endif

flexio_dev_event_handler_t flexio_scheduler_handle;
__dpa_global__ void flexio_scheduler_handle(uint64_t thread_arg) {
	struct host2dev_packet_processor_data_sch *data_from_host = (void *)thread_arg;
	struct flexio_dev_thread_ctx *dtctx;
	register int i = data_from_host->sch_id;
	register uint32_t threads_num_per_scheduler = data_from_host->threads_num_per_scheduler;
	register uint32_t tenants_num = data_from_host->tenants_num > MAX_TENANT_NUM ?
				       MAX_TENANT_NUM : data_from_host->tenants_num;
	register uint32_t num_queues = data_from_host->num_queues;
	struct dpa_sche_context *this_sch_ctx = &(dpa_schs_ctx[i]);
	int first_run = !data_from_host->not_first_run;
	size_t time_interval = 15;
	register size_t reschedule_cycle = __dpa_thread_cycles() + time_interval * DPA_FREQ_HZ;
	register size_t next_sched_cycle = __dpa_thread_cycles() + SCHED_PERIOD_CYCLES;
#if SCH_CYCLE_USAGE_REPORT || SCH_LOOP_ITER_REPORT || SCH_ROLLOVER_COST_REPORT || SCH_DRF_D_REPORT
	register size_t next_report_cycle = __dpa_thread_cycles() + DPA_FREQ_HZ;
#endif
	size_t now_cycle = 0;

	flexio_dev_get_thread_ctx(&dtctx);

	if (first_run) {
		sch_ctx_init(dtctx, data_from_host);
		sch_assign_workers(data_from_host, this_sch_ctx);
		data_from_host->not_first_run = 1;
	}

	sch_check_workers(dtctx, i, threads_num_per_scheduler);

	while (__dpa_thread_cycles() < reschedule_cycle) {
#if SCH_LOOP_ITER_REPORT
		this_sch_ctx->sched_loop_current++;
#endif
		sch_check_workers(dtctx, i, threads_num_per_scheduler);
		sch_check_budget(this_sch_ctx, tenants_num);

		now_cycle = __dpa_thread_cycles();
		if (now_cycle >= next_sched_cycle) {
#if SCH_LOOP_ITER_REPORT
			this_sch_ctx->sched_loop_report_total += this_sch_ctx->sched_loop_current;
			this_sch_ctx->sched_loop_current = 0;
			this_sch_ctx->sched_loop_report_periods++;
#endif
#if SCH_ROLLOVER_COST_REPORT
			size_t rollover_begin = __dpa_thread_cycles();
#endif
			sch_rollover_budget(this_sch_ctx, tenants_num);
#if SCH_ROLLOVER_COST_REPORT
			this_sch_ctx->rollover_cost_report_total_cycles +=
				__dpa_thread_cycles() - rollover_begin;
			this_sch_ctx->rollover_cost_report_periods++;
#endif
			next_sched_cycle = now_cycle + SCHED_PERIOD_CYCLES;
		}

#if SCH_CYCLE_USAGE_REPORT || SCH_LOOP_ITER_REPORT || SCH_ROLLOVER_COST_REPORT || SCH_DRF_D_REPORT
		if (now_cycle >= next_report_cycle) {
#if SCH_CYCLE_USAGE_REPORT
			sch_report_cycle_usage(this_sch_ctx, i, tenants_num);
#endif
			sch_report_tenant_packets(this_sch_ctx, i, tenants_num);
#if SCH_LOOP_ITER_REPORT
			sch_report_loop_iters(this_sch_ctx, i);
#endif
#if SCH_ROLLOVER_COST_REPORT
			sch_report_rollover_cost(this_sch_ctx, i);
#endif
#if SCH_DRF_D_REPORT
			sch_report_drf_d(this_sch_ctx, i, tenants_num);
#endif
			next_report_cycle = now_cycle + DPA_FREQ_HZ;
		}
#endif
	}

	__dpa_thread_memory_writeback();
	for (uint32_t j = 0; j < num_queues; j++) {
		struct flexio_dpa_dev_queue *this_tenant = &(this_sch_ctx->queues[j]);
		flexio_dev_cq_arm(dtctx, this_tenant->rq_cq_ctx.cq_idx, this_tenant->rq_cq_ctx.cq_number);
	}
	flexio_dev_thread_reschedule();
}
