#ifndef __FLEXIO_PP_DEV_UTILS_H__
#define __FLEXIO_PP_DEV_UTILS_H__

#include "com_dev.h"
// #include "../../common/dev/com_dev.h"
#include <libflexio-dev/flexio_dev_err.h>
#include <libflexio-dev/flexio_dev_queue_access.h>
#include <libflexio-dev/flexio_dev_debug.h>
#include <libflexio-libc/string.h>
#include <stddef.h>
#include <dpaintrin.h>
/* Shared header file for packet processor sample */
#include "../flexio_pp_com.h"

/*
0 表示使用调度器分配的队列 ✅
1 表示使用线程私有队列 ❌
*/
#define WORKER_TX_USE_PRIVATE_SQ 0

/*
worker 线程每 reschedule 汇报数据包平均处理cycle的开关
*/
#define WORKER_QUEUE_CYCLE_REPORT 0

/*
sch 线程每秒钟汇报租户 cycle 使用量的开关
*/
#define SCH_CYCLE_USAGE_REPORT 1

/* 
sch 线程每秒钟汇报 设置资源预算 开销的开关
*/
#define SCH_ROLLOVER_COST_REPORT 0

/* 
sch 线程每秒钟汇报每调度周期“资源检查”循环迭代次数的开关
*/
#define SCH_LOOP_ITER_REPORT 0
/*
sch 线程每秒钟汇报租户实际主导资源使用比例 D 的开关
*/
#define SCH_DRF_D_REPORT 0

/*
sch 线程 rollover 模式切换开关
0: 关闭 work-conserving
1: 仅启用溢出桶借用
2: 启用溢出桶借用 + DRF 重分配
*/
#define SCH_ROLLOVER_MODE_OFF 0
#define SCH_ROLLOVER_MODE_BUCKET 1
#define SCH_ROLLOVER_MODE_DRF 2
#ifndef SCH_ROLLOVER_WORK_CONSERVING
#define SCH_ROLLOVER_WORK_CONSERVING SCH_ROLLOVER_MODE_BUCKET
#endif

#define assert_debug 0

// #define DEFAULT_LINK_BANDWIDTH_BPS (45 * 1000000000ULL)
#define DEFAULT_LINK_BANDWIDTH_BPS (90 * 1000000000ULL)
#define MAX_CYCLE_PERCENTAGE_DPA_HEAP 7800
#define MAX_CYCLE_PERCENTAGE_DPU_MEM 8483
#define MAX_CYCLE_TOTAL 10000
#define WORKER_BATCH_SIZE 1048576UL
#define WORKER_QUEUE_POLL_CYCLE_LIMIT 148480UL
#define SCHED_PERIOD_CYCLES (DPA_FREQ_HZ / 1000)
#define WC_BUDGET_CAP_NUM 2
#define WC_BUDGET_CAP_DEN 1
#define DRF_SHIFT 20
#define DRF_CAP_EXTRA_Q20 \
	(WC_BUDGET_CAP_NUM > WC_BUDGET_CAP_DEN ? \
	 ((((uint64_t)WC_BUDGET_CAP_NUM - WC_BUDGET_CAP_DEN) << DRF_SHIFT) / WC_BUDGET_CAP_DEN) : 0)

#define TENANT_RESTRICT_NONE 0
#define TENANT_RESTRICT_CYCLE 1
#define TENANT_RESTRICT_BW 2

#define PP_WORKLOAD_L2_REFLECTOR_ID 0
#define PP_WORKLOAD_CHECKSUM16_ID 1
#define PP_WORKLOAD_CHECKSUM_NRND_ID 2
#define PP_WORKLOAD_NOF_ID 3

static uint32_t cycle_weights[MAX_TENANT_NUM];
static uint32_t bandwidth_weights[MAX_TENANT_NUM];

