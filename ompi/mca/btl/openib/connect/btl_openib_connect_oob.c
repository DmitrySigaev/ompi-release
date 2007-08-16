/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart, 
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2006-2007 Cisco, Inc.  All rights reserved.
 * Copyright (c) 2006      Los Alamos National Security, LLC.  All rights
 *                         reserved. 
 *
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

#include "ompi_config.h"

#include "orte/mca/ns/base/base.h"
#include "orte/mca/oob/base/base.h"
#include "orte/mca/rml/rml.h"
#include "orte/mca/errmgr/errmgr.h"
#include "orte/dss/dss.h"

#include "btl_openib.h"
#include "btl_openib_endpoint.h" 
#include "btl_openib_proc.h"
#include "connect/connect.h"

typedef enum {
    ENDPOINT_CONNECT_REQUEST,
    ENDPOINT_CONNECT_RESPONSE,
    ENDPOINT_CONNECT_ACK
} connect_message_type_t;

#define OOB_TAG (ORTE_RML_TAG_DYNAMIC - 1)

static int oob_init(void);
static int oob_start_connect(mca_btl_base_endpoint_t *e);
static int oob_finalize(void);

static int reply_start_connect(mca_btl_openib_endpoint_t *endpoint,
                               mca_btl_openib_rem_info_t *rem_info);
static int set_remote_info(mca_btl_base_endpoint_t* endpoint,
                           mca_btl_openib_rem_info_t* rem_info);
static int qp_connect_all(mca_btl_base_endpoint_t* endpoint);
static int qp_create_all(mca_btl_base_endpoint_t* endpoint);
static int qp_create_one(mca_btl_base_endpoint_t* endpoint, int prio, int qp);
static int send_connect_data(mca_btl_base_endpoint_t* endpoint, 
                             uint8_t message_type);

static void rml_send_cb(int status, orte_process_name_t* endpoint, 
                        orte_buffer_t* buffer, orte_rml_tag_t tag, 
                        void* cbdata);
static void rml_recv_cb(int status, orte_process_name_t* process_name, 
                        orte_buffer_t* buffer, orte_rml_tag_t tag, 
                        void* cbdata);

/*
 * The "module" struct -- the top-level function pointers for the oob
 * connection scheme.
 */
ompi_btl_openib_connect_base_funcs_t ompi_btl_openib_connect_oob = {
    "oob",
    /* No need for "open */
    NULL,
    /* Init */
    oob_init,
    /* Connect */
    oob_start_connect,
    /* Finalize */
    oob_finalize,
};

/*
 * Init function.  Post non-blocking RML receive to accept incoming
 * connection requests.
 */
static int oob_init(void)
{
    int rc;

    rc = orte_rml.recv_buffer_nb(ORTE_NAME_WILDCARD, 
                                 OOB_TAG,
                                 ORTE_RML_PERSISTENT,
                                 rml_recv_cb,
                                 NULL);
    return (ORTE_SUCCESS == rc) ? OMPI_SUCCESS : rc;
}

/*
 * Connect function.  Start initiation of connections to a remote
 * peer.  We send our Queue Pair information over the RML/OOB
 * communication mechanism.  On completion of our send, a send
 * completion handler is called.
 */
static int oob_start_connect(mca_btl_base_endpoint_t *endpoint)
{
    int rc;
   
    if (OMPI_SUCCESS != (rc = qp_create_all(endpoint))) {
        return rc;
    }

    /* Send connection info over to remote endpoint */
    endpoint->endpoint_state = MCA_BTL_IB_CONNECTING;
    if (OMPI_SUCCESS !=
        (rc = send_connect_data(endpoint, ENDPOINT_CONNECT_REQUEST))) {
        BTL_ERROR(("error sending connect request, error code %d", rc)); 
        return rc;
    }

    return OMPI_SUCCESS;
}

/*
 * Finalize function.  Cleanup RML non-blocking receive.
 */
static int oob_finalize(void)
{
    orte_rml.recv_cancel(ORTE_NAME_WILDCARD, OOB_TAG);
    return OMPI_SUCCESS;
}

/**************************************************************************/

/*
 * Reply to a `start - connect' message
 */
