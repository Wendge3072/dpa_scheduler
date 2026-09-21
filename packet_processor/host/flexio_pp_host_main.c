#include "flexio_pp_host_utils.h"
#include <errno.h>
#include <string.h>

size_t scheduler_num = 1;
size_t tenants_num = DEFAULT_TENANT_NUM;
size_t tenant_shards = 0;
size_t threads_num_per_scheduler = 8;
size_t threads_num = 0;
size_t begin_schedr = 0;
size_t begin_worker = 16;
uint64_t DMAC = 0xa088c2320440;
size_t buffer_location = 0;
size_t use_copy = 1;

struct tenant_flow_binding {
	struct flow_rule *rx_rule;
	struct flow_rule *tx_table_rule;
	struct flow_rule *tx_vport_rule;
	uint64_t dmac;
	uint32_t scheduler_id;
	uint32_t tenant_id;
	uint32_t worker_id;
};

static size_t align_to_cacheline(size_t size)
{
	return (size + 63) & ~(size_t)63;
}

static int qos_line_is_exit(const char *line)
{
	size_t token_len = 0;

	while (*line == ' ' || *line == '\t' || *line == '\n' || *line == '\r') {
		line++;
	}
	while (line[token_len] &&
	       line[token_len] != ' ' &&
	       line[token_len] != '\t' &&
	       line[token_len] != '\n' &&
	       line[token_len] != '\r') {
		token_len++;
	}

	return (token_len == 1 && !strncmp(line, "q", token_len)) ||
	       (token_len == 4 && !strncmp(line, "quit", token_len)) ||
	       (token_len == 4 && !strncmp(line, "exit", token_len));
}

static int parse_qos_request(const char *line, struct host2dev_qos_update *update)
{
	uint32_t values[MAX_TENANT_NUM * 2] = {0};
	uint32_t tenant_count = tenants_num > MAX_TENANT_NUM ?
				MAX_TENANT_NUM : tenants_num;
	uint32_t needed = tenant_count * 2;
	uint32_t parsed = 0;
	uint64_t cycle_sum = 0;
	uint64_t bw_sum = 0;
	const char *cursor = line;

	while (*cursor && parsed < needed) {
		char *end = NULL;
		unsigned long value = 0;

		while (*cursor && (*cursor < '0' || *cursor > '9')) {
			cursor++;
		}
		if (!*cursor) {
			break;
		}
		if (cursor > line && cursor[-1] == '-') {
			return -1;
		}
		errno = 0;
		value = strtoul(cursor, &end, 10);
		if (errno || end == cursor || value > QOS_MAX_WEIGHT) {
			return -1;
		}
		values[parsed++] = (uint32_t)value;
		cursor = end;
	}

	if (parsed != needed) {
		return -1;
	}

	memset(update, 0, sizeof(*update));
	update->scheduler_num = scheduler_num;
	update->threads_num_per_scheduler = threads_num_per_scheduler;
	update->tenants_num = tenant_count;
	update->buffer_location = buffer_location;

	for (uint32_t t = 0; t < tenant_count; t++) {
		update->cycle_weights[t] = values[t];
		update->bandwidth_weights[t] = values[tenant_count + t];
		cycle_sum += update->cycle_weights[t];
		bw_sum += update->bandwidth_weights[t];
	}

	if (!cycle_sum || !bw_sum) {
		return -1;
	}

	return 0;
}

static int send_qos_update(struct app_context *app_ctx,
			   struct host2dev_qos_update *update)
{
	flexio_uintptr_t update_daddr = 0;
	uint64_t rpc_ret_val = 0;
	int ret = 0;

	if (flexio_copy_from_host(app_ctx->flexio_process, update, sizeof(*update),
				  &update_daddr)) {
		printf("Failed to copy QoS update to DPA.\n");
		return -1;
	}

	if (flexio_process_call(app_ctx->flexio_process, &qos_update,
				&rpc_ret_val, update_daddr)) {
		printf("Failed to call QoS update RPC.\n");
		ret = -1;
	} else if (rpc_ret_val) {
		printf("QoS update rejected by DPA, rpc_ret=%" PRIu64 "\n",
		       rpc_ret_val);
		ret = -1;
	}

	if (flexio_buf_dev_free(app_ctx->flexio_process, update_daddr)) {
		printf("Failed to free QoS update buffer on DPA heap.\n");
		ret = -1;
	}

	return ret;
}