enum pp_workload_type {
	PP_WORKLOAD_L2_REFLECTOR = PP_WORKLOAD_L2_REFLECTOR_ID,
	PP_WORKLOAD_CHECKSUM16 = PP_WORKLOAD_CHECKSUM16_ID,
	PP_WORKLOAD_CHECKSUM_NRND = PP_WORKLOAD_CHECKSUM_NRND_ID,
	PP_WORKLOAD_NOF = PP_WORKLOAD_NOF_ID,
};

#ifndef PP_WORKER_WORKLOAD_TYPE
#define PP_WORKER_WORKLOAD_TYPE PP_WORKLOAD_L2_REFLECTOR_ID
#endif

#ifndef PP_WORKLOAD_CHECKSUM_ROUNDS
#define PP_WORKLOAD_CHECKSUM_ROUNDS 1
#endif

#define PP_MAC_SWAP_MASK 0x0000ffffffffffffULL

struct flexio_dpa_dev_queue {
	/* lkey - local memory key */
	uint32_t sq_lkey;
	uint32_t rq_lkey;
	cq_ctx_t rq_cq_ctx;     /* RQ CQ */
	rq_ctx_t rq_ctx;        /* RQ */
	sq_ctx_t sq_ctx;        /* SQ */
	cq_ctx_t sq_cq_ctx;     /* SQ CQ */
	dt_ctx_t dt_ctx;        /* SQ Data ring */
};

/* The structure of the sample DPA application contains global data that the application uses */
struct dpa_thread_context {
	/* Packet count - used for debug message */
	uint64_t packets_count;
	int buffer_location;
	uint32_t window_id;
	uint32_t idx;
	// NVMe related
	flexio_uintptr_t host_buffer;
	flexio_uintptr_t result;
	uint8_t restrict_probe_shadow[WORKER_QUEUES_PER_THREAD];
#if WORKER_QUEUE_CYCLE_REPORT
	size_t queue_cycle_sum[WORKER_QUEUES_PER_THREAD];
	size_t queue_pkt_count[WORKER_QUEUES_PER_THREAD];
#endif
	struct flexio_dpa_dev_queue queue;
};

/* The structure of the sample DPA application contains global data that the application uses */
struct dpa_sche_context {
	/* Packet count - used for debug message */
	uint64_t packets_count;
	int buffer_location;
	uint32_t window_id;
	uint32_t idx;
	struct flexio_dpa_dev_queue queues[MAX_SCHEDULER_QUEUES];
	size_t tenant_cycle_target[MAX_TENANT_NUM];
	size_t tenant_cycle_consumed[MAX_TENANT_NUM];
	size_t tenant_bw_target[MAX_TENANT_NUM];
	size_t tenant_bw_consumed[MAX_TENANT_NUM];
	size_t tenant_cycle_budget[MAX_TENANT_NUM];
	size_t tenant_cycle_budget_cap[MAX_TENANT_NUM];
	size_t tenant_bw_budget[MAX_TENANT_NUM];
	size_t tenant_bw_budget_cap[MAX_TENANT_NUM];
	size_t tenant_cycle_debt[MAX_TENANT_NUM];
	uint8_t restrict_tenant[MAX_TENANT_NUM];
	uint64_t tenant_packets_forwarded[MAX_TENANT_NUM];
	uint64_t tenant_packets_dropped[MAX_TENANT_NUM];
	uint64_t tenant_bytes_forwarded[MAX_TENANT_NUM];
	uint64_t dmac_base;
	uint32_t tenants_num;
	uint32_t tenant_shards;
#if SCH_CYCLE_USAGE_REPORT
	size_t tenant_cycle_report_used[MAX_TENANT_NUM];
	size_t tenant_cycle_report_periods;
#endif
#if SCH_LOOP_ITER_REPORT
	size_t sched_loop_current;
	size_t sched_loop_report_periods;
	size_t sched_loop_report_total;
#endif
#if SCH_ROLLOVER_COST_REPORT
	size_t rollover_cost_report_periods;
	size_t rollover_cost_report_total_cycles;
#endif
#if SCH_DRF_D_REPORT
	size_t tenant_d_report_periods[MAX_TENANT_NUM];
	size_t tenant_d_report_cycle_used[MAX_TENANT_NUM];
	size_t tenant_d_report_bw_used[MAX_TENANT_NUM];
#endif
};

