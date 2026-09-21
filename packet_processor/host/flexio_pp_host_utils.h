#ifndef __FLEXIO_PP_HOST_UTILS_H__
#define __FLEXIO_PP_HOST_UTILS_H__

#include <unistd.h>
#include <malloc.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <fcntl.h>

#include <inttypes.h>
#include <assert.h>

/* Used for IBV device operations. */
#include <infiniband/mlx5dv.h>

/* Flex IO SDK host side API header. */
#include <libflexio/flexio.h>

/* Common header for communication between host and DPA. */
#include "../flexio_pp_com.h"

struct flow_matcher {
	struct mlx5dv_dr_domain *dr_domain;
	struct mlx5dv_dr_table *dr_table_root;
	struct mlx5dv_dr_matcher *dr_matcher_root;
	struct mlx5dv_dr_table *dr_table_sws;
	struct mlx5dv_dr_matcher *dr_matcher_sws;
};

struct flow_rule {
	struct mlx5dv_dr_action *actions[2];
	uint32_t action_count;
	struct mlx5dv_dr_rule *dr_rule;
};

/* Application context struct holding necessary host side variables */
struct app_context {
	struct flexio_process *flexio_process;
	struct flexio_window *flexio_window;
	struct flexio_msg_stream *stream;
	struct flexio_uar *process_uar;
	struct ibv_pd *process_pd;
	struct ibv_context *ibv_ctx;
	struct ibv_mr *mr;
	struct flow_matcher *rx_matcher;
	struct flow_matcher *tx_matcher;
};

struct flexio_queues {
	/* Flex IO SQ's CQ. */
	struct flexio_cq *flexio_sq_cq_ptr;
	/* Flex IO SQ. */
	struct flexio_sq *flexio_sq_ptr;
	/* Flex IO RQ's CQ. */
	struct flexio_cq *flexio_rq_cq_ptr;
	/* Flex IO RQ. */
	struct flexio_rq *flexio_rq_ptr;
	/* Transfer structs with information to pass to DPA side.
	 * The structs are defined by a common header which both sides may use.
	 */
	/* SQ's CQ transfer information. */
	struct app_transfer_cq sq_cq_transf;
	/* SQ transfer information. */
	struct app_transfer_wq sq_transf;
	/* RQ's CQ transfer information. */
	struct app_transfer_cq rq_cq_transf;
	/* RQ transfer information. */
	struct app_transfer_wq rq_transf;

	/* Memory key (MKey) for SQ data. */
	struct flexio_mkey *sqd_mkey;
	/* MKey for RQ data. */
	struct flexio_mkey *rqd_mkey;

	void *host_rq_buffer;
	void *host_sq_buffer;
	uint32_t host_rq_mkey_id;
	uint32_t host_sq_mkey_id;
	size_t rq_data_on_host;
	size_t sq_data_on_host;
	size_t sq_data_reuses_rq;

	struct mlx5dv_devx_obj *rq_tir_obj;

	struct flow_rule *rx_flow_rule;
	struct flow_rule *tx_flow_rule;
	struct flow_rule *tx_flow_rule2;
};

struct thread_context {
	struct flexio_event_handler *event_handler;
	struct ibv_mr* mr;

	struct flexio_queues *queues;
	uint32_t num_queues;

	flexio_uintptr_t app_data_daddr;

	uint32_t result_buffer_mkey_id;

	void* result_buffer;
	void* host_buffer;
	void* host_alloc_base;
	size_t host_alloc_size;
	
	int thd_id;
};

enum matcher_criteria {
	MATCHER_CRITERIA_EMPTY = 0,
	MATCHER_CRITERIA_OUTER = 1 << 0,
	MATCHER_CRITERIA_MISC  = 1 << 1,
	MATCHER_CRITERIA_INNER = 1 << 2,
	MATCHER_CRITERIA_MISC2 = 1 << 3,
	MATCHER_CRITERIA_MISC3 = 1 << 4,
};

struct mlx5_ifc_dr_match_spec_bits {
	uint8_t smac_47_16[0x20];

	uint8_t smac_15_0[0x10];
	uint8_t ethertype[0x10];

	uint8_t dmac_47_16[0x20];

	uint8_t dmac_15_0[0x10];
	uint8_t first_prio[0x3];
	uint8_t first_cfi[0x1];
	uint8_t first_vid[0xc];

	uint8_t ip_protocol[0x8];
	uint8_t ip_dscp[0x6];
	uint8_t ip_ecn[0x2];
	uint8_t cvlan_tag[0x1];
	uint8_t svlan_tag[0x1];
	uint8_t frag[0x1];
	uint8_t ip_version[0x4];
	uint8_t tcp_flags[0x9];

	uint8_t tcp_sport[0x10];
	uint8_t tcp_dport[0x10];

	uint8_t reserved_at_c0[0x18];
	uint8_t ip_ttl_hoplimit[0x8];

	uint8_t udp_sport[0x10];
	uint8_t udp_dport[0x10];

	uint8_t src_ip_127_96[0x20];

	uint8_t src_ip_95_64[0x20];

	uint8_t src_ip_63_32[0x20];

	uint8_t src_ip_31_0[0x20];

	uint8_t dst_ip_127_96[0x20];

	uint8_t dst_ip_95_64[0x20];

	uint8_t dst_ip_63_32[0x20];

