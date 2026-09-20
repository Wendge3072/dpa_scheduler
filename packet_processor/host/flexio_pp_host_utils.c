#include <malloc.h>
#include <stdint.h>
#include <assert.h>

#include "flexio_pp_host_utils.h"

int app_open_ibv_ctx(struct app_context *app_ctx, char *device)
{
	/* Queried IBV device list. */
	struct ibv_device **dev_list;
	/* Fucntion return value. */
	int ret = 0;
	/* IBV device iterator. */
	int dev_i;

	/* Query IBV devices list. */
	dev_list = ibv_get_device_list(NULL);
	if (!dev_list) {
		printf("Failed to get IB devices list\n");
		return -1;
	}

	/* Loop over found IBV devices. */
	for (dev_i = 0; dev_list[dev_i]; dev_i++) {
		/* Look for a device with the user provided name. */
		if (!strcmp(ibv_get_device_name(dev_list[dev_i]), device))
			break;
	}

	/* Check a device was found. */
	if (!dev_list[dev_i]) {
		printf("No IBV device found for device name '%s'\n", device);
		ret = -1;
		goto cleanup;
	}

	/* Open IBV device context for the requested device. */
	app_ctx->ibv_ctx = ibv_open_device(dev_list[dev_i]);
	if (!app_ctx->ibv_ctx) {
		printf("Couldn't open an IBV context for device '%s'\n", device);
		ret = -1;
	}

cleanup:
	/* Free queried IBV devices list. */
	ibv_free_device_list(dev_list);

	return ret;
}

/* Creates an MKey with proper permissions for access from DPA.
 * For this application, we only need memory write access.
 * Returns pointer to flexio_mkey structure on success. Otherwise, returns NULL.
 * app_ctx - pointer to app_context structure.
 * daddr - address of MKEY data.
 */
static struct flexio_mkey *create_dpa_mkey(struct app_context *app_ctx, flexio_uintptr_t daddr)
{
	/* Flex IO MKey attributes. */
	struct flexio_mkey_attr mkey_attr = {0};
	/* Flex IO MKey. */
	struct flexio_mkey *mkey;

	/* Set MKey protection domain (PD) to the Flex IO process PD. */
	mkey_attr.pd = app_ctx->process_pd;
	/* Set MKey address. */
	mkey_attr.daddr = daddr;
	/* Set MKey length. */
	mkey_attr.len = Q_DATA_BSIZE;
	/* Set MKey access to memory write (from DPA). */
	mkey_attr.access = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE;
	/* Create Flex IO MKey. */
	if (flexio_device_mkey_create(app_ctx->flexio_process, &mkey_attr, &mkey)) {
		printf("Failed to create Flex IO Mkey\n");
		return NULL;
	}

	return mkey;
}

/* Allocate and initialize DPA heap memory for CQ.
 * Returns 0 on success and -1 if the allocation fails.
 * process - pointer to the previously allocated process information.
 * cq_transf - structure with allocated DPA buffers for CQ.
 */
static int cq_mem_alloc(struct flexio_process *process, struct app_transfer_cq *cq_transf)
{
	/* Pointer to the CQ ring source memory on the host (to copy). */
	struct mlx5_cqe64 *cq_ring_src;
	/* Temp pointer to an iterator for CQE initialization. */
	struct mlx5_cqe64 *cqe;

	/* DBR source memory on the host (to copy). */
	__be32 dbr[2] = { 0, 0 };
	/* Function return value. */
	int ret = 0;
	/* Iterator for CQE initialization. */
	uint32_t i;

	/* Allocate and initialize CQ DBR memory on the DPA heap memory. */
	if (flexio_copy_from_host(process, dbr, sizeof(dbr), &cq_transf->cq_dbr_daddr)) {
		printf("Failed to allocate CQ DBR memory on DPA heap.\n");
		return -1;
	}

	/* Allocate memory for the CQ ring on the host. */
	cq_ring_src = calloc(Q_DEPTH, CQE_BSIZE);
	if (!cq_ring_src) {
		printf("Failed to allocate memory for cq_ring_src.\n");
		return -1;
	}

	/* Init CQEs and set ownership bit. */
	for (i = 0, cqe = cq_ring_src; i < Q_DEPTH; i++)
		mlx5dv_set_cqe_owner(cqe++, 1);

	/* Allocate and copy the initialized CQ ring from host to DPA heap memory. */
	if (flexio_copy_from_host(process, cq_ring_src, CQ_BSIZE, &cq_transf->cq_ring_daddr)) {
		printf("Failed to allocate CQ ring memory on DPA heap.\n");
		ret = -1;
	}

	/* Free CQ ring source memory from host once copied to DPA. */
	free(cq_ring_src);

	return ret;
}

/* Allocate DPA heap memory for SQ.
 * Returns 0 on success and -1 if the allocation fails.
 * process - pointer to the previously allocated process info.
 * sq_transf - structure with allocated DPA buffers for SQ.
 */
static int sq_mem_alloc(struct app_context* app_ctx, struct thread_context* thd_ctx, struct flexio_process *process,
	struct app_transfer_wq *sq_transf, int index, size_t buffer_location, size_t use_copy)
{
	struct flexio_queues *queue = &thd_ctx->queues[index];
	size_t same_queue = (use_copy == 0);