typedef uint8_t eu_status;

enum {
	EU_OFF  = 0,
    EU_FREE = 1,
    EU_HANG = 2,
};

struct offload_dispatch_info {
	struct flexio_dpa_dev_queue *assigned_queue;
	struct dpa_sche_context *sch_ctx;
	uint32_t wakeup_cq_num;
	eu_status status;
};

extern struct dpa_thread_context dpa_thds_ctx[190];
extern struct dpa_sche_context dpa_schs_ctx[32];
extern struct offload_dispatch_info offload_info[190];

void spin_on_status(uint16_t thd_id, eu_status expected_status);
void sch_ctx_init(struct flexio_dev_thread_ctx *dtctx,
             struct host2dev_packet_processor_data_sch *data_from_host);
void sch_apply_qos_update(struct host2dev_qos_update *qos_update);

extern flexio_dev_rpc_handler_t qos_update;
__dpa_rpc__ uint64_t qos_update(uint64_t data);

static inline __attribute__((always_inline)) int
pp_queue_acquire_host_buffer(struct flexio_dev_thread_ctx *dtctx,
			     struct flexio_dpa_dev_queue *queue,
			     uint32_t window_id)
{
	flexio_dev_status_t ret;

	ret = flexio_dev_window_config(dtctx, (uint16_t)window_id, queue->rq_lkey);
	if (ret != FLEXIO_DEV_STATUS_SUCCESS) {
		flexio_dev_print("failed to config host rq window\n");
		return -1;
	}
	ret = flexio_dev_window_ptr_acquire(dtctx,
					    (uint64_t)queue->rq_ctx.rqd_host_addr,
					    &(queue->rq_ctx.rqd_dpa_addr));
	if (ret != FLEXIO_DEV_STATUS_SUCCESS) {
		flexio_dev_print("failed to acquire host rq buffer\n");
		return -1;
	}

	if (queue->sq_lkey != queue->rq_lkey) {
		ret = flexio_dev_window_config(dtctx, (uint16_t)window_id, queue->sq_lkey);
		if (ret != FLEXIO_DEV_STATUS_SUCCESS) {
			flexio_dev_print("failed to config host sq window\n");
			return -1;
		}
	}
	ret = flexio_dev_window_ptr_acquire(dtctx,
					    (uint64_t)queue->sq_ctx.sqd_host_addr,
					    &(queue->sq_ctx.sqd_dpa_addr));
	if (ret != FLEXIO_DEV_STATUS_SUCCESS) {
		flexio_dev_print("failed to acquire host sq buffer\n");
		return -1;
	}

	return 0;
}

static inline __attribute__((always_inline)) uint32_t
pp_get_packet_size(struct flexio_dpa_dev_queue *rq_queue)
{
	return be32_to_cpu((volatile __be32)rq_queue->rq_cq_ctx.cqe->byte_cnt);
}

static inline __attribute__((always_inline)) uint32_t
pp_payload_size(uint32_t packet_size)
{
	return packet_size > ETH_HEADER_SIZE ? packet_size - ETH_HEADER_SIZE : 0;
}

static inline __attribute__((always_inline)) uint32_t
pp_aligned_payload_offset(uint32_t alignment)
{
	register uint32_t offset = ETH_HEADER_SIZE;
	register uint32_t misalignment = offset & (alignment - 1);

	if (misalignment) {
		offset += alignment - misalignment;
	}

	return offset;
}