static int reply_start_connect(mca_btl_openib_endpoint_t *endpoint,
                               mca_btl_openib_rem_info_t *rem_info)
{
    int rc;

    BTL_VERBOSE(("Initialized QPs, LID = %d",
                 ((mca_btl_openib_module_t*)endpoint->endpoint_btl)->lid));

    /* Create local QP's and post receive resources */
    if (OMPI_SUCCESS != (rc = qp_create_all(endpoint))) {
        return rc;
    }

    /* Set the remote side info */
    set_remote_info(endpoint, rem_info);
    
    /* Connect to remote endpoint qp's */
    if (OMPI_SUCCESS != (rc = qp_connect_all(endpoint))) {
        return rc;
    }

    /* Send connection info over to remote endpoint */
    endpoint->endpoint_state = MCA_BTL_IB_CONNECT_ACK;
    if (OMPI_SUCCESS !=
        (rc = send_connect_data(endpoint, ENDPOINT_CONNECT_RESPONSE))) {
        BTL_ERROR(("error in endpoint send connect request error code is %d",
                   rc));
        return rc;
    }
    return OMPI_SUCCESS;
}


static int set_remote_info(mca_btl_base_endpoint_t* endpoint,
                           mca_btl_openib_rem_info_t* rem_info)
{
    /* copy the rem_info stuff */
    memcpy(&((mca_btl_openib_endpoint_t*) endpoint)->rem_info, 
           rem_info, sizeof(mca_btl_openib_rem_info_t)); 
    
    /* copy over the rem qp info */
    memcpy(endpoint->rem_info.rem_qps,
           rem_info->rem_qps, sizeof(mca_btl_openib_rem_qp_info_t) * 
           mca_btl_openib_component.num_qps);
    
    BTL_VERBOSE(("Setting QP info,  LID = %d", endpoint->rem_info.rem_lid));
    return ORTE_SUCCESS;

}


/*
 * Connect the local ends of all qp's to the remote side
 */
static int qp_connect_all(mca_btl_openib_endpoint_t *endpoint)
{
    int i;
    struct ibv_qp* qp;
    struct ibv_qp_attr* attr;
    mca_btl_openib_module_t* openib_btl =
        (mca_btl_openib_module_t*)endpoint->endpoint_btl;

    for (i = 0; i < mca_btl_openib_component.num_qps; i++) { 
        qp = endpoint->qps[i].lcl_qp;
        attr = endpoint->qps[i].lcl_qp_attr;
        attr->qp_state = IBV_QPS_RTR; 
        attr->path_mtu = (openib_btl->hca->mtu < endpoint->rem_info.rem_mtu) ? 
            openib_btl->hca->mtu : endpoint->rem_info.rem_mtu;
        if (mca_btl_openib_component.verbose) {
            BTL_OUTPUT(("Set MTU to IBV value %d (%s bytes)", attr->path_mtu,
                        (attr->path_mtu == IBV_MTU_256) ? "256" :
                        (attr->path_mtu == IBV_MTU_512) ? "512" :
                        (attr->path_mtu == IBV_MTU_1024) ? "1024" :
                        (attr->path_mtu == IBV_MTU_2048) ? "2048" :
                        (attr->path_mtu == IBV_MTU_4096) ? "4096" :
                        "unknown (!)"));
        }
        attr->dest_qp_num = endpoint->rem_info.rem_qps[i].rem_qp_num,
        attr->rq_psn = endpoint->rem_info.rem_qps[i].rem_psn,
        attr->max_dest_rd_atomic = mca_btl_openib_component.ib_max_rdma_dst_ops; 
        attr->min_rnr_timer = mca_btl_openib_component.ib_min_rnr_timer; 
        attr->ah_attr.is_global = 0; 
        attr->ah_attr.dlid = endpoint->rem_info.rem_lid,
        attr->ah_attr.sl = mca_btl_openib_component.ib_service_level; 
        attr->ah_attr.src_path_bits = openib_btl->src_path_bits;
        attr->ah_attr.port_num = openib_btl->port_num; 
        /* JMS to be filled in later dynamically */
        attr->ah_attr.static_rate = 0;
        
        if (ibv_modify_qp(qp, attr, 
                          IBV_QP_STATE              |
                          IBV_QP_AV                 |
                          IBV_QP_PATH_MTU           |
                          IBV_QP_DEST_QPN           |
                          IBV_QP_RQ_PSN             |
                          IBV_QP_MAX_DEST_RD_ATOMIC |
                          IBV_QP_MIN_RNR_TIMER)) {
            BTL_ERROR(("error modifing QP to RTR errno says %s",  
                       strerror(errno))); 
            return OMPI_ERROR; 
        }
        attr->qp_state 	     = IBV_QPS_RTS;
        attr->timeout        = mca_btl_openib_component.ib_timeout;
        attr->retry_cnt      = mca_btl_openib_component.ib_retry_count;
        attr->rnr_retry      = mca_btl_openib_component.ib_rnr_retry;
        attr->sq_psn         = endpoint->qps[i].lcl_psn;
        attr->max_rd_atomic  = mca_btl_openib_component.ib_max_rdma_dst_ops;
        if (ibv_modify_qp(qp, attr,
                          IBV_QP_STATE              |
                          IBV_QP_TIMEOUT            |
                          IBV_QP_RETRY_CNT          |
                          IBV_QP_RNR_RETRY          |
                          IBV_QP_SQ_PSN             |
                          IBV_QP_MAX_QP_RD_ATOMIC)) {
            BTL_ERROR(("error modifying QP to RTS errno says %s", 
                       strerror(errno))); 
            return OMPI_ERROR;
        }
    }

    return OMPI_SUCCESS;
}