	queue->sq_data_reuses_rq = same_queue;
	if (same_queue) {
		sq_transf->wqd_daddr = queue->rq_transf.wqd_daddr;
		sq_transf->wqd_mkey_id = queue->rq_transf.wqd_mkey_id;
		queue->sqd_mkey = queue->rqd_mkey;
		queue->sq_data_on_host = queue->rq_data_on_host;
	} else if (buffer_location == 0) {
		/* Allocate DPA heap memory for SQ data. */
		flexio_buf_dev_alloc(process, Q_DATA_BSIZE, &sq_transf->wqd_daddr);
		if (!sq_transf->wqd_daddr)
			return -1;
		/* Create an MKey for SQ data buffer to send. */
		queue->sqd_mkey = create_dpa_mkey(app_ctx, sq_transf->wqd_daddr);
		if (!queue->sqd_mkey) {
			printf("Failed to create an MKey for SQ data buffer\n");
			return -1;
		}
		/* Set SQ's data buffer MKey ID in communication struct. */
		sq_transf->wqd_mkey_id = flexio_mkey_get_id(queue->sqd_mkey);
		queue->sq_data_on_host = 0;
	} else {
		if (!queue->host_sq_buffer || !queue->host_sq_mkey_id) {
			printf("Missing host SQ buffer for queue %d\n", index);
			return -1;
		}
		sq_transf->wqd_daddr = (flexio_uintptr_t)queue->host_sq_buffer;
		sq_transf->wqd_mkey_id = queue->host_sq_mkey_id;
		queue->sq_data_on_host = 1;
	}
	/* Allocate DPA heap memory for SQ ring. */
	flexio_buf_dev_alloc(process, SQ_RING_BSIZE, &sq_transf->wq_ring_daddr);
	if (!sq_transf->wq_ring_daddr)
		return -1;

	return 0;
}

/* Allocate DPA heap memory for SQ.
 * Returns 0 on success and -1 if the allocation fails.
 * process - pointer to the previously allocated process info.
 * rq_transf - structure with allocated DPA buffers for RQ.
 */
static int rq_mem_alloc(struct app_context* app_ctx, struct thread_context* thd_ctx, struct flexio_process *process,
	struct app_transfer_wq *rq_transf, int index, size_t buffer_location)
{	
	/* DBR source memory on the host (to copy). */
	__be32 dbr[2] = { 0, 0 };
	struct flexio_queues *queue = &thd_ctx->queues[index];

	if (buffer_location == 0) {
		/* Allocate DPA heap memory for RQ data. */
		flexio_buf_dev_alloc(process, Q_DATA_BSIZE, &rq_transf->wqd_daddr);
		if (!rq_transf->wqd_daddr)
			return -1;
		/* Create an MKey for RX buffer */
		queue->rqd_mkey = create_dpa_mkey(app_ctx, queue->rq_transf.wqd_daddr);
		if (!queue->rqd_mkey) {
			printf("Failed to create an MKey for RQ data buffer.\n");
			return -1;
		}
		queue->rq_transf.wqd_mkey_id = flexio_mkey_get_id(queue->rqd_mkey);
		if (!queue->rq_transf.wqd_mkey_id) {
			printf("Failed to get mkey id for RQ data buffer.\n");
			return -1;
		}
		queue->rq_data_on_host = 0;
	} else {
		if (!queue->host_rq_buffer || !queue->host_rq_mkey_id) {
			printf("Missing host RQ buffer for queue %d\n", index);
			return -1;
		}
		queue->rq_transf.wqd_daddr = (flexio_uintptr_t)queue->host_rq_buffer;
		queue->rq_transf.wqd_mkey_id = queue->host_rq_mkey_id;
		queue->rq_data_on_host = 1;
	}
	/* Allocate DPA heap memory for RQ ring. */
	flexio_buf_dev_alloc(process, RQ_RING_BSIZE, &rq_transf->wq_ring_daddr);
	if (!rq_transf->wq_ring_daddr) {
		return -1;
	}

	/* Allocate and initialize RQ DBR memory on the DPA heap memory. */
	flexio_copy_from_host(process, dbr, sizeof(dbr), &rq_transf->wq_dbr_daddr);
	if (!rq_transf->wq_dbr_daddr)
		return -1;

	return 0;
}