static inline __attribute__((always_inline)) void
pp_swap_mac_fast(char *packet)
{
	register uint64_t src_mac = *((uint64_t *)packet);
	register uint64_t dst_mac = *((uint64_t *)(packet + 6));

	*((uint64_t *)packet) = dst_mac;
	*((uint64_t *)(packet + 6)) = (src_mac & PP_MAC_SWAP_MASK) |
				      (dst_mac & ~PP_MAC_SWAP_MASK);
}

static inline __attribute__((always_inline)) void
pp_workload_l2_reflector(char *packet, uint32_t packet_size)
{
	(void)packet_size;

	pp_swap_mac_fast(packet);
}

static inline __attribute__((always_inline)) void
pp_workload_checksum16(char *packet, uint32_t packet_size)
{
	register uint32_t payload_size;
	register uint16_t *payload;
	volatile uint16_t checksum;

	pp_swap_mac_fast(packet);

	payload_size = pp_payload_size(packet_size);
	if (payload_size < sizeof(uint16_t)) {
		return;
	}

	payload = (uint16_t *)(packet + ETH_HEADER_SIZE);
	checksum = calculate_checksum(payload, payload_size / sizeof(uint16_t));
	// payload[0] = checksum;
	(void) checksum;
}

static inline __attribute__((always_inline)) void
pp_workload_checksum_nrnd(char *packet, uint32_t packet_size)
{
	register uint32_t payload_offset;
	register uint32_t payload_size;
	register uint_test *payload;
	volatile uint_test checksum;

	pp_swap_mac_fast(packet);

	payload_offset = pp_aligned_payload_offset(sizeof(uint_test));
	if (packet_size <= payload_offset) {
		return;
	}

	payload_size = packet_size - payload_offset;
	if (payload_size < sizeof(uint_test)) {
		return;
	}

	payload = (uint_test *)(packet + payload_offset);
	checksum = calculate_checksum_nrnd(payload,
					   payload_size / sizeof(uint_test),
					   PP_WORKLOAD_CHECKSUM_ROUNDS);
	// payload[0] = checksum;
	(void) checksum;
}

static inline __attribute__((always_inline)) void
pp_workload_nof(struct dpa_thread_context *thd_ctx, char *packet, uint32_t packet_size)
{
	register uint32_t payload_size;
	register uint32_t copy_size;
	register flexio_uintptr_t dst;

	pp_swap_mac_fast(packet);

	if (!thd_ctx->host_buffer) {
		return;
	}

	payload_size = pp_payload_size(packet_size);
	if (!payload_size) {
		return;
	}

	copy_size = payload_size > NVME_QUEUE_ENTRY_SIZE ? NVME_QUEUE_ENTRY_SIZE : payload_size;
	copy_size /= 2;
	dst = thd_ctx->host_buffer +
	      (thd_ctx->idx % NVME_QUEUE_ENTRY_NUM) * NVME_QUEUE_ENTRY_SIZE;
	memcpy((void *)dst, packet + ETH_HEADER_SIZE, copy_size);
	thd_ctx->idx++;
	__dpa_thread_window_writeback();
}

static inline __attribute__((always_inline)) uint64_t
pp_read_dmac(const char *packet)
{
	const uint8_t *mac = (const uint8_t *)packet;

	return ((uint64_t)mac[0] << 40) |
	       ((uint64_t)mac[1] << 32) |
	       ((uint64_t)mac[2] << 24) |
	       ((uint64_t)mac[3] << 16) |
	       ((uint64_t)mac[4] << 8) |
	       (uint64_t)mac[5];
}