	uint8_t dst_ip_31_0[0x20];
};

/* Flex IO packet processor application struct.
 * Created by DPACC during compilation. The DEV_APP_NAME
 * is a macro transferred from Meson through gcc, with the
 * same name as the created application.
 */
extern struct flexio_app *DEV_APP_NAME;
/* Flex IO packet processor device (DPA) side function stub. */
extern flexio_func_t thd_ctx_init;
extern flexio_func_t qos_update;
extern flexio_func_t flexio_pp_dev_worker;
extern flexio_func_t flexio_pp_dev_worker_host;
extern flexio_func_t flexio_scheduler_handle;

extern size_t scheduler_num;
extern size_t threads_num_per_scheduler;
extern size_t threads_num;
extern size_t tenants_num;
extern size_t tenant_shards;
extern size_t begin_schedr;
extern size_t begin_worker;
extern uint64_t DMAC;
extern size_t buffer_location;
extern size_t use_copy;

/* Open ibv device
 * Returns 0 on success and -1 if the destroy was failed.
 * app_ctx - app_ctx - pointer to app_context structure.
 * device - device name to open.
 */
int app_open_ibv_ctx(struct app_context *app_ctx, char *device);

/* Create an SQ over the DPA for sending packets from DPA to wire.
 * A CQ is also created for the SQ.
 * Returns 0 on success and -1 if the allocation fails.
 * app_ctx - app_ctx - pointer to app_context structure.
 */
int create_app_sq(struct app_context *app_ctx, struct thread_context* thd_ctx,
	size_t buffer_location, size_t use_copy);

/* Create an RQ over the DPA for receiving packets on DPA.
 * A CQ is also created for the RQ.
 * Returns 0 on success and -1 if the allocation fails.
 * app_ctx - app_ctx - pointer to app_context structure.
 */
int create_app_rq(struct app_context *app_ctx, struct thread_context* thd_ctx,
	size_t buffer_location);

/* Copy schedular information to DPA.
 * DPA side needs queue information in order to process the packets.
 * The DPA heap memory address will be passed as the event handler argument.
 * Returns 0 if success and -1 if the copy failed.
 * app_ctx - app_ctx - pointer to app_context structure.
 */
int copy_sch_data_to_dpa(struct app_context *app_ctx, struct thread_context *thd_ctx, 
	int buffer_location, size_t use_copy);

/* Copy application information to DPA.
 * DPA side needs queue information in order to process the packets.
 * The DPA heap memory address will be passed as the event handler argument.
 * Returns 0 if success and -1 if the copy failed.
 * app_ctx - app_ctx - pointer to app_context structure.
 */
int copy_thd_data_to_dpa(struct app_context *app_ctx, struct thread_context *thd_ctx, 
	int buffer_location, size_t use_copy);

/* Clean up previously allocated RQ
 * Returns 0 on success and -1 if the destroy failed.
 * app_ctx - app_ctx - pointer to app_context structure.
 */
int clean_up_app_rq(struct app_context* app_ctx, struct thread_context *thd_ctx);

/* Clean up previously allocated SQ
 * Returns 0 on success and -1 if the destroy failed.
 * app_ctx - app_ctx - pointer to app_context structure.
 */
int clean_up_app_sq(struct app_context* app_ctx, struct thread_context *thd_ctx);

// ----==== FLOW STEERING FUNCTIONS ====----

/* Create a flow matcher for Ethernet packets received on the NIC.
 *  ibv_ctx - context of the IBV device.
 */
struct flow_matcher *create_matcher_rx(struct ibv_context *ibv_ctx);

/* Create a SW flow steering rule for ethernet packets received on the NIC.
 *  ibv_ctx - context of the IBV device.
 *  tir_obj - TIR mlx5dv object
 *  dmac - Destination MAC address.
 *  tenant_tag - Non-zero tenant identifier written to the receive CQE.
 */
struct flow_rule *create_rule_rx_mac_match(struct flow_matcher *flow_match,
					   struct mlx5dv_devx_obj *tir_obj,
					   uint64_t dmac, uint32_t tenant_tag);

/* Create a flow matcher for Ethernet packets transmitted on the NIC.
 *  ibv_ctx - context of the IBV device.
 */
struct flow_matcher *create_matcher_tx(struct ibv_context *ibv_ctx);

/* Create a flow rule for Ethernet packets transmitted on the NIC.
 *  flow_match - pointer to the previously created flow_matcher structure.
 *  dmac - Destination MAC address
 */
struct flow_rule *create_rule_tx_fwd_to_vport(struct flow_matcher *flow_match,
					      uint64_t dmac);

/* Create a flow rule for Ethernet packets transmitted on the NIC through
 *  thw software-steering table.
 *  flow_match - pointer to the previously created flow_matcher structure.
 *  dmac - Destination MAC address
 */
struct flow_rule *create_rule_tx_fwd_to_sws_table(struct flow_matcher *flow_match,
						  uint64_t dmac);
/* Destroy the flow matcher.
 *  matcher - the matcher to destroy.
 */
int destroy_matcher(struct flow_matcher *matcher);

/* Destroy the flow rule.
 *  rule - the rule to destroy.
 */
int destroy_rule(struct flow_rule *rule);

#endif /* __FLEXIO_PP_HOST_UTILS_H__ */