int create_app_sq(struct app_context *app_ctx, struct thread_context* thd_ctx,
	size_t buffer_location, size_t use_copy)
{
	/* Pointer to the application Flex IO process (ease of use). */
	struct flexio_process *app_fp = app_ctx->flexio_process;
	/* Attributes for the SQ's CQ. */
	struct flexio_cq_attr sqcq_attr = {0};
	/* Attributes for the SQ. */
	struct flexio_wq_attr sq_attr = {0};

	/* UAR ID for CQ/SQ from Flex IO process UAR. */
	uint32_t uar_id = flexio_uar_get_id(app_ctx->process_uar);
	/* SQ's CQ number. */
	uint32_t cq_num;
	for (int i = 0; i < thd_ctx->num_queues; i++) {
		/* Allocate CQ memory (ring and DBR) on DPA heap memory. */
		if (cq_mem_alloc(app_fp, &thd_ctx->queues[i].sq_cq_transf)) {
			printf("Failed to alloc memory for SQ's CQ.\n");
			return -1;
		}

		/* Set CQ depth (log) attribute. */
		sqcq_attr.log_cq_depth = LOG_Q_DEPTH;
		/* Set CQ element type attribute to 'non DPA CQ'.
		* This means this CQ will not be attached to an event handler.
		*/
		sqcq_attr.element_type = FLEXIO_CQ_ELEMENT_TYPE_NON_DPA_CQ;
		/* Set CQ UAR ID attribute to the Flex IO process UAR ID.
		* This will allow updating/arming the CQ from the DPA side.
		*/
		sqcq_attr.uar_id = uar_id;
		/* Set CQ DBR memory. DBR memory is on the DPA side in order to allow direct access from
		* DPA.
		*/
		sqcq_attr.cq_dbr_daddr = thd_ctx->queues[i].sq_cq_transf.cq_dbr_daddr;
		/* Set CQ ring memory. Ring memory is on the DPA side in order to allow reading CQEs from
		* DPA during packet forwarding.
		*/
		sqcq_attr.cq_ring_qmem.daddr = thd_ctx->queues[i].sq_cq_transf.cq_ring_daddr;
		/* Create CQ for SQ. */
		if (flexio_cq_create(app_fp, NULL, &sqcq_attr, &thd_ctx->queues[i].flexio_sq_cq_ptr)) {
			printf("Failed to create Flex IO CQ\n");
			return -1;
		}

		/* Fetch SQ's CQ number to communicate to DPA side. */
		cq_num = flexio_cq_get_cq_num(thd_ctx->queues[i].flexio_sq_cq_ptr);
		/* Set SQ's CQ number in communication struct. */
		thd_ctx->queues[i].sq_cq_transf.cq_num = cq_num;
		/* Set SQ's CQ depth in communication struct. */
		thd_ctx->queues[i].sq_cq_transf.log_cq_depth = LOG_Q_DEPTH;
		/* Allocate SQ memory (ring and data). */
		if (sq_mem_alloc(app_ctx, thd_ctx, app_fp, &thd_ctx->queues[i].sq_transf,
				 i, buffer_location, use_copy)) {
			printf("Failed to allocate memory for SQ\n");
			return -1;
		}

		/* Set SQ depth (log) attribute. */
		sq_attr.log_wq_depth = LOG_Q_DEPTH;
		/* Set SQ UAR ID attribute to the Flex IO process UAR ID.
		* This will allow writing doorbells to the SQ from the DPA side.
		*/
		sq_attr.uar_id = uar_id;
		/* Set SQ ring memory. Ring memory is on the DPA side in order to allow writing WQEs from
		* DPA during packet forwarding.
		*/
		sq_attr.wq_ring_qmem.daddr = thd_ctx->queues[i].sq_transf.wq_ring_daddr;

		/* Set SQ protection domain */
		sq_attr.pd = app_ctx->process_pd;

		sq_attr.sq.allow_multi_pkt_send_wqe = 1;

		/* Create SQ.
		* Second argument is NULL as SQ is created on the same GVMI as the process.
		*/
		if (flexio_sq_create(app_fp, NULL, cq_num, &sq_attr, &thd_ctx->queues[i].flexio_sq_ptr)) {
			printf("Failed to create Flex IO SQ\n");
			return -1;
		}

		/* Fetch SQ's number to communicate to DPA side. */
		thd_ctx->queues[i].sq_transf.wq_num = flexio_sq_get_wq_num(thd_ctx->queues[i].flexio_sq_ptr);
	}

	return 0;
}

/* Initialize an RQ ring memory over the DPA heap memory.
 * RQ WQEs need to be initialized (produced) by SW so they are ready for incoming packets.
 * The WQEs are initialized over temporary host memory and then copied to the DPA.
 * Returns 0 on success and -1 if the allocation fails.
 * app_ctx - app_ctx - pointer to app_context structure.
 */
static int init_dpa_rq_ring(struct app_context *app_ctx, struct thread_context *thd_ctx, int index)
{
	/* RQ WQE data iterator. */
	flexio_uintptr_t wqe_data_daddr = thd_ctx->queues[index].rq_transf.wqd_daddr;
	/* RQ ring MKey. */
	uint32_t mkey_id = thd_ctx->queues[index].rq_transf.wqd_mkey_id;
	/* Temporary host memory for RQ ring. */
	struct mlx5_wqe_data_seg *rx_wqes;
	/* RQ WQE iterator. */
	struct mlx5_wqe_data_seg *dseg;
	/* Function return value. */
	int retval = 0;
	/* RQ WQE index iterator. */
	uint32_t i;

	/* Allocate temporary host memory for RQ ring.*/
	rx_wqes = calloc(1, RQ_RING_BSIZE);
	if (!rx_wqes) {
		printf("Failed to allocate memory for rx_wqes\n");
		return -1;
	}

	/* Initialize RQ WQEs'. */
	for (i = 0, dseg = rx_wqes; i < Q_DEPTH; i++, dseg++) {
		/* Set WQE's data segment to point to the relevant RQ data segment. */
		mlx5dv_set_data_seg(dseg, Q_DATA_ENTRY_BSIZE, mkey_id, wqe_data_daddr);
		/* Advance data pointer to next segment. */
		wqe_data_daddr += Q_DATA_ENTRY_BSIZE;
	}

	/* Copy RX WQEs from host to RQ ring DPA heap memory. */
	if (flexio_host2dev_memcpy(app_ctx->flexio_process, rx_wqes, RQ_RING_BSIZE,
				   thd_ctx->queues[index].rq_transf.wq_ring_daddr)) {
		retval = -1;
	}

	/* Free temporary host memory. */
	free(rx_wqes);
	return retval;
}

/* Initialize RQ's DBR.
 * Recieve counter need to be set to number of produces WQEs.
 * Returns 0 on success and -1 if the allocation fails.
 * app_ctx - app_ctx - pointer to app_context structure.
 */
static int init_rq_dbr(struct app_context *app_ctx, struct thread_context * thd_ctx, int index)
{
	/* Temporary host memory for DBR value. */
	__be32 dbr[2];

	/* Set receiver counter to number of WQEs. */
	dbr[0] = htobe32(Q_DEPTH & 0xffff);
	/* Send counter is not used for RQ so it is nullified. */
	dbr[1] = htobe32(0);
	/* Copy DBR value to DPA heap memory.*/
	if (flexio_host2dev_memcpy(app_ctx->flexio_process, dbr, sizeof(dbr),
				thd_ctx->queues[index].rq_transf.wq_dbr_daddr)) {
		return -1;
	}

	return 0;
}