/*
 * Create the local side of all the qp's.  The remote sides will be
 * connected later.
 */
static int qp_create_all(mca_btl_base_endpoint_t* endpoint)
{
    int qp, rc, prio;

    for (qp = 0; qp < mca_btl_openib_component.num_qps; ++qp) { 
        /* If the size for this qp is <= the eager limit, make it a
           high priority QP.  Otherwise, make it a low priority QP. */
        prio = (mca_btl_openib_component.qp_infos[qp].size <=
                mca_btl_openib_component.eager_limit) ? 
            BTL_OPENIB_HP_CQ : BTL_OPENIB_LP_CQ;
        rc = qp_create_one(endpoint, prio, qp);
        if (OMPI_SUCCESS != rc) {
            return rc;
        }
    }

    /* Now that all the qp's are created locally, post some receive
       buffers, setup credits, etc. */
    return mca_btl_openib_endpoint_post_recvs(endpoint);
}


/*
 * Create the local side of one qp.  The remote side will be connected
 * later.
 */
static int qp_create_one(mca_btl_base_endpoint_t* endpoint, int prio, int qp)
{
    mca_btl_openib_module_t *openib_btl =
        (mca_btl_openib_module_t*)endpoint->endpoint_btl;
    struct ibv_srq *srq = 
        (MCA_BTL_OPENIB_PP_QP == openib_btl->qps[qp].type) ? NULL : 
        openib_btl->qps[qp].u.srq_qp.srq;
    
    /* Create the Queue Pair */
#if 0
    if (OMPI_SUCCESS != (rc = create_qp(openib_btl,
                                        openib_btl->hca->ib_pd,
                                        openib_btl->ib_cq[prio],
                                        srq,
                                        endpoint->qps[qp].lcl_qp_attr,
                                        &endpoint->qps[qp].lcl_qp, 
                                        qp))) {
static int create_qp(
                     struct ibv_qp** qp,
                     int qp_idx);
        BTL_ERROR(("error creating queue pair, error code %d", rc)); 
        return rc;
    }
#endif

    {
        struct ibv_qp* my_qp; 
        struct ibv_qp_init_attr qp_init_attr; 

        memset(&qp_init_attr, 0, sizeof(struct ibv_qp_init_attr)); 

        qp_init_attr.send_cq = 
            qp_init_attr.recv_cq = openib_btl->ib_cq[prio];
        
        if (MCA_BTL_OPENIB_PP_QP == mca_btl_openib_component.qp_infos[qp].type) { 
            qp_init_attr.cap.max_recv_wr =
                mca_btl_openib_component.qp_infos[qp].rd_num +
                mca_btl_openib_component.qp_infos[qp].u.pp_qp.rd_rsv;
            qp_init_attr.cap.max_send_wr =
                mca_btl_openib_component.qp_infos[qp].rd_num + 1;
        } else { 
            qp_init_attr.cap.max_recv_wr =
                mca_btl_openib_component.qp_infos[qp].rd_num;
            qp_init_attr.cap.max_send_wr =
                mca_btl_openib_component.qp_infos[qp].u.srq_qp.sd_max +
                BTL_OPENIB_EAGER_RDMA_QP(qp);
        }
        
        qp_init_attr.cap.max_send_sge = mca_btl_openib_component.ib_sg_list_size;
        qp_init_attr.cap.max_recv_sge = mca_btl_openib_component.ib_sg_list_size;
        qp_init_attr.qp_type = IBV_QPT_RC; 
        qp_init_attr.srq = srq; 
        my_qp = ibv_create_qp(openib_btl->hca->ib_pd, &qp_init_attr); 
    
        if (NULL == my_qp) { 
            BTL_ERROR(("error creating qp errno says %s", strerror(errno))); 
            return OMPI_ERROR; 
        }
        endpoint->qps[qp].lcl_qp = my_qp;
        openib_btl->ib_inline_max = qp_init_attr.cap.max_inline_data; 
    }
    
    {
        endpoint->qps[qp].lcl_qp_attr->qp_state = IBV_QPS_INIT; 
        endpoint->qps[qp].lcl_qp_attr->pkey_index = openib_btl->pkey_index;
        endpoint->qps[qp].lcl_qp_attr->port_num = openib_btl->port_num; 
        endpoint->qps[qp].lcl_qp_attr->qp_access_flags = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ; 

        if (ibv_modify_qp(endpoint->qps[qp].lcl_qp, 
                          endpoint->qps[qp].lcl_qp_attr, 
                          IBV_QP_STATE | 
                          IBV_QP_PKEY_INDEX | 
                          IBV_QP_PORT | 
                          IBV_QP_ACCESS_FLAGS )) { 
            BTL_ERROR(("error modifying qp to INIT errno says %s", strerror(errno))); 
            return OMPI_ERROR; 
        } 
    } 

    /* Setup meta data on the endpoint */
    endpoint->qps[qp].lcl_psn = lrand48() & 0xffffff;
    endpoint->qps[qp].credit_frag = NULL;
    openib_btl->cq_users[prio]++;

    return OMPI_SUCCESS;
}