static int listen_for_qos_requests(struct app_context *app_ctx)
{
	char line[4096];

	printf("QoS listener started. Use: qos <%zu cycle weights> <%zu bandwidth weights>, or q to exit.\n",
	       tenants_num, tenants_num);
	while (fgets(line, sizeof(line), stdin)) {
		struct host2dev_qos_update update;

		if (qos_line_is_exit(line)) {
			break;
		}
		if (parse_qos_request(line, &update)) {
			printf("Invalid QoS request. Provide exactly %zu cycle weights and %zu bandwidth weights; each is 0..%u and each group needs a non-zero sum.\n",
			       tenants_num, tenants_num, QOS_MAX_WEIGHT);
			continue;
		}
		if (!send_qos_update(app_ctx, &update)) {
			printf("QoS updated:");
			for (uint32_t t = 0; t < update.tenants_num; t++) {
				printf(" tenant%u(cycle_weight=%u, bw_weight=%u)", t,
				       update.cycle_weights[t],
				       update.bandwidth_weights[t]);
			}
			printf("\n");
		}
	}

	return 0;
}

static int alloc_context_host_memory(struct app_context *app_ctx,
				     struct thread_context *ctx,
				     size_t extra_host_buffer_size)
{
	size_t queue_buffer_count = buffer_location ? ctx->num_queues * (use_copy ? 2 : 1) : 0;
	size_t needed_buffer_size = queue_buffer_count * Q_DATA_BSIZE +
				    SPEED_RESULT_SIZE + extra_host_buffer_size;
	size_t mmap_size = align_to_cacheline(needed_buffer_size);
	void *tmp_ptr = mmap(NULL, mmap_size, PROT_READ | PROT_WRITE,
			     MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
	int access = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
		     IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_ATOMIC;
	char *cursor;

	if (tmp_ptr == MAP_FAILED) {
		printf("Failed to allocate host buffer\n");
		return -1;
	}
	memset(tmp_ptr, 0, mmap_size);

	if (!buffer_location && extra_host_buffer_size == 0) {
		access |= IBV_ACCESS_RELAXED_ORDERING;
	}

	ctx->mr = ibv_reg_mr(app_ctx->process_pd, tmp_ptr, mmap_size, access);
	if (ctx->mr == NULL) {
		printf("Failed to register MR\n");
		return -1;
	}

	ctx->host_alloc_base = tmp_ptr;
	ctx->host_alloc_size = mmap_size;
	cursor = (char *)tmp_ptr;

	if (buffer_location) {
		for (uint32_t i = 0; i < ctx->num_queues; i++) {
			ctx->queues[i].host_rq_buffer = cursor;
			ctx->queues[i].host_rq_mkey_id = ctx->mr->lkey;
			cursor += Q_DATA_BSIZE;

			if (use_copy == 0) {
				ctx->queues[i].host_sq_buffer = ctx->queues[i].host_rq_buffer;
			} else {
				ctx->queues[i].host_sq_buffer = cursor;
				cursor += Q_DATA_BSIZE;
			}
			ctx->queues[i].host_sq_mkey_id = ctx->mr->lkey;
		}
	}

	ctx->result_buffer_mkey_id = ctx->mr->lkey;
	ctx->result_buffer = cursor;
	cursor += SPEED_RESULT_SIZE;
	ctx->host_buffer = extra_host_buffer_size ? cursor : NULL;

	return 0;
}

// #define nic_mode 1
/* Main host side function.
 * Responsible for allocating resources and making preparations for DPA side envocatin.
 */
int main(int argc, char **argv)
{
	size_t scheduler_queue_count = 0;

	if (argc < 2) {
		printf("Usage: %s <device> [schedulers] [tenants] [workers/scheduler] [scheduler EU] [worker EU] [buffer location] [shards/tenant]\n",
		       argv[0]);
		return -1;
	}

	if (argc > 2) {
		scheduler_num = atoi(argv[2]);
	}

    if (argc > 3) {
        tenants_num = atoi(argv[3]);
    }

	if (!tenants_num || tenants_num > MAX_TENANT_NUM) {
		printf("Invalid tenants_num value. Valid range is 1..%d.\n",
		       MAX_TENANT_NUM);
		return -1;
	}

	if (argc > 4) {
		threads_num_per_scheduler = atoi(argv[4]);
	}

	threads_num = threads_num_per_scheduler * scheduler_num;
	scheduler_queue_count = threads_num_per_scheduler * WORKER_QUEUES_PER_THREAD;

	if (!scheduler_num || scheduler_num > 32 || !threads_num_per_scheduler ||
	    scheduler_queue_count > MAX_SCHEDULER_QUEUES || threads_num > 190) {
		printf("Invalid topology. Schedulers must be 1..32, workers per scheduler 1..%d, and total workers <= 190.\n",
		       MAX_SCHEDULER_QUEUES / WORKER_QUEUES_PER_THREAD);
		return -1;
	}

	if (argc > 5) {
        begin_schedr = atoi(argv[5]);
    }

	if (argc > 6) {
        begin_worker = atoi(argv[6]);
		if (begin_worker < ((scheduler_num + 15) / 16) * 16) {
			printf("Invalid begin_worker value. It must be at least %zu.\n", ((scheduler_num + 15) / 16) * 16);
			return -1;
		}
    }

	if (argc > 7) {
		buffer_location = atoi(argv[7]);
	}

	if (argc > 8) {
		tenant_shards = atoi(argv[8]);
	} else {
		tenant_shards = threads_num_per_scheduler;
	}
	if (!tenant_shards ||
	    tenant_shards > threads_num_per_scheduler * WORKER_QUEUES_PER_THREAD) {
		printf("Invalid tenant_shards value. Valid range is 1..%zu.\n",
		       threads_num_per_scheduler * WORKER_QUEUES_PER_THREAD);
		return -1;
	}

	int err = 0;
	flexio_status ret = 0;
	struct flexio_process_attr process_attr = { NULL, 0 };
	struct app_context app_ctx = {};
	struct thread_context* thd_ctx = NULL;
	struct thread_context* sch_ctx = NULL;
	struct tenant_flow_binding *flow_bindings = NULL;
	size_t flow_binding_count = scheduler_num * tenants_num * tenant_shards;

	printf("Welcome to Flex IO SDK packet processing app.\n");
	printf("Workload-affine dispatch: schedulers=%zu workers/scheduler=%zu tenants=%zu shards/tenant=%zu queues/worker=%d flow-rules=%zu\n",
	       scheduler_num, threads_num_per_scheduler, tenants_num,
	       tenant_shards, WORKER_QUEUES_PER_THREAD, flow_binding_count);
	printf("Tenant/shard MAC: base + ((scheduler * tenants + tenant) * shards + shard).\n");

	flow_bindings = calloc(flow_binding_count, sizeof(*flow_bindings));
	if (!flow_bindings) {
		printf("malloc flow bindings failed\n");
		return -1;
	}

	thd_ctx = calloc(threads_num, sizeof(struct thread_context));
	if (thd_ctx == NULL) {
		printf("malloc thread context failed\n");
		return -1;
	}
	for (int i = 0; i < threads_num; i++) {
		thd_ctx[i].queues = calloc(1, sizeof(struct flexio_queues));
		if (thd_ctx[i].queues == NULL) {
			printf("malloc queue context failed\n");
			return -1;
		}
		thd_ctx[i].num_queues = 1;
	}

	sch_ctx = calloc(scheduler_num, sizeof(struct thread_context));
	if (sch_ctx == NULL) {
		printf("malloc scheduler context failed\n");
		return -1;
	}
	for (int i = 0; i < scheduler_num; i++) {
		sch_ctx[i].queues = calloc(scheduler_queue_count, sizeof(struct flexio_queues));
		if (sch_ctx[i].queues == NULL) {
			printf("malloc scheduler queue context failed\n");
			return -1;
		}
		sch_ctx[i].num_queues = scheduler_queue_count;
	}

	if (geteuid()) {
		printf("Failed - the application must run with root privileges\n");
		return -1;
	}

	err = app_open_ibv_ctx(&(app_ctx), argv[1]);
	if (err) {
		printf("Failed to open ibv context.\n");
		return -1;
		goto cleanup;
	}

	app_ctx.process_pd = ibv_alloc_pd(app_ctx.ibv_ctx);
	if (app_ctx.process_pd == NULL) {
		printf("Failed to create pd.\n");
		err = -1;
		goto cleanup;
	}

	if (flexio_process_create(app_ctx.ibv_ctx, DEV_APP_NAME, &process_attr, &(app_ctx.flexio_process))) {
		printf("Failed to create Flex IO process.\n");
		err = -1;
		goto cleanup;
	}

	ret = flexio_window_create(app_ctx.flexio_process, app_ctx.process_pd, &(app_ctx.flexio_window));
	if (ret != FLEXIO_STATUS_SUCCESS) {
		printf("Failed to create FlexIO window\n");
		err = -1;
		goto cleanup;
	}

	app_ctx.process_uar = flexio_process_get_uar(app_ctx.flexio_process);

	flexio_msg_stream_attr_t stream_fattr = {0};
	stream_fattr.uar = app_ctx.process_uar;
	stream_fattr.data_bsize = 4 * 2048;
	stream_fattr.sync_mode = FLEXIO_LOG_DEV_SYNC_MODE_SYNC;
	stream_fattr.level = FLEXIO_MSG_DEV_DEBUG;
	stream_fattr.stream_name = "Default Stream";
	stream_fattr.mgmt_affinity.type = FLEXIO_AFFINITY_NONE;
	if (flexio_msg_stream_create(app_ctx.flexio_process, &stream_fattr, stdout, NULL,
						&(app_ctx.stream))) {
		printf("Failed to init device messaging environment, exiting App\n");
		err = -1;
		goto cleanup;
	}

	app_ctx.rx_matcher = create_matcher_rx(app_ctx.ibv_ctx);
#ifndef nic_mode
	app_ctx.tx_matcher = create_matcher_tx(app_ctx.ibv_ctx);
#endif


#define THREAD_NUM 190
#define THREAD_RUNNING_BITMAP 32
	/* Create thread running bitmap on the DPA heap */
	// flexio_buf_dev_alloc(app_ctx.flexio_process, THREAD_RUNNING_BITMAP, &app_ctx.dpa_thread_running_bm_daddr);

	for (int i = 0; i < scheduler_num; i++) {
		struct flexio_event_handler_attr handler_attr = {0};
		handler_attr.host_stub_func = flexio_scheduler_handle;
		handler_attr.affinity.type = FLEXIO_AFFINITY_STRICT;
		handler_attr.affinity.id = begin_schedr + i;

		ret = flexio_event_handler_create(app_ctx.flexio_process, &handler_attr, &(sch_ctx[i].event_handler));
		if (ret != FLEXIO_STATUS_SUCCESS) {
			printf("Fail tp create event handler.\n");
			goto cleanup;
		}
		sch_ctx[i].thd_id = i;
		if (alloc_context_host_memory(&app_ctx, &(sch_ctx[i]), 0)) {
			err = -1;
			goto cleanup;
		}
		if (create_app_rq(&(app_ctx), &(sch_ctx[i]), buffer_location)) {
			printf("Failed to create Flex EQ.\n");
			err = -1;
			goto cleanup;
		}
		if (create_app_sq(&(app_ctx), &(sch_ctx[i]), buffer_location, use_copy)) {
			printf("Failed to create Flex SQ.\n");
			err = -1;
			goto cleanup;
		}


		for (uint32_t j = 0; j < sch_ctx[i].num_queues; j++) {
			sch_ctx[i].queues[j].rq_tir_obj = flexio_rq_get_tir(sch_ctx[i].queues[j].flexio_rq_ptr);
			if (sch_ctx[i].queues[j].rq_tir_obj == NULL) {
				printf("Fail creating rq_tir_obj (errno %d)\n", errno);
				goto cleanup;
			}
		}

		for (uint32_t tenant = 0; tenant < tenants_num; tenant++) {
			for (uint32_t shard = 0; shard < tenant_shards; shard++) {
				size_t binding_idx =
					((size_t)i * tenants_num + tenant) * tenant_shards + shard;
				uint32_t worker = shard % threads_num_per_scheduler;
				uint32_t lane = (tenant +
					(shard / threads_num_per_scheduler)) %
					WORKER_QUEUES_PER_THREAD;
				uint32_t queue_idx =
					worker * WORKER_QUEUES_PER_THREAD + lane;
				uint64_t cur_dmac = DMAC + binding_idx;
				struct tenant_flow_binding *binding = &flow_bindings[binding_idx];

				binding->dmac = cur_dmac;
				binding->scheduler_id = i;
				binding->tenant_id = tenant;
				binding->worker_id = worker;
				binding->rx_rule = create_rule_rx_mac_match(
					app_ctx.rx_matcher,
					sch_ctx[i].queues[queue_idx].rq_tir_obj,
					cur_dmac, tenant + 1);
				binding->tx_table_rule = create_rule_tx_fwd_to_sws_table(
					app_ctx.tx_matcher, cur_dmac);
				binding->tx_vport_rule = create_rule_tx_fwd_to_vport(
					app_ctx.tx_matcher, cur_dmac);
				if (!binding->rx_rule || !binding->tx_table_rule ||
				    !binding->tx_vport_rule) {
					printf("Failed to create flow binding for scheduler=%d tenant=%u shard=%u\n",
					       i, tenant, shard);
					err = -1;
					goto cleanup;
				}
			}
		}
		printf("scheduler %d: %u queues, %zu tenant/shard bindings, DMAC 0x%012" PRIx64 "..0x%012" PRIx64 "\n",
		       i, sch_ctx[i].num_queues, tenants_num * tenant_shards,
		       DMAC + (uint64_t)i * tenants_num * tenant_shards,
		       DMAC + (uint64_t)(i + 1) * tenants_num * tenant_shards - 1);

		if (copy_sch_data_to_dpa(&app_ctx, &(sch_ctx[i]), buffer_location, use_copy)) {
			printf("Failed to copy application data to DPA.\n");
			err = -1;
			goto cleanup;
		}

		if (flexio_event_handler_run(sch_ctx[i].event_handler, sch_ctx[i].app_data_daddr)) {
			printf("Failed to run event handler.\n");
			err = -1;
			goto cleanup;
		}
	}

	for (int i = 0; i < threads_num; i++) {
		struct flexio_event_handler_attr handler_attr = {0};
		uint64_t rpc_ret_val = 0;

		handler_attr.host_stub_func = buffer_location ?
					      flexio_pp_dev_worker_host :
					      flexio_pp_dev_worker;

        handler_attr.affinity.type = FLEXIO_AFFINITY_STRICT;
		handler_attr.affinity.id = i + begin_worker;

        ret = flexio_event_handler_create(app_ctx.flexio_process, &handler_attr, &(thd_ctx[i].event_handler));
        if (ret != FLEXIO_STATUS_SUCCESS) {
			printf("Fail tp create event handler.\n");
			goto cleanup;
		}
		thd_ctx[i].thd_id = i;
		if (alloc_context_host_memory(&app_ctx, &(thd_ctx[i]), NVME_QUEUE_MEMORY_SIZE)) {
			err = -1;
			goto cleanup;
		}

		if (create_app_rq(&(app_ctx), &(thd_ctx[i]), buffer_location)) {
			printf("Failed to create Flex EQ.\n");
			err = -1;
			goto cleanup;
		}

		if (create_app_sq(&(app_ctx), &(thd_ctx[i]), buffer_location, use_copy)) {
			printf("Failed to create Flex SQ.\n");
			err = -1;
			goto cleanup;
		}

		if (copy_thd_data_to_dpa(&app_ctx, &(thd_ctx[i]), buffer_location, use_copy)) {
			printf("Failed to copy application data to DPA.\n");
			err = -1;
			goto cleanup;
		}
		
		flexio_process_call(app_ctx.flexio_process, &thd_ctx_init, &rpc_ret_val, thd_ctx[i].app_data_daddr);

		if (flexio_event_handler_run(thd_ctx[i].event_handler, thd_ctx[i].app_data_daddr)) {
			printf("Failed to run event handler.\n");
			err = -1;
			goto cleanup;
		}

	}

	listen_for_qos_requests(&app_ctx);

cleanup:
	/* Clean up flow is done in reverse order of creation as there's a refernce system
	 * that won't allow destroying resources that has references to existing resources.
	 */

	for (size_t i = 0; i < threads_num; i++) {    
        if (thd_ctx[i].app_data_daddr && flexio_buf_dev_free(app_ctx.flexio_process, thd_ctx[i].app_data_daddr)) {
    	    printf("Failed to dealloc application data memory on Flex IO heap\n");
        }
    }
	for (size_t i = 0; i < scheduler_num; i++) {    
		if (sch_ctx[i].app_data_daddr && flexio_buf_dev_free(app_ctx.flexio_process, sch_ctx[i].app_data_daddr)) {
		    printf("Failed to dealloc application data memory on Flex IO heap\n");
		}
	}

	for (size_t i = 0; i < flow_binding_count; i++) {
		if (flow_bindings[i].rx_rule && destroy_rule(flow_bindings[i].rx_rule)) {
			printf("Failed to destroy tenant rx rule\n");
		}
		if (flow_bindings[i].tx_table_rule &&
		    destroy_rule(flow_bindings[i].tx_table_rule)) {
			printf("Failed to destroy tenant tx table rule\n");
		}
		if (flow_bindings[i].tx_vport_rule &&
		    destroy_rule(flow_bindings[i].tx_vport_rule)) {
			printf("Failed to destroy tenant tx vport rule\n");
		}
	}

	for (size_t i = 0; i < scheduler_num; i++) { 
		for (size_t j = 0; j < sch_ctx[i].num_queues; j++) { 
			/* Clean up rx rule if created */
			if (sch_ctx[i].queues[j].rx_flow_rule) {
				if (destroy_rule(sch_ctx[i].queues[j].rx_flow_rule)) {
					printf("Failed to destroy rx rule\n");
				}
			}
			if (sch_ctx[i].queues[j].tx_flow_rule) {
				if (destroy_rule(sch_ctx[i].queues[j].tx_flow_rule)) {
					printf("Failed to destroy tx rule\n");
				}
			}
			if (sch_ctx[i].queues[j].tx_flow_rule2) {
				if (destroy_rule(sch_ctx[i].queues[j].tx_flow_rule2)) {
					printf("Failed to destroy tx rule2\n");
				}
			}
		}
	}

	for (size_t i = 0; i < threads_num; i++) { 
		for (size_t j = 0; j < thd_ctx[i].num_queues; j++) {
	        /* Clean up rx rule if created */
	        if (thd_ctx[i].queues[j].rx_flow_rule) {
	            if (destroy_rule(thd_ctx[i].queues[j].rx_flow_rule)) {
	                printf("Failed to destroy rx rule\n");
	            }
	        }
			if (thd_ctx[i].queues[j].tx_flow_rule) {
	            if (destroy_rule(thd_ctx[i].queues[j].tx_flow_rule)) {
	                printf("Failed to destroy tx rule\n");
	            }
	        }
			if (thd_ctx[i].queues[j].tx_flow_rule2) {
	            if (destroy_rule(thd_ctx[i].queues[j].tx_flow_rule2)) {
	                printf("Failed to destroy tx rule2\n");
	            }
	        }
		}
    }

    if (app_ctx.rx_matcher && destroy_matcher(app_ctx.rx_matcher)) {
        printf("Failed to destroy rx matcher\n");
    }

    if (app_ctx.tx_matcher && destroy_matcher(app_ctx.tx_matcher)) {
        printf("Failed to destroy tx matcher\n");
    }

	for (size_t i = 0; i < threads_num; i++) {
		/* Clean up previously allocated SQ */
		if (clean_up_app_sq(&app_ctx, &(thd_ctx[i]))) {
            printf("Failed to destroy sq\n");
		}

		/* Clean up previously allocated RQ */
		if (clean_up_app_rq(&app_ctx, &(thd_ctx[i]))) {
            printf("Failed to destroy cq\n");
		}
		if (thd_ctx[i].event_handler && flexio_event_handler_destroy(thd_ctx[i].event_handler)) {
            printf("Failed to destroy event handler\n");
		}
	}

	for (size_t i = 0; i < scheduler_num; i++) {
		/* Clean up previously allocated SQ */
		if (clean_up_app_sq(&app_ctx, &(sch_ctx[i]))) {
			printf("Failed to destroy sq\n");
		}

		/* Clean up previously allocated RQ */
		if (clean_up_app_rq(&app_ctx, &(sch_ctx[i]))) {
			printf("Failed to destroy cq\n");
		}
		if (sch_ctx[i].event_handler && flexio_event_handler_destroy(sch_ctx[i].event_handler)) {
			printf("Failed to destroy event handler\n");
		}
	}

	if (app_ctx.stream && flexio_msg_stream_destroy(app_ctx.stream)) {
		printf("Failed to destroy device messaging environment\n");
	}

	if (app_ctx.flexio_window && flexio_window_destroy(app_ctx.flexio_window)) {
		printf("Failed to destroy window.\n");
	}

	if (app_ctx.flexio_process && flexio_process_destroy(app_ctx.flexio_process)) {
		printf("Failed to destroy process.\n");
	}

	/* Close the IBV device */
	if (app_ctx.ibv_ctx && ibv_close_device(app_ctx.ibv_ctx)) {
		printf("Failed to close ibv context.\n");
	}

	free(flow_bindings);

	return err;
}