static inline __attribute__((always_inline)) uint32_t
pp_decode_tenant(const struct dpa_sche_context *sch_ctx,
		 const char *packet, uint32_t packet_size)
{
	register uint64_t dmac;
	register uint64_t offset;
	register uint64_t binding_count;

	if (packet_size < 6 || !sch_ctx->tenant_shards || !sch_ctx->tenants_num) {
		return MAX_TENANT_NUM;
	}

	dmac = pp_read_dmac(packet);
	if (dmac < sch_ctx->dmac_base) {
		return MAX_TENANT_NUM;
	}
	offset = dmac - sch_ctx->dmac_base;
	binding_count = (uint64_t)sch_ctx->tenants_num * sch_ctx->tenant_shards;
	if (offset >= binding_count) {
		return MAX_TENANT_NUM;
	}

	return (uint32_t)(offset / sch_ctx->tenant_shards);
}

/*
 * The workload is selected at build time. A worker never branches on workload
 * while polling, so tenant multiplexing cannot trigger a workload switch.
 */
static inline __attribute__((always_inline)) void
pp_apply_worker_workload(struct dpa_thread_context *thd_ctx,
			 char *packet, uint32_t packet_size)
{
#if PP_WORKER_WORKLOAD_TYPE == PP_WORKLOAD_NOF_ID
	pp_workload_nof(thd_ctx, packet, packet_size);
#elif PP_WORKER_WORKLOAD_TYPE == PP_WORKLOAD_CHECKSUM16_ID
	pp_workload_checksum16(packet, packet_size);
#elif PP_WORKER_WORKLOAD_TYPE == PP_WORKLOAD_CHECKSUM_NRND_ID
	pp_workload_checksum_nrnd(packet, packet_size);
#else
	(void)thd_ctx;
	pp_workload_l2_reflector(packet, packet_size);
#endif
}

#define PP_DEFINE_AFFINE_QUEUE(_name, _host_buffer) \
static inline __attribute__((always_inline)) uint32_t \
_name(struct flexio_dev_thread_ctx *dtctx, \
      struct dpa_thread_context *thd_ctx, \
      struct dpa_sche_context *sch_ctx, \
      struct flexio_dpa_dev_queue *rq_queue, \
      sq_ctx_t *tx_sq_ctx, \
      uint32_t tx_sq_number, \
      uint32_t *tenant_id, \
      uint8_t *forwarded) \
{ \
	register cq_ctx_t *rq_cq_ctx = &(rq_queue->rq_cq_ctx); \
	register rq_ctx_t *rq_ctx = &(rq_queue->rq_ctx); \
	register struct flexio_dev_wqe_rcv_data_seg *rwqe; \
	register union flexio_dev_sqe_seg *swqe; \
	register uint32_t rq_wqe_idx; \
	register uint32_t data_sz; \
	register char *rq_data; \
	register char *packet; \
	\
	rq_wqe_idx = be16_to_cpu((volatile __be16)rq_cq_ctx->cqe->wqe_counter); \
	data_sz = be32_to_cpu((volatile __be32)rq_cq_ctx->cqe->byte_cnt); \
	rwqe = &(rq_ctx->rq_ring[rq_wqe_idx & RQ_IDX_MASK]); \
	rq_data = (void *)be64_to_cpu((volatile __be64)rwqe->addr); \
	packet = (_host_buffer) ? \
		(char *)((flexio_uintptr_t)rq_data - rq_ctx->rqd_host_addr + \
			 rq_ctx->rqd_dpa_addr) : rq_data; \
	*tenant_id = pp_decode_tenant(sch_ctx, packet, data_sz); \
	*forwarded = *tenant_id < sch_ctx->tenants_num && \
		!__atomic_load_n(&sch_ctx->restrict_tenant[*tenant_id], \
				 __ATOMIC_RELAXED); \
	if (*forwarded) { \
		pp_apply_worker_workload(thd_ctx, packet, data_sz); \
		swqe = &(tx_sq_ctx->sq_ring[(tx_sq_ctx->sq_wqe_seg_idx + 2) & \
					      SQ_IDX_MASK]); \
		tx_sq_ctx->sq_wqe_seg_idx += 4; \
		flexio_dev_swqe_seg_mem_ptr_data_set(swqe, data_sz, rq_queue->rq_lkey, \
						   (uint64_t)rq_data); \
		__dpa_thread_memory_writeback(); \
		if (_host_buffer) { \
			__dpa_thread_window_writeback(); \
		} \
		flexio_dev_qp_sq_ring_db(dtctx, ++tx_sq_ctx->sq_pi, tx_sq_number); \
	} \
	flexio_dev_dbr_rq_inc_pi(rq_ctx->rq_dbr); \
	com_step_cq(rq_cq_ctx); \
	return data_sz; \
}