/*
 * RML send connect information to remote endpoint
 */
static int send_connect_data(mca_btl_base_endpoint_t* endpoint, 
                             uint8_t message_type)
{
    orte_buffer_t* buffer = OBJ_NEW(orte_buffer_t);
    int rc;
    
    if (NULL == buffer) {
         ORTE_ERROR_LOG(ORTE_ERR_OUT_OF_RESOURCE);
         return ORTE_ERR_OUT_OF_RESOURCE;
    }

    /* pack the info in the send buffer */ 
    opal_output(mca_btl_base_output, "packing %d of %d\n", 1, ORTE_UINT8);
    rc = orte_dss.pack(buffer, &message_type, 1, ORTE_UINT8);
    if (ORTE_SUCCESS != rc) {
        ORTE_ERROR_LOG(rc);
        return rc;
    }

    opal_output(mca_btl_base_output, "packing %d of %d\n", 1, ORTE_UINT64);
    rc = orte_dss.pack(buffer, &endpoint->subnet_id, 1, ORTE_UINT64);
    if (ORTE_SUCCESS != rc) {
        ORTE_ERROR_LOG(rc);
        return rc;
    }

    if (message_type != ENDPOINT_CONNECT_REQUEST) {
        /* send the QP connect request info we respond to */
        opal_output(mca_btl_base_output, "packing %d of %d\n", 1, ORTE_UINT32);
        rc = orte_dss.pack(buffer,
                           &endpoint->rem_info.rem_qps[0].rem_qp_num, 1,
                           ORTE_UINT32);
        if (ORTE_SUCCESS != rc) {
            ORTE_ERROR_LOG(rc);
            return rc;
        }
        opal_output(mca_btl_base_output, "packing %d of %d\n", 1, ORTE_UINT16);
        rc = orte_dss.pack(buffer, &endpoint->rem_info.rem_lid, 1, ORTE_UINT16);
        if (ORTE_SUCCESS != rc) {
            ORTE_ERROR_LOG(rc);
            return rc;
        }
    }

    if (message_type != ENDPOINT_CONNECT_ACK) {
        int qp;
        /* stuff all the QP info into the buffer */
        for (qp = 0; qp < mca_btl_openib_component.num_qps; qp++) { 
            opal_output(mca_btl_base_output, "packing %d of %d\n", 1, ORTE_UINT32);
            rc = orte_dss.pack(buffer, &endpoint->qps[qp].lcl_qp->qp_num,
                               1, ORTE_UINT32);
            if (ORTE_SUCCESS != rc) {
                ORTE_ERROR_LOG(rc);
                return rc;
            }
            opal_output(mca_btl_base_output, "packing %d of %d\n", 1, ORTE_UINT32);
            rc = orte_dss.pack(buffer, &endpoint->qps[qp].lcl_psn, 1,
                               ORTE_UINT32); 
            if (ORTE_SUCCESS != rc) {
                ORTE_ERROR_LOG(rc);
                return rc;
            }
        }
        
        opal_output(mca_btl_base_output, "packing %d of %d\n", 1, ORTE_UINT16);
        rc = orte_dss.pack(buffer, &endpoint->endpoint_btl->lid, 1, ORTE_UINT16);
        if (ORTE_SUCCESS != rc) {
            ORTE_ERROR_LOG(rc);
            return rc;
        }
        opal_output(mca_btl_base_output, "packing %d of %d\n", 1, ORTE_UINT32);
        rc = orte_dss.pack(buffer, &endpoint->endpoint_btl->hca->mtu, 1,
                ORTE_UINT32);
        if (ORTE_SUCCESS != rc) {
            ORTE_ERROR_LOG(rc);
            return rc;
        }
        opal_output(mca_btl_base_output, "packing %d of %d\n", 1, ORTE_UINT32);
        rc = orte_dss.pack(buffer, &endpoint->index, 1, ORTE_UINT32);
        if (ORTE_SUCCESS != rc) {
            ORTE_ERROR_LOG(rc);
            return rc;
        }
    }

    /* send to remote endpoint */
    rc = orte_rml.send_buffer_nb(&endpoint->endpoint_proc->proc_guid, 
                                 buffer, OOB_TAG, 0,
                                 rml_send_cb, NULL);
    if (ORTE_SUCCESS != rc) {
        ORTE_ERROR_LOG(rc);
        return rc;
    }
    BTL_VERBOSE(("Sent QP Info, LID = %d, SUBNET = %016x\n",
                 endpoint->endpoint_btl->lid, 
                 endpoint->subnet_id));

    return OMPI_SUCCESS;
}


