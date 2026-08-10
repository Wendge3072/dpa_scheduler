/*
 * SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
/* Shared header file with defines and structures that used in
 * both host and device sides samples.
 */

#ifndef __FLEXIO_PP_COM_H__
#define __FLEXIO_PP_COM_H__

#include <stdint.h>

/* Scheduler configurations */
#define MAX_SCHEDULER_QUEUES 64
#define MAX_TENANT_NUM 4
#define WORKER_QUEUES_PER_THREAD MAX_TENANT_NUM
#define DPA_FREQ_HZ 1800000000ULL  // 1.8GHz
#define QOS_RESOURCE_PERCENT_TOTAL 100

/* Every usage of this value is in bytes */
#define MATCH_VAL_BSIZE 64

/* Convert logarithm to value */
#define L2V(l) (1UL << (l))
/* Number of entries in each RQ/SQ/CQ is 2^LOG_Q_DEPTH. */
#define LOG_Q_DEPTH 7
#define Q_DEPTH L2V(LOG_Q_DEPTH)

/* Mask for CQ index */
#define CQ_IDX_MASK ((1 << LOG_Q_DEPTH) - 1)
/* Mask for RQ index */
#define RQ_IDX_MASK ((1 << LOG_Q_DEPTH) - 1)
/* Mask for SQ index */
#define SQ_IDX_MASK ((1 << (LOG_Q_DEPTH + LOG_SQE_NUM_SEGS)) - 1)
/* Mask for data index */
#define DATA_IDX_MASK ((1 << (LOG_Q_DEPTH)) - 1)

/* SQ/RQ data entry byte size is 2048B (enough for ethernet packet data). */
#define LOG_Q_DATA_ENTRY_BSIZE 11
/* SQ/RQ data entry byte size log to value. */
#define Q_DATA_ENTRY_BSIZE L2V(LOG_Q_DATA_ENTRY_BSIZE)
/* SQ/RQ DATA byte size is queue depth times entry byte size. */
#define Q_DATA_BSIZE (Q_DEPTH * Q_DATA_ENTRY_BSIZE)

/* SQ WQE byte size is 64B. */
#define LOG_SQ_WQE_BSIZE 6
/* SQ WQE byte size log to value. */
#define SQ_WQE_BSIZE L2V(LOG_SQ_WQE_BSIZE)
/* SQ ring byte size is queue depth times WQE byte size. */
#define SQ_RING_BSIZE (Q_DEPTH * SQ_WQE_BSIZE)

/* CQE size is 64B */
#define CQE_BSIZE 64
#define CQ_BSIZE (Q_DEPTH * CQE_BSIZE)

/* RQ WQE byte size is 64B. */
#define LOG_RQ_WQE_BSIZE 4
/* RQ WQE byte size log to value. */
#define RQ_WQE_BSIZE L2V(LOG_RQ_WQE_BSIZE)
/* RQ ring byte size is queue depth times WQE byte size. */
#define RQ_RING_BSIZE (Q_DEPTH * RQ_WQE_BSIZE)

#define SPEED_RESULT_SIZE 8
#define ETH_HEADER_SIZE 42
#define NVME_QUEUE_ENTRY_SIZE (1024)
#define NVME_QUEUE_ENTRY_NUM (128)
#define NVME_QUEUE_MEMORY_SIZE (NVME_QUEUE_ENTRY_SIZE * NVME_QUEUE_ENTRY_NUM)

/* Structure for transfer CQ data */
struct app_transfer_cq {
	/* CQ number */
	uint32_t cq_num;
	/* Depth of CQ in the logarithm */
	uint32_t log_cq_depth;
	/* CQ ring DPA address */
	flexio_uintptr_t cq_ring_daddr;
	/* CQ DBR DPA address */
	flexio_uintptr_t cq_dbr_daddr;
} __attribute__((__packed__, aligned(8)));

/* Structure for transfer WQ data */
struct app_transfer_wq {
	/* WQ number */
	uint32_t wq_num;
	/* WQ MKEY Id */
	uint32_t wqd_mkey_id;
	/* WQ ring DPA address */
	flexio_uintptr_t wq_ring_daddr;
	/* WQ ring DBR address */
	flexio_uintptr_t wq_dbr_daddr;
	/* WQ data address */
	flexio_uintptr_t wqd_daddr;
} __attribute__((__packed__, aligned(8)));

struct host2dev_queue {
	/* RQ's CQ transfer information. */
	struct app_transfer_cq rq_cq_transf;
	/* RQ transfer information. */
	struct app_transfer_wq rq_transf;
	/* SQ's CQ transfer information. */
	struct app_transfer_cq sq_cq_transf;
	/* SQ transfer information. */
	struct app_transfer_wq sq_transf;
};

/* Collateral structure for transfer host data to device */
struct host2dev_packet_processor_data_thd {
	/* RQ's CQ transfer information. */
	struct app_transfer_cq rq_cq_transf;
	/* RQ transfer information. */
	struct app_transfer_wq rq_transf;
	/* SQ's CQ transfer information. */
	struct app_transfer_cq sq_cq_transf;
	/* SQ transfer information. */
	struct app_transfer_wq sq_transf;
	uint8_t not_first_run;

	// flexio_uintptr_t dpa_thread_running_bm_daddr;

	int thd_id;
	int buffer_location;
	int use_copy;
	uint32_t window_id;
	uint32_t result_buffer_mkey_id;
	void* result_buffer;
	void* host_buffer;
	uint64_t dpa_result_buffer;
} __attribute__((__packed__, aligned(8)));

struct host2dev_packet_processor_data_sch {
	struct host2dev_queue queues[MAX_SCHEDULER_QUEUES];
	// flexio_uintptr_t dpa_thread_running_bm_daddr;
	uint8_t not_first_run;
	int sch_id;
	int buffer_location;
	int use_copy;
	uint32_t window_id;
	uint32_t result_buffer_mkey_id;
	void* result_buffer;
	uint64_t dpa_result_buffer;
	size_t num_queues;
	size_t scheduler_num;
	size_t threads_num_per_scheduler;
	size_t tenants_num;
} __attribute__((__packed__, aligned(8)));

struct host2dev_qos_update {
	size_t scheduler_num;
	size_t threads_num_per_scheduler;
	size_t tenants_num;
	int buffer_location;
	uint32_t cycle_weights[MAX_TENANT_NUM];
	uint32_t bandwidth_weights[MAX_TENANT_NUM];
} __attribute__((__packed__, aligned(8)));


struct host_to_device_config {
	uint32_t window_id;     /* FlexIO Window ID */
	uint32_t mkey;          /* Memory key for the host data */
	uint64_t haddr;         /* Host address for the buffer */
} __attribute__((__packed__, aligned(8)));

#endif /* __FLEXIO_PP_COM_H__ */