PP_DEFINE_AFFINE_QUEUE(pp_queue_workload_affine, 0)
PP_DEFINE_AFFINE_QUEUE(pp_queue_workload_affine_host, 1)

#undef PP_DEFINE_AFFINE_QUEUE

#define PP_DEFINE_QUEUE_WORKLOAD(_name, _workload) \
static inline __attribute__((always_inline)) uint32_t \
_name(struct flexio_dev_thread_ctx *dtctx, \
      struct dpa_thread_context *thd_ctx, \
      struct flexio_dpa_dev_queue *rq_queue, \
      sq_ctx_t *tx_sq_ctx, \
      uint32_t tx_sq_number) \
{ \
	(void)thd_ctx; \
	register cq_ctx_t *rq_cq_ctx = &(rq_queue->rq_cq_ctx); \
	register rq_ctx_t *rq_ctx = &(rq_queue->rq_ctx); \
	register struct flexio_dev_wqe_rcv_data_seg *rwqe; \
	register union flexio_dev_sqe_seg *swqe; \
	register uint32_t rq_wqe_idx; \
	register uint32_t data_sz; \
	register char *rq_data; \
	\
	rq_wqe_idx = be16_to_cpu((volatile __be16)rq_cq_ctx->cqe->wqe_counter); \
	data_sz = be32_to_cpu((volatile __be32)rq_queue->rq_cq_ctx.cqe->byte_cnt); \
	rwqe = &(rq_ctx->rq_ring[rq_wqe_idx & RQ_IDX_MASK]); \
	rq_data = (void *)be64_to_cpu((volatile __be64)rwqe->addr); \
	\
	_workload(rq_data, data_sz); \
	\
	swqe = &(tx_sq_ctx->sq_ring[(tx_sq_ctx->sq_wqe_seg_idx + 2) & SQ_IDX_MASK]); \
	tx_sq_ctx->sq_wqe_seg_idx += 4; \
	flexio_dev_swqe_seg_mem_ptr_data_set(swqe, data_sz, rq_queue->rq_lkey, (uint64_t)rq_data); \
	\
	__dpa_thread_memory_writeback(); \
	flexio_dev_qp_sq_ring_db(dtctx, ++tx_sq_ctx->sq_pi, tx_sq_number); \
	flexio_dev_dbr_rq_inc_pi(rq_ctx->rq_dbr); \
	com_step_cq(rq_cq_ctx); \
	\
	return data_sz; \
}