/*
 * Callback when we have finished RML sending the connect data to a
 * remote peer
 */
static void rml_send_cb(int status, orte_process_name_t* endpoint, 
                        orte_buffer_t* buffer, orte_rml_tag_t tag, 
                        void* cbdata)
{
    OBJ_RELEASE(buffer);
}


/*
 * Non blocking RML recv callback.  Read incoming QP and other info,
 * and if this endpoint is trying to connect, reply with our QP info,
 * otherwise try to modify QP's and establish reliable connection
 */
static void rml_recv_cb(int status, orte_process_name_t* process_name, 
                        orte_buffer_t* buffer, orte_rml_tag_t tag, 
                        void* cbdata)
{
    mca_btl_openib_proc_t *ib_proc;
    mca_btl_openib_endpoint_t *ib_endpoint = NULL;
    int endpoint_state;
    int rc;
    uint32_t i, lcl_qp = 0;
    uint16_t lcl_lid = 0;
    int32_t cnt = 1;
    mca_btl_openib_rem_info_t rem_info;
    uint8_t message_type;
    bool master;
    
    /* start by unpacking data first so we know who is knocking at 
       our door */ 
    opal_output(mca_btl_base_output, "unpacking %d of %d\n", cnt, ORTE_UINT8);
    rc = orte_dss.unpack(buffer, &message_type, &cnt, ORTE_UINT8);
    if (ORTE_SUCCESS != rc) {
        ORTE_ERROR_LOG(rc);
        return;
    }
    
    opal_output(mca_btl_base_output, "unpacking %d of %d\n", cnt, ORTE_UINT64);
    rc = orte_dss.unpack(buffer, &rem_info.rem_subnet_id, &cnt, ORTE_UINT64);
    if (ORTE_SUCCESS != rc) {
        ORTE_ERROR_LOG(rc);
        return;
    }
    
    if (ENDPOINT_CONNECT_REQUEST != message_type) {
        opal_output(mca_btl_base_output, "unpacking %d of %d\n", cnt, ORTE_UINT32);
        rc = orte_dss.unpack(buffer, &lcl_qp, &cnt, ORTE_UINT32);
        if (ORTE_SUCCESS != rc) {
            ORTE_ERROR_LOG(rc);
            return;
        }
        opal_output(mca_btl_base_output, "unpacking %d of %d\n", cnt, ORTE_UINT16);
        rc = orte_dss.unpack(buffer, &lcl_lid, &cnt, ORTE_UINT16);
        if (ORTE_SUCCESS != rc) {
            ORTE_ERROR_LOG(rc);
            return;
        }
    }
    if (ENDPOINT_CONNECT_ACK != message_type) {
        int qp; 
        /* get ready for the data */
        rem_info.rem_qps = 
            (mca_btl_openib_rem_qp_info_t*) malloc(sizeof(mca_btl_openib_rem_qp_info_t) * 
                                                   mca_btl_openib_component.num_qps);
        
        /* unpack all the qp info */
        for (qp = 0; qp < mca_btl_openib_component.num_qps; ++qp) { 
            opal_output(mca_btl_base_output, "unpacking %d of %d\n", cnt, ORTE_UINT32);
            rc = orte_dss.unpack(buffer, &rem_info.rem_qps[qp].rem_qp_num, &cnt,
                                 ORTE_UINT32);
            if (ORTE_SUCCESS != rc) {
                ORTE_ERROR_LOG(rc);
                return;
            }
            opal_output(mca_btl_base_output, "unpacking %d of %d\n", cnt, ORTE_UINT32);
            rc = orte_dss.unpack(buffer, &rem_info.rem_qps[qp].rem_psn, &cnt,
                                 ORTE_UINT32);
            if (ORTE_SUCCESS != rc) {
                ORTE_ERROR_LOG(rc);
                return;
            }
        }
        
        opal_output(mca_btl_base_output, "unpacking %d of %d\n", cnt, ORTE_UINT16);
        rc = orte_dss.unpack(buffer, &rem_info.rem_lid, &cnt, ORTE_UINT16);
        if (ORTE_SUCCESS != rc) {
            ORTE_ERROR_LOG(rc);
            return;
        }
        opal_output(mca_btl_base_output, "unpacking %d of %d\n", cnt, ORTE_UINT32);
        rc = orte_dss.unpack(buffer, &rem_info.rem_mtu, &cnt, ORTE_UINT32);
        if (ORTE_SUCCESS != rc) {
            ORTE_ERROR_LOG(rc);
            return;
        }
        opal_output(mca_btl_base_output, "unpacking %d of %d\n", cnt, ORTE_UINT32);
        rc = orte_dss.unpack(buffer, &rem_info.rem_index, &cnt, ORTE_UINT32);
        if (ORTE_SUCCESS != rc) {
            ORTE_ERROR_LOG(rc);
            return;
        }
    }
    
    BTL_VERBOSE(("Received QP Info,  LID = %d, SUBNET = %016x\n",
                 rem_info.rem_lid, 
                 rem_info.rem_subnet_id));
    
    master = orte_ns.compare_fields(ORTE_NS_CMP_ALL, orte_process_info.my_name,
                                    process_name) > 0 ? true : false;
    
    for (ib_proc = (mca_btl_openib_proc_t*)
            opal_list_get_first(&mca_btl_openib_component.ib_procs);
        ib_proc != (mca_btl_openib_proc_t*)
            opal_list_get_end(&mca_btl_openib_component.ib_procs);
        ib_proc  = (mca_btl_openib_proc_t*)opal_list_get_next(ib_proc)) {
        bool found = false;
        
        if (orte_ns.compare_fields(ORTE_NS_CMP_ALL,
                                   &ib_proc->proc_guid, process_name) != ORTE_EQUAL) {
            continue;
        }
        
        if (ENDPOINT_CONNECT_REQUEST != message_type) {
            /* This is a reply message. Try to get the endpoint
               instance the reply belongs to */
            for (i = 0; i < ib_proc->proc_endpoint_count; i++) { 
                ib_endpoint = ib_proc->proc_endpoints[i];
                if (ib_endpoint->qps[0].lcl_qp != NULL &&
                    lcl_lid == ib_endpoint->endpoint_btl->lid &&
                    lcl_qp == ib_endpoint->qps[0].lcl_qp->qp_num &&
                    rem_info.rem_subnet_id == ib_endpoint->subnet_id) {
                    found = true;
                    break;
                }
            }
        } else {
            /* This is new connection request. If this is master try
               to find endpoint in a connecting state. If this is
               slave try to find  endpoint in closed state and
               initiate connection back */
            mca_btl_openib_endpoint_t *ib_endpoint_found = NULL;
            for (i = 0; i < ib_proc->proc_endpoint_count; i++) { 
                ib_endpoint = ib_proc->proc_endpoints[i];
                if (ib_endpoint->subnet_id != rem_info.rem_subnet_id ||
                   (ib_endpoint->endpoint_state != MCA_BTL_IB_CONNECTING
                    && ib_endpoint->endpoint_state != MCA_BTL_IB_CLOSED))
                    continue;
                found = true;
                ib_endpoint_found = ib_endpoint;
                if ((master &&
                     MCA_BTL_IB_CONNECTING == ib_endpoint->endpoint_state) ||
                    (!master &&
                     MCA_BTL_IB_CLOSED == ib_endpoint->endpoint_state))
                    break; /* Found one. No point to continue */
            }
            ib_endpoint = ib_endpoint_found;
            
            /* if this is slave and there is no endpoints in closed
               state then all connection are already in progress so
               just ignore this connection request */
            if (found && !master &&
                MCA_BTL_IB_CLOSED != ib_endpoint->endpoint_state) {
                return;
            }
        }
        
        if (!found) {
            BTL_ERROR(("can't find suitable endpoint for this peer\n")); 
            return; 
        }
        
        endpoint_state = ib_endpoint->endpoint_state;
        
        /* Update status */
        switch (endpoint_state) {
        case MCA_BTL_IB_CLOSED :
            /* We had this connection closed before.  The endpoint is
               trying to connect. Move the status of this connection
               to CONNECTING, and then reply with our QP
               information */
            if (master) {
                rc = reply_start_connect(ib_endpoint, &rem_info);
            } else {
                rc = oob_start_connect(ib_endpoint);
            }
            
            if (OMPI_SUCCESS != rc) {
                BTL_ERROR(("error in endpoint reply start connect"));
                break;
            }
            
            /* As long as we expect a message from the peer (in order
               to setup the connection) let the event engine pool the
               RML events. Note: we increment it once peer active
               connection. */
            opal_progress_event_users_increment();
            break;
             
        case MCA_BTL_IB_CONNECTING :
            set_remote_info(ib_endpoint, &rem_info);
            if (OMPI_SUCCESS != (rc = qp_connect_all(ib_endpoint))) {
                BTL_ERROR(("endpoint connect error: %d", rc)); 
                break;
            }
           
            if (master) {
                ib_endpoint->endpoint_state = MCA_BTL_IB_WAITING_ACK;

                /* Send him an ACK */
                send_connect_data(ib_endpoint, ENDPOINT_CONNECT_RESPONSE);
            } else {
                send_connect_data(ib_endpoint, ENDPOINT_CONNECT_ACK);
                /* Tell main BTL that we're done */
                mca_btl_openib_endpoint_connected(ib_endpoint);
             }
            break;
            
        case MCA_BTL_IB_WAITING_ACK:
            /* Tell main BTL that we're done */
            mca_btl_openib_endpoint_connected(ib_endpoint);
            break;
            
        case MCA_BTL_IB_CONNECT_ACK:
            send_connect_data(ib_endpoint, ENDPOINT_CONNECT_ACK);
            /* Tell main BTL that we're done */
            mca_btl_openib_endpoint_connected(ib_endpoint);
            break;
            
        case MCA_BTL_IB_CONNECTED:
            break;

        default :
            BTL_ERROR(("Invalid endpoint state %d", endpoint_state));
        }
        OPAL_THREAD_UNLOCK(&ib_endpoint->endpoint_lock);
        break;
    }
}