int create_app_rq(struct app_context *app_ctx, struct thread_context* thd_ctx,
	size_t buffer_location)
{
	/* Pointer to the application Flex IO process (ease of use). */
	struct flexio_process *app_fp = app_ctx->flexio_process;
	/* Attributes for the RQ's CQ. */
	struct flexio_cq_attr rqcq_attr = {0};
	/* Attributes for the RQ. */
	struct flexio_wq_attr rq_attr = {0};

	/* UAR ID for CQ/SQ from Flex IO process UAR. */
	uint32_t uar_id = flexio_uar_get_id(app_ctx->process_uar);
	/* RQ's CQ number. */
	uint32_t cq_num;


	for (int i = 0; i < thd_ctx->num_queues; i++) {
		/* Allocate CQ memory (ring and DBR) on DPA heap memory. */
		if (cq_mem_alloc(app_fp, &thd_ctx->queues[i].rq_cq_transf)) {
			printf("Failed to alloc memory for RQ's CQ.\n");
			return -1;
		}

		/* Set CQ depth (log) attribute. */
		rqcq_attr.log_cq_depth = LOG_Q_DEPTH;
		/* Set CQ element type attribute to 'DPA thread'.
		* This means that a CQE on this CQ will trigger the connetced DPA thread.
		* This will be used for running the DPA program for each incoming packet on the RQ.
		*/
		rqcq_attr.element_type = FLEXIO_CQ_ELEMENT_TYPE_DPA_THREAD;
		/* Set CQ thread to the application event handler's thread. */
		rqcq_attr.thread = flexio_event_handler_get_thread(thd_ctx->event_handler);
		/* Set CQ UAR ID attribute to the Flex IO process UAR ID.
		* This will allow updating/arming the CQ from the DPA side.
		*/
		rqcq_attr.uar_id = uar_id;
		/* Set CQ DBR memory. DBR memory is on the DPA side in order to allow direct access from
		* DPA.
		*/
		rqcq_attr.cq_dbr_daddr = thd_ctx->queues[i].rq_cq_transf.cq_dbr_daddr;
		/* Set CQ ring memory. Ring memory is on the DPA side in order to allow reading CQEs from
		* DPA during packet forwarding.
		*/
		rqcq_attr.cq_ring_qmem.daddr = thd_ctx->queues[i].rq_cq_transf.cq_ring_daddr;
		/* Create CQ for RQ. */
		if (flexio_cq_create(app_fp, NULL, &rqcq_attr, &thd_ctx->queues[i].flexio_rq_cq_ptr)) {
			printf("Failed to create Flex IO CQ\n");
			return -1;
		}

		/* Fetch SQ's CQ number to communicate to DPA side. */
		cq_num = flexio_cq_get_cq_num(thd_ctx->queues[i].flexio_rq_cq_ptr);
		/* Set RQ's CQ number in communication struct. */
		thd_ctx->queues[i].rq_cq_transf.cq_num = cq_num;
		/* Set RQ's CQ depth in communication struct. */
		thd_ctx->queues[i].rq_cq_transf.log_cq_depth = LOG_Q_DEPTH;
		/* Allocate RQ memory (ring and data). */
		if (rq_mem_alloc(app_ctx, thd_ctx, app_fp, &thd_ctx->queues[i].rq_transf,
				 i, buffer_location)) {
			printf("Failed to allocate memory for RQ.\n");
			return -1;
		}

		// /* Set SQ's data buffer MKey ID in communication struct. */
		// thd_ctx->rq_transf.wqd_mkey_id = flexio_mkey_get_id(thd_ctx->rqd_mkey);
		/* Initialize RQ ring. */
		if (init_dpa_rq_ring(app_ctx, thd_ctx, i)) {
			printf("Failed to init RQ ring.\n");
			return -1;
		}

		/* Set RQ depth (log) attribute. */
		rq_attr.log_wq_depth = LOG_Q_DEPTH;
		/* Set RQ protection domain attribute to be the same as the Flex IO process. */
		rq_attr.pd = app_ctx->process_pd;
		/* Set RQ DBR memory type to DPA heap memory. */
		rq_attr.wq_dbr_qmem.memtype = FLEXIO_MEMTYPE_DPA;
		/* Set RQ DBR memory address. */
		rq_attr.wq_dbr_qmem.daddr = thd_ctx->queues[i].rq_transf.wq_dbr_daddr;
		/* Set RQ ring memory address. */
		rq_attr.wq_ring_qmem.daddr = thd_ctx->queues[i].rq_transf.wq_ring_daddr;
		/* Create the Flex IO RQ.
		* Second argument is NULL as RQ is created on the same GVMI as the process.
		*/
		if (flexio_rq_create(app_fp, NULL, cq_num, &rq_attr, &thd_ctx->queues[i].flexio_rq_ptr)) {
			printf("Failed to create Flex IO RQ.\n");
			return -1;
		}

		/* Fetch RQ's number to communicate to DPA side. */
		thd_ctx->queues[i].rq_transf.wq_num = flexio_rq_get_wq_num(thd_ctx->queues[i].flexio_rq_ptr);
		if (init_rq_dbr(app_ctx, thd_ctx, i)) {
			printf("Failed to init RQ DBR.\n");
			return -1;
		}
		
	}

	return 0;
}

// ----==== h2d data copy functions ====----