#define PP_DEFINE_QUEUE_WORKLOAD_CTX(_name, _workload) \
static inline __attribute__((always_inline)) uint32_t \
_name(struct flexio_dev_thread_ctx *dtctx, \
      struct dpa_thread_context *thd_ctx, \
      struct flexio_dpa_dev_queue *rq_queue, \
      sq_ctx_t *tx_sq_ctx, \
      uint32_t tx_sq_number) \
{ \
	register cq_ctx_t *rq_cq_ctx = &(rq_queue->rq_cq_ctx); \
	register rq_ctx_t *rq_ctx = &(rq_queue->rq_ctx); \
	register struct flexio_dev_wqe_rcv_data_seg *rwqe; \
	register union flexio_dev_sqe_seg *swqe; \
	register uint32_t rq_wqe_idx; \
	register uint32_t data_sz; \
	register char *rq_data; \
	\
	rq_wqe_idx = be16_to_cpu((volatile __be16)rq_cq_ctx->cqe->wqe_counter); \
	data_sz = be32_to_cpu((volatile __be32)rq_queue->rq_cq_ctx.cqe->byte_cnt); \
	rwqe = &(rq_ctx->rq_ring[rq_wqe_idx & RQ_IDX_MASK]); \
	rq_data = (void *)be64_to_cpu((volatile __be64)rwqe->addr); \
	\
	_workload(thd_ctx, rq_data, data_sz); \
	\
	swqe = &(tx_sq_ctx->sq_ring[(tx_sq_ctx->sq_wqe_seg_idx + 2) & SQ_IDX_MASK]); \
	tx_sq_ctx->sq_wqe_seg_idx += 4; \
	flexio_dev_swqe_seg_mem_ptr_data_set(swqe, data_sz, rq_queue->rq_lkey, (uint64_t)rq_data); \
	\
	__dpa_thread_memory_writeback(); \
	flexio_dev_qp_sq_ring_db(dtctx, ++tx_sq_ctx->sq_pi, tx_sq_number); \
	flexio_dev_dbr_rq_inc_pi(rq_ctx->rq_dbr); \
	com_step_cq(rq_cq_ctx); \
	\
	return data_sz; \
}

PP_DEFINE_QUEUE_WORKLOAD(pp_queue, pp_workload_l2_reflector)
PP_DEFINE_QUEUE_WORKLOAD(pp_queue_checksum16, pp_workload_checksum16)
PP_DEFINE_QUEUE_WORKLOAD(pp_queue_checksum_nrnd, pp_workload_checksum_nrnd)
PP_DEFINE_QUEUE_WORKLOAD_CTX(pp_queue_nof, pp_workload_nof)

#undef PP_DEFINE_QUEUE_WORKLOAD_CTX
#undef PP_DEFINE_QUEUE_WORKLOAD

#define PP_DEFINE_QUEUE_WORKLOAD_HOST(_name, _workload) \
static inline __attribute__((always_inline)) uint32_t \
_name(struct flexio_dev_thread_ctx *dtctx, \
      struct dpa_thread_context *thd_ctx, \
      struct flexio_dpa_dev_queue *rq_queue, \
      sq_ctx_t *tx_sq_ctx, \
      uint32_t tx_sq_number) \
{ \
	(void)thd_ctx; \
	register cq_ctx_t *rq_cq_ctx = &(rq_queue->rq_cq_ctx); \
	register rq_ctx_t *rq_ctx = &(rq_queue->rq_ctx); \
	register struct flexio_dev_wqe_rcv_data_seg *rwqe; \
	register union flexio_dev_sqe_seg *swqe; \
	register uint32_t rq_wqe_idx; \
	register uint32_t data_sz; \
	register char *rq_data; \
	register char *rq_data_host; \
	\
	rq_wqe_idx = be16_to_cpu((volatile __be16)rq_cq_ctx->cqe->wqe_counter); \
	data_sz = be32_to_cpu((volatile __be32)rq_queue->rq_cq_ctx.cqe->byte_cnt); \
	rwqe = &(rq_ctx->rq_ring[rq_wqe_idx & RQ_IDX_MASK]); \
	rq_data_host = (void *)be64_to_cpu((volatile __be64)rwqe->addr); \
	rq_data = (char *)((flexio_uintptr_t)rq_data_host - \
			   rq_ctx->rqd_host_addr + rq_ctx->rqd_dpa_addr); \
	\
	_workload(rq_data, data_sz); \
	\
	swqe = &(tx_sq_ctx->sq_ring[(tx_sq_ctx->sq_wqe_seg_idx + 2) & SQ_IDX_MASK]); \
	tx_sq_ctx->sq_wqe_seg_idx += 4; \
	flexio_dev_swqe_seg_mem_ptr_data_set(swqe, data_sz, rq_queue->rq_lkey, (uint64_t)rq_data_host); \
	\
	__dpa_thread_memory_writeback(); \
	__dpa_thread_window_writeback(); \
	flexio_dev_qp_sq_ring_db(dtctx, ++tx_sq_ctx->sq_pi, tx_sq_number); \
	flexio_dev_dbr_rq_inc_pi(rq_ctx->rq_dbr); \
	com_step_cq(rq_cq_ctx); \
	\
	return data_sz; \
}