int copy_sch_data_to_dpa(struct app_context *app_ctx, struct thread_context *thd_ctx, 
	int buffer_location, size_t use_copy)
{
	/* Size of application information struct. */
	uint64_t struct_bsize = sizeof(struct host2dev_packet_processor_data_sch);
	/* Temporary application information struct to copy. */
	struct host2dev_packet_processor_data_sch *h2d_data;
	/* Function return value. */
	int ret = 0;

	/* Allocate memory for temporary struct to copy. */
	h2d_data = calloc(1, struct_bsize);
	if (!h2d_data) {
		printf("Failed to allocate memory for h2d_data\n");
		return -1;
	}

	for (int i = 0; i < thd_ctx->num_queues; i++) {
		/* Set SQ's CQ information. */
		h2d_data->queues[i].sq_cq_transf = thd_ctx->queues[i].sq_cq_transf;
		/* Set SQ's information. */
		h2d_data->queues[i].sq_transf = thd_ctx->queues[i].sq_transf;
		/* Set RQ's CQ information. */
		h2d_data->queues[i].rq_cq_transf = thd_ctx->queues[i].rq_cq_transf;
		/* Set RQ's information. */
		h2d_data->queues[i].rq_transf = thd_ctx->queues[i].rq_transf;
	}
	/* Set APP data info for first run. */
	// h2d_data->dpa_thread_running_bm_daddr = app_ctx->dpa_thread_running_bm_daddr;
	h2d_data->not_first_run = 0;
	h2d_data->sch_id = thd_ctx->thd_id;
	h2d_data->scheduler_num = scheduler_num;
	h2d_data->threads_num_per_scheduler = threads_num_per_scheduler;
	h2d_data->tenants_num = tenants_num;
	h2d_data->tenant_shards = tenant_shards;
	h2d_data->dmac_base = DMAC +
		(uint64_t)thd_ctx->thd_id * tenants_num * tenant_shards;
	printf("copied schedular id %d\n", h2d_data->sch_id);
	h2d_data->buffer_location = buffer_location;
	h2d_data->use_copy = use_copy;
	h2d_data->window_id = flexio_window_get_id(app_ctx->flexio_window);
	if (h2d_data->window_id == 0) {
		printf("failed to allocate window id.\n");
	}
	h2d_data->result_buffer_mkey_id = thd_ctx->result_buffer_mkey_id;
	h2d_data->result_buffer = thd_ctx->result_buffer;
	h2d_data->num_queues = thd_ctx->num_queues;
	// h2d_data->dpa_result_buffer = thd_ctx->dpa_result_buffer;
	

	/* Copy to DPA heap memory.
	 * Allocated DPA heap memory address will be kept in app_data_daddr.
	 */
	if (flexio_copy_from_host(app_ctx->flexio_process, h2d_data, struct_bsize,
				  &thd_ctx->app_data_daddr)) {
		printf("Failed to copy application information to DPA.\n");
		ret = -1;
	}

	/* Free temporary host memory. */
	free(h2d_data);
	return ret;
}

int copy_thd_data_to_dpa(struct app_context *app_ctx, struct thread_context *thd_ctx, 
	int buffer_location, size_t use_copy)
{
	/* Size of application information struct. */
	uint64_t struct_bsize = sizeof(struct host2dev_packet_processor_data_thd);
	/* Temporary application information struct to copy. */
	struct host2dev_packet_processor_data_thd *h2d_data;
	/* Function return value. */
	int ret = 0;

	/* Allocate memory for temporary struct to copy. */
	h2d_data = calloc(1, struct_bsize);
	if (!h2d_data) {
		printf("Failed to allocate memory for h2d_data\n");
		return -1;
	}

	/* Set SQ's CQ information. */
	h2d_data->sq_cq_transf = thd_ctx->queues[0].sq_cq_transf;
	/* Set SQ's information. */
	h2d_data->sq_transf = thd_ctx->queues[0].sq_transf;
	/* Set RQ's CQ information. */
	h2d_data->rq_cq_transf = thd_ctx->queues[0].rq_cq_transf;
	/* Set RQ's information. */
	h2d_data->rq_transf = thd_ctx->queues[0].rq_transf;
	/* Set APP data info for first run. */
	h2d_data->not_first_run = 0;
	// h2d_data->dpa_thread_running_bm_daddr = app_ctx->dpa_thread_running_bm_daddr;
	h2d_data->thd_id = thd_ctx->thd_id;
	printf("copied thread id %d\n", h2d_data->thd_id);
	h2d_data->buffer_location = buffer_location;
	h2d_data->use_copy = use_copy;
	h2d_data->window_id = flexio_window_get_id(app_ctx->flexio_window);
	if (h2d_data->window_id == 0) {
		printf("failed to allocate window id.\n");
	}
	h2d_data->result_buffer_mkey_id = thd_ctx->result_buffer_mkey_id;
	h2d_data->result_buffer = thd_ctx->result_buffer;
	h2d_data->host_buffer = thd_ctx->host_buffer;
	// h2d_data->dpa_result_buffer = thd_ctx->dpa_result_buffer;
	

	/* Copy to DPA heap memory.
	 * Allocated DPA heap memory address will be kept in app_data_daddr.
	 */
	if (flexio_copy_from_host(app_ctx->flexio_process, h2d_data, struct_bsize,
				  &thd_ctx->app_data_daddr)) {
		printf("Failed to copy application information to DPA.\n");
		ret = -1;
	}
	/* Free temporary host memory. */
	free(h2d_data);
	return ret;
}

// ----==== clean up functions ====----