#define PP_DEFINE_QUEUE_WORKLOAD_HOST_CTX(_name, _workload) \
static inline __attribute__((always_inline)) uint32_t \
_name(struct flexio_dev_thread_ctx *dtctx, \
      struct dpa_thread_context *thd_ctx, \
      struct flexio_dpa_dev_queue *rq_queue, \
      sq_ctx_t *tx_sq_ctx, \
      uint32_t tx_sq_number) \
{ \
	register cq_ctx_t *rq_cq_ctx = &(rq_queue->rq_cq_ctx); \
	register rq_ctx_t *rq_ctx = &(rq_queue->rq_ctx); \
	register struct flexio_dev_wqe_rcv_data_seg *rwqe; \
	register union flexio_dev_sqe_seg *swqe; \
	register uint32_t rq_wqe_idx; \
	register uint32_t data_sz; \
	register char *rq_data; \
	register char *rq_data_host; \
	\
	rq_wqe_idx = be16_to_cpu((volatile __be16)rq_cq_ctx->cqe->wqe_counter); \
	data_sz = be32_to_cpu((volatile __be32)rq_queue->rq_cq_ctx.cqe->byte_cnt); \
	rwqe = &(rq_ctx->rq_ring[rq_wqe_idx & RQ_IDX_MASK]); \
	rq_data_host = (void *)be64_to_cpu((volatile __be64)rwqe->addr); \
	rq_data = (char *)((flexio_uintptr_t)rq_data_host - \
			   rq_ctx->rqd_host_addr + rq_ctx->rqd_dpa_addr); \
	\
	_workload(thd_ctx, rq_data, data_sz); \
	\
	swqe = &(tx_sq_ctx->sq_ring[(tx_sq_ctx->sq_wqe_seg_idx + 2) & SQ_IDX_MASK]); \
	tx_sq_ctx->sq_wqe_seg_idx += 4; \
	flexio_dev_swqe_seg_mem_ptr_data_set(swqe, data_sz, rq_queue->rq_lkey, (uint64_t)rq_data_host); \
	\
	__dpa_thread_memory_writeback(); \
	__dpa_thread_window_writeback(); \
	flexio_dev_qp_sq_ring_db(dtctx, ++tx_sq_ctx->sq_pi, tx_sq_number); \
	flexio_dev_dbr_rq_inc_pi(rq_ctx->rq_dbr); \
	com_step_cq(rq_cq_ctx); \
	\
	return data_sz; \
}

PP_DEFINE_QUEUE_WORKLOAD_HOST(pp_queue_host, pp_workload_l2_reflector)
PP_DEFINE_QUEUE_WORKLOAD_HOST(pp_queue_checksum16_host, pp_workload_checksum16)
PP_DEFINE_QUEUE_WORKLOAD_HOST(pp_queue_checksum_nrnd_host, pp_workload_checksum_nrnd)
PP_DEFINE_QUEUE_WORKLOAD_HOST_CTX(pp_queue_nof_host, pp_workload_nof)

#undef PP_DEFINE_QUEUE_WORKLOAD_HOST_CTX
#undef PP_DEFINE_QUEUE_WORKLOAD_HOST

flexio_dev_rpc_handler_t thd_ctx_init;
__dpa_rpc__ uint64_t thd_ctx_init(uint64_t data);

#endif /* __FLEXIO_PP_DEV_UTILS_H__ */