int clean_up_app_rq(struct app_context* app_ctx, struct thread_context *thd_ctx)
{
	int err = 0;
	for (int i = 0; i < thd_ctx->num_queues; i++) {
		/* Clean up rq pointer if created */
		if (thd_ctx->queues[i].flexio_rq_ptr && flexio_rq_destroy(thd_ctx->queues[i].flexio_rq_ptr)) {
			printf("Failed to destroy RQ\n");
			err = -1;
		}

		/* Clean up memory key for rqd if created */
		if (thd_ctx->queues[i].rqd_mkey && flexio_device_mkey_destroy(thd_ctx->queues[i].rqd_mkey)) {
			printf("Failed to destroy mkey RQD\n");
			err = -1;
		}

		/* Clean up app data daddr if created */
		if (thd_ctx->queues[i].rq_transf.wq_dbr_daddr &&
			flexio_buf_dev_free(app_ctx->flexio_process, thd_ctx->queues[i].rq_transf.wq_dbr_daddr)) {
			printf("Failed to free rq_transf.wq_dbr_daddr\n");
			err = -1;
		}

		/* Clean up wq_ring_daddr for rq_transf if created */
		if (thd_ctx->queues[i].rq_transf.wq_ring_daddr &&
			flexio_buf_dev_free(app_ctx->flexio_process, thd_ctx->queues[i].rq_transf.wq_ring_daddr)) {
			printf("Failed to free rq_transf.wq_ring_daddr\n");
			err = -1;
		}

		if (!thd_ctx->queues[i].rq_data_on_host &&
			thd_ctx->queues[i].rq_transf.wqd_daddr &&
			flexio_buf_dev_free(app_ctx->flexio_process, thd_ctx->queues[i].rq_transf.wqd_daddr)) {
			printf("Failed to free rq_transf.wqd_daddr\n");
			err = -1;
		}

		if (thd_ctx->queues[i].flexio_rq_cq_ptr && flexio_cq_destroy(thd_ctx->queues[i].flexio_rq_cq_ptr)) {
			printf("Failed to destroy RQ' CQ\n");
			err = -1;
		}

		if (thd_ctx->queues[i].rq_cq_transf.cq_ring_daddr &&
			flexio_buf_dev_free(app_ctx->flexio_process, thd_ctx->queues[i].rq_cq_transf.cq_ring_daddr)) {
			printf("Failed to free rq_cq_transf.cq_ring_daddr\n");
			err = -1;
		}

		if (thd_ctx->queues[i].rq_cq_transf.cq_dbr_daddr &&
			flexio_buf_dev_free(app_ctx->flexio_process, thd_ctx->queues[i].rq_cq_transf.cq_dbr_daddr)) {
			printf("Failed to free rq_cq_transf.cq_dbr_daddr\n");
			err = -1;
		}
	}

	return err;
}

int clean_up_app_sq(struct app_context* app_ctx, struct thread_context *thd_ctx)
{
	int err = 0;

	for (int i = 0; i < thd_ctx->num_queues; i++) {
		if (thd_ctx->queues[i].flexio_sq_ptr && flexio_sq_destroy(thd_ctx->queues[i].flexio_sq_ptr)) {
			printf("Failed to destroy SQ\n");
			err = -1;
		}

		if (!thd_ctx->queues[i].sq_data_reuses_rq &&
			thd_ctx->queues[i].sqd_mkey &&
			flexio_device_mkey_destroy(thd_ctx->queues[i].sqd_mkey)) {
			printf("Failed to destroy mkey SQD\n");
			err = -1;
		}

		if (thd_ctx->queues[i].sq_transf.wq_ring_daddr &&
			flexio_buf_dev_free(app_ctx->flexio_process, thd_ctx->queues[i].sq_transf.wq_ring_daddr)) {
			printf("Failed to free sq_transf.wq_ring_daddr\n");
			err = -1;
		}

		if (!thd_ctx->queues[i].sq_data_on_host &&
			!thd_ctx->queues[i].sq_data_reuses_rq &&
			thd_ctx->queues[i].sq_transf.wqd_daddr &&
			flexio_buf_dev_free(app_ctx->flexio_process, thd_ctx->queues[i].sq_transf.wqd_daddr)) {
			printf("Failed to free sq_transf.wqd_daddr\n");
			err = -1;
		}

		if (thd_ctx->queues[i].flexio_sq_cq_ptr && flexio_cq_destroy(thd_ctx->queues[i].flexio_sq_cq_ptr)) {
			printf("Failed to destroy SQ' CQ\n");
			err = -1;
		}

		if (thd_ctx->queues[i].sq_cq_transf.cq_ring_daddr &&
			flexio_buf_dev_free(app_ctx->flexio_process, thd_ctx->queues[i].sq_cq_transf.cq_ring_daddr)) {
			printf("Failed to free sq_cq_transf.cq_ring_daddr\n");
			err = -1;
		}

		if (thd_ctx->queues[i].sq_cq_transf.cq_dbr_daddr &&
			flexio_buf_dev_free(app_ctx->flexio_process, thd_ctx->queues[i].sq_cq_transf.cq_dbr_daddr)) {
			printf("Failed to free sq_cq_transf.cq_dbr_daddr\n");
			err = -1;
		}
	}
	return err;
}

// ----==== mlx5dr flow matcher and rule creation functions ====----

static struct flow_matcher
*create_flow_matcher_sw_steer_rx(struct ibv_context *ibv_ctx,
				 struct mlx5dv_flow_match_parameters *match_mask,
				 enum mlx5dv_dr_domain_type type)
{
	struct flow_matcher *flow_match;

	flow_match = (struct flow_matcher *)calloc(1, sizeof(*flow_match));
	assert(flow_match);

	/* SW steering table and matcher are not used for RX steering */
	flow_match->dr_table_sws = NULL;
	flow_match->dr_matcher_sws = NULL;

	flow_match->dr_domain = mlx5dv_dr_domain_create(ibv_ctx, type);
	if (!flow_match->dr_domain) {
		printf("Fail creating dr_domain (errno %d)\n", errno);
		goto error;
	}

	flow_match->dr_table_root = mlx5dv_dr_table_create(flow_match->dr_domain, 0);
	if (!flow_match->dr_table_root) {
		printf("Fail creating dr_table (errno %d)\n", errno);
		goto error;
	}

	flow_match->dr_matcher_root = mlx5dv_dr_matcher_create(flow_match->dr_table_root, 0,
							       MATCHER_CRITERIA_OUTER, match_mask);
	if (!flow_match->dr_matcher_root) {
		printf("Fail creating dr_matcher (errno %d)\n", errno);
		goto error;
	}
	return flow_match;

error:
	return NULL;
}

static struct flow_matcher
*create_flow_matcher_sw_steer_tx(struct ibv_context *ibv_ctx,
				 struct mlx5dv_flow_match_parameters *match_mask,
				 enum mlx5dv_dr_domain_type type)
{
	struct flow_matcher *flow_match;

	flow_match = (struct flow_matcher *)calloc(1, sizeof(*flow_match));
	assert(flow_match);

	flow_match->dr_domain = mlx5dv_dr_domain_create(ibv_ctx, type);
	if (!flow_match->dr_domain) {
		printf("Fail creating dr_domain (errno %d)\n", errno);
		goto error;
	}

	flow_match->dr_table_root = mlx5dv_dr_table_create(flow_match->dr_domain, 0);
	if (!flow_match->dr_table_root) {
		printf("Fail creating dr_table_root (errno %d)\n", errno);
		goto error;
	}

	flow_match->dr_matcher_root = mlx5dv_dr_matcher_create(flow_match->dr_table_root, 0,
							       MATCHER_CRITERIA_OUTER, match_mask);
	if (!flow_match->dr_matcher_root) {
		printf("Fail creating dr_matcher_root (errno %d)\n", errno);
		goto error;
	}

	flow_match->dr_table_sws = mlx5dv_dr_table_create(flow_match->dr_domain, 1);
	if (!flow_match->dr_table_sws) {
		printf("Fail creating dr_table_sws (errno %d)\n", errno);
		goto error;
	}

	flow_match->dr_matcher_sws = mlx5dv_dr_matcher_create(flow_match->dr_table_sws, 0,
							      MATCHER_CRITERIA_OUTER, match_mask);
	if (!flow_match->dr_matcher_sws) {
		printf("Fail creating dr_matcher_sws (errno %d)\n", errno);
		goto error;
	}

	return flow_match;

error:
	return NULL;
}

static struct flow_rule *create_flow_rule_rx(struct flow_matcher *flow_matcher,
					     struct mlx5dv_devx_obj *tir_obj,
					     struct mlx5dv_flow_match_parameters *match_value)
{
	struct mlx5dv_dr_action *actions[1];
	struct flow_rule *flow_rule;

	flow_rule = (struct flow_rule *)calloc(1, sizeof(*flow_rule));
	assert(flow_rule);

	flow_rule->action = mlx5dv_dr_action_create_dest_devx_tir(tir_obj);
	if (!flow_rule->action) {
		printf("Failed creating TIR action (errno %d).\n", errno);
		goto err_out;
	}
	actions[0] = flow_rule->action;

	flow_rule->dr_rule = mlx5dv_dr_rule_create(flow_matcher->dr_matcher_root, match_value, 1,
						   actions);
	if (!flow_rule->dr_rule) {
		printf("Fail creating dr_rule (errno %d).\n", errno);
		goto err_out;
	}

	return flow_rule;

err_out:
	if (flow_rule->action)
		mlx5dv_dr_action_destroy(flow_rule->action);
	free(flow_rule);
	return NULL;
}

static struct flow_rule *create_flow_rule_tx(struct flow_matcher *flow_matcher,
					     struct mlx5dv_flow_match_parameters *match_value)
{
	struct mlx5dv_dr_action *actions[1];
	struct flow_rule *flow_rule;

	flow_rule = (struct flow_rule *)calloc(1, sizeof(*flow_rule));
	assert(flow_rule);

	flow_rule->action = mlx5dv_dr_action_create_dest_vport(flow_matcher->dr_domain, 0xFFFF);
	if (!flow_rule->action) {
		printf("Failed creating dest vport action (errno %d).\n", errno);
		goto err_out;
	}
	actions[0] = flow_rule->action;

	flow_rule->dr_rule = mlx5dv_dr_rule_create(flow_matcher->dr_matcher_sws, match_value, 1,
						   actions);
	if (!flow_rule->dr_rule) {
		printf("Fail creating dr_rule (errno %d).\n", errno);
		goto err_out;
	}

	return flow_rule;

err_out:
	if (flow_rule->action)
		mlx5dv_dr_action_destroy(flow_rule->action);
	free(flow_rule);
	return NULL;
}

static struct flow_rule *create_flow_rule_tx_table(struct flow_matcher *flow_matcher,
						   struct mlx5dv_flow_match_parameters *match_value)
{
	struct mlx5dv_dr_action *actions[1];
	struct flow_rule *flow_rule;

	flow_rule = (struct flow_rule *)calloc(1, sizeof(*flow_rule));
	assert(flow_rule);

	flow_rule->action = mlx5dv_dr_action_create_dest_table(flow_matcher->dr_table_sws);
	if (!flow_rule->action) {
		printf("Failed creating dest SWS table action (errno %d).\n", errno);
		goto err_out;
	}
	actions[0] = flow_rule->action;

	flow_rule->dr_rule = mlx5dv_dr_rule_create(flow_matcher->dr_matcher_root, match_value, 1,
						   actions);
	if (!flow_rule->dr_rule) {
		printf("Fail creating dr_rule (errno %d).\n", errno);
		goto err_out;
	}

	return flow_rule;

err_out:
	if (flow_rule->action)
		mlx5dv_dr_action_destroy(flow_rule->action);
	free(flow_rule);
	return NULL;
}

struct flow_matcher *create_matcher_rx(struct ibv_context *ibv_ctx)
{
	struct mlx5dv_flow_match_parameters *match_mask;
	struct flow_matcher *matcher;
	int match_mask_size;

	/* mask & match value */
	match_mask_size = sizeof(*match_mask) + MATCH_VAL_BSIZE;
	match_mask = (struct mlx5dv_flow_match_parameters *)calloc(1, match_mask_size);
	assert(match_mask);

	match_mask->match_sz = MATCH_VAL_BSIZE;
	DEVX_SET(dr_match_spec, match_mask->match_buf, dmac_47_16, 0xffffffff);
	DEVX_SET(dr_match_spec, match_mask->match_buf, dmac_15_0, 0xffff);

	matcher = create_flow_matcher_sw_steer_rx(ibv_ctx, match_mask,
						  MLX5DV_DR_DOMAIN_TYPE_NIC_RX);
	free(match_mask);

	return matcher;
}

struct flow_matcher *create_matcher_tx(struct ibv_context *ibv_ctx)
{
	struct mlx5dv_flow_match_parameters *match_mask;
	struct flow_matcher *matcher;
	int match_mask_size;

	/* mask & match value */
	match_mask_size = sizeof(*match_mask) + MATCH_VAL_BSIZE;
	match_mask = (struct mlx5dv_flow_match_parameters *)calloc(1, match_mask_size);
	assert(match_mask);

	match_mask->match_sz = MATCH_VAL_BSIZE;
	DEVX_SET(dr_match_spec, match_mask->match_buf, smac_47_16, 0xffffffff);
	DEVX_SET(dr_match_spec, match_mask->match_buf, smac_15_0, 0xffff);
	matcher = create_flow_matcher_sw_steer_tx(ibv_ctx, match_mask, MLX5DV_DR_DOMAIN_TYPE_FDB);
	free(match_mask);

	return matcher;
}

struct flow_rule *create_rule_rx_mac_match(struct flow_matcher *flow_match,
					   struct mlx5dv_devx_obj *tir_obj, uint64_t dmac)
{
	struct mlx5dv_flow_match_parameters *match_value;
	struct flow_rule *flow_rule;
	int match_value_size;

	/* mask & match value */
	match_value_size = sizeof(*match_value) + MATCH_VAL_BSIZE;
	match_value = (struct mlx5dv_flow_match_parameters *)calloc(1, match_value_size);
	assert(match_value);

	match_value->match_sz = MATCH_VAL_BSIZE;
	DEVX_SET(dr_match_spec, match_value->match_buf, dmac_47_16, dmac >> 16);
	DEVX_SET(dr_match_spec, match_value->match_buf, dmac_15_0, dmac % (1 << 16));
	flow_rule = create_flow_rule_rx(flow_match, tir_obj, match_value);
	free(match_value);

	return flow_rule;
}

struct flow_rule *create_rule_tx_fwd_to_vport(struct flow_matcher *flow_match, uint64_t smac)
{
	struct mlx5dv_flow_match_parameters *match_value;
	struct flow_rule *flow_rule;
	int match_value_size;

	/* mask & match value */
	match_value_size = sizeof(*match_value) + MATCH_VAL_BSIZE;
	match_value = (struct mlx5dv_flow_match_parameters *)calloc(1, match_value_size);
	assert(match_value);

	match_value->match_sz = MATCH_VAL_BSIZE;
	DEVX_SET(dr_match_spec, match_value->match_buf, smac_47_16, smac >> 16);
	DEVX_SET(dr_match_spec, match_value->match_buf, smac_15_0, smac % (1 << 16));
	flow_rule = create_flow_rule_tx(flow_match, match_value);
	free(match_value);

	return flow_rule;
}

struct flow_rule *create_rule_tx_fwd_to_sws_table(struct flow_matcher *flow_match, uint64_t smac)
{
	struct mlx5dv_flow_match_parameters *match_value;
	struct flow_rule *flow_rule;
	int match_value_size;

	/* mask & match value */
	match_value_size = sizeof(*match_value) + MATCH_VAL_BSIZE;
	match_value = (struct mlx5dv_flow_match_parameters *)calloc(1, match_value_size);
	assert(match_value);

	match_value->match_sz = MATCH_VAL_BSIZE;
	DEVX_SET(dr_match_spec, match_value->match_buf, smac_47_16, smac >> 16);
	DEVX_SET(dr_match_spec, match_value->match_buf, smac_15_0, smac % (1 << 16));
	flow_rule = create_flow_rule_tx_table(flow_match, match_value);
	free(match_value);

	return flow_rule;
}

int destroy_matcher(struct flow_matcher *matcher)
{
	int err;

	err = mlx5dv_dr_matcher_destroy(matcher->dr_matcher_root);
	if (err)
		return err;

	err = mlx5dv_dr_table_destroy(matcher->dr_table_root);
	if (err)
		return err;

	if (matcher->dr_matcher_sws) {
		err = mlx5dv_dr_matcher_destroy(matcher->dr_matcher_sws);
		if (err)
			return err;
	}

	if (matcher->dr_table_sws) {
		err = mlx5dv_dr_table_destroy(matcher->dr_table_sws);
		if (err)
			return err;
	}

	err = mlx5dv_dr_domain_destroy(matcher->dr_domain);
	if (err)
		return err;

	free(matcher);

	return 0;
}

int destroy_rule(struct flow_rule *rule)
{
	int err;

	err = mlx5dv_dr_rule_destroy(rule->dr_rule);
	if (err)
		return err;

	err = mlx5dv_dr_action_destroy(rule->action);
	if (err)
		return err;

	free(rule);

	return 0;
}
