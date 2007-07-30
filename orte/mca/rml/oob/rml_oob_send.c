/*
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

#include "orte_config.h"

#include "rml_oob.h"

#include "orte/mca/routed/routed.h"
#include "orte/mca/oob/oob.h"
#include "orte/mca/oob/base/base.h"
#include "orte/dss/dss.h"
#include "orte/mca/ns/ns.h"
#include "orte/mca/errmgr/errmgr.h"

static void
orte_rml_send_msg_callback(int status,
                           struct orte_process_name_t* peer,
                           struct iovec* iov,
                           int count,
                           orte_rml_tag_t tag,
                           void* cbdata)
{
    orte_rml_oob_msg_t *msg = (orte_rml_oob_msg_t*) cbdata;
        orte_rml_oob_msg_header_t *hdr =
            (orte_rml_oob_msg_header_t*) iov[0].iov_base;

    if (msg->msg_type == ORTE_RML_BLOCKING_SEND) {
        /* blocking send */
        if (status > 0) {
            msg->msg_status = status - sizeof(orte_rml_oob_msg_header_t);
        } else {
            msg->msg_status = status;
        }
        msg->msg_complete = true;
        opal_condition_broadcast(&msg->msg_cond);
    } else if (msg->msg_type == ORTE_RML_NONBLOCKING_IOV_SEND) {
        /* non-blocking iovec send */
        if (status > 0) {
            status -= sizeof(orte_rml_oob_msg_header_t);
        }
        msg->msg_cbfunc.iov(status, peer, iov + 1, count - 1, 
                            hdr->tag, msg->msg_cbdata);
        OBJ_RELEASE(msg);
    } else if (msg->msg_type == ORTE_RML_NONBLOCKING_BUFFER_SEND) {
        /* non-blocking buffer send */
        if (status > 0) {
            status -= sizeof(orte_rml_oob_msg_header_t);
        }
        msg->msg_cbfunc.buffer(status, peer, msg->user_buffer, 
                               hdr->tag, msg->msg_cbdata);
        OBJ_RELEASE(msg->user_buffer);
        OBJ_RELEASE(msg);
    } else {
        abort();
    }
}
                           

int
orte_rml_oob_send(orte_process_name_t* peer,
                  struct iovec *iov,
                  int count,
                  int tag,
                  int flags)
{
    orte_rml_oob_msg_t *msg = OBJ_NEW(orte_rml_oob_msg_t);
    int ret;
    orte_process_name_t next;
    int real_tag;
    int i;
    int bytes = 0;

    msg->msg_type = ORTE_RML_BLOCKING_SEND;
    flags |= ORTE_RML_FLAG_RECURSIVE_CALLBACK;

    next = orte_routed.get_route(peer);
    if (next.vpid == ORTE_VPID_INVALID) {
        ORTE_ERROR_LOG(ORTE_ERR_ADDRESSEE_UNKNOWN);
        return ORTE_ERR_ADDRESSEE_UNKNOWN;
    }
    msg->msg_data = malloc(sizeof(struct iovec) * (count + 1));
    msg->msg_data[0].iov_base = (ompi_iov_base_ptr_t)&msg->msg_header;
    msg->msg_data[0].iov_len = sizeof(orte_rml_oob_msg_header_t);
    bytes += msg->msg_data[0].iov_len;
    for (i = 0 ; i < count ; ++i) {
        msg->msg_data[i + 1].iov_base = iov[i].iov_base;
        msg->msg_data[i + 1].iov_len = iov[i].iov_len;
        bytes += msg->msg_data[i + 1].iov_len;
    }

    msg->msg_header.origin = *ORTE_PROC_MY_NAME;
    msg->msg_header.destination = *peer;
    msg->msg_header.tag = tag;

    if (0 == orte_ns.compare_fields(ORTE_NS_CMP_ALL, &next, peer)) {
        real_tag = tag;
    } else {
        real_tag = ORTE_RML_TAG_RML_ROUTE;
    }

    ret = orte_rml_oob_module.active_oob->oob_send_nb(&next, 
                                                      msg->msg_data,
                                                      count + 1,
                                                      real_tag, 
                                                      flags,
                                                      orte_rml_send_msg_callback,
                                                      msg);
    if (0 < ret) goto cleanup;

    OPAL_THREAD_LOCK(&msg->msg_lock);
    while (!msg->msg_complete) {
        opal_condition_wait(&msg->msg_cond, &msg->msg_lock);
    }
    ret = msg->msg_status;
    OPAL_THREAD_UNLOCK(&msg->msg_lock);

 cleanup:
    OBJ_RELEASE(msg);

    return ret;
}


int
orte_rml_oob_send_nb(orte_process_name_t* peer,
                     struct iovec* iov,
                     int count,
                     orte_rml_tag_t tag,
                     int flags,
                     orte_rml_callback_fn_t cbfunc,
                     void* cbdata)
{
    orte_rml_oob_msg_t *msg = OBJ_NEW(orte_rml_oob_msg_t);
    int ret;
    int real_tag;
    orte_process_name_t next;
    int i;
    int bytes = 0;

    msg->msg_type = ORTE_RML_NONBLOCKING_IOV_SEND;
    msg->msg_cbfunc.iov = cbfunc;
    msg->msg_cbdata = cbdata;

    next = orte_routed.get_route(peer);
    if (next.vpid == ORTE_VPID_INVALID) {
        ORTE_ERROR_LOG(ORTE_ERR_ADDRESSEE_UNKNOWN);
        return ORTE_ERR_ADDRESSEE_UNKNOWN;
    }

    msg->msg_data = malloc(sizeof(struct iovec) * (count + 1));

    msg->msg_data[0].iov_base = (ompi_iov_base_ptr_t)&msg->msg_header;
    msg->msg_data[0].iov_len = sizeof(orte_rml_oob_msg_header_t);
    bytes += msg->msg_data[0].iov_len;
    for (i = 0 ; i < count ; ++i) {
        msg->msg_data[i + 1].iov_base = iov[i].iov_base;
        msg->msg_data[i + 1].iov_len = iov[i].iov_len;
        bytes += msg->msg_data[i + 1].iov_len;
    }

    msg->msg_header.origin = *ORTE_PROC_MY_NAME;
    msg->msg_header.destination = *peer;
    msg->msg_header.tag = tag;

    if (0 == orte_ns.compare_fields(ORTE_NS_CMP_ALL, &next, peer)) {
        real_tag = tag;
    } else {
        real_tag = ORTE_RML_TAG_RML_ROUTE;
    }

    ret = orte_rml_oob_module.active_oob->oob_send_nb(&next, 
                                                      msg->msg_data,
                                                      count + 1,
                                                      real_tag,
                                                      flags,
                                                      orte_rml_send_msg_callback,
                                                      msg);
    if (ret < 0) OBJ_RELEASE(msg);

    return ret;
}


int
orte_rml_oob_send_buffer(orte_process_name_t* peer,
                         orte_buffer_t* buffer,
                         orte_rml_tag_t tag,
                         int flags)
{
    int ret;
    void *dataptr;
    orte_std_cntr_t datalen;
    struct iovec iov[1];

    /* first build iovec from buffer information */
    ret = orte_dss.unload(buffer, &dataptr, &datalen);
    if (ret != ORTE_SUCCESS) return ret;
    orte_dss.load(buffer, dataptr, datalen);

    iov[0].iov_base = (IOVBASE_TYPE*)dataptr;
    iov[0].iov_len  = datalen;

    return orte_rml_oob_send(peer, iov, 1, tag, flags);
}


int
orte_rml_oob_send_buffer_nb(orte_process_name_t* peer,
                            orte_buffer_t* buffer,
                            orte_rml_tag_t tag,
                            int flags,
                            orte_rml_buffer_callback_fn_t cbfunc,
                            void* cbdata)
{
    orte_rml_oob_msg_t *msg = OBJ_NEW(orte_rml_oob_msg_t);
    void *dataptr;
    orte_std_cntr_t datalen;
    int ret;
    int real_tag;
    orte_process_name_t next;
    int bytes = 0;

    /* first build iovec from buffer information */
    ret = orte_dss.unload(buffer, &dataptr, &datalen);
    if (ORTE_SUCCESS != ret) {
        OBJ_RELEASE(msg);
        return ret;
    }
    orte_dss.load(buffer, dataptr, datalen);

    msg->msg_type = ORTE_RML_NONBLOCKING_BUFFER_SEND;
    msg->msg_cbfunc.buffer = cbfunc;
    msg->msg_cbdata = cbdata;
    msg->user_buffer = buffer;

    msg->msg_data = malloc(sizeof(struct iovec) * 2);

    next = orte_routed.get_route(peer);
    if (next.vpid == ORTE_VPID_INVALID) {
        ORTE_ERROR_LOG(ORTE_ERR_ADDRESSEE_UNKNOWN);
        return ORTE_ERR_ADDRESSEE_UNKNOWN;
    }

    msg->msg_data[0].iov_base = (ompi_iov_base_ptr_t)&msg->msg_header;
    msg->msg_data[0].iov_len = sizeof(orte_rml_oob_msg_header_t);
    bytes += msg->msg_data[0].iov_len;

    msg->msg_data[1].iov_base = (IOVBASE_TYPE*)dataptr;
    msg->msg_data[1].iov_len  = datalen;
    
    bytes += msg->msg_data[1].iov_len;

    msg->msg_header.origin = *ORTE_PROC_MY_NAME;
    msg->msg_header.destination = *peer;
    msg->msg_header.tag = tag;

    if (0 == orte_ns.compare_fields(ORTE_NS_CMP_ALL, &next, peer)) {
        real_tag = tag;
    } else {
        real_tag = ORTE_RML_TAG_RML_ROUTE;
    }

    OBJ_RETAIN(buffer);

    ret = orte_rml_oob_module.active_oob->oob_send_nb(&next,
                                                      msg->msg_data,
                                                      2,
                                                      real_tag,
                                                      flags,
                                                      orte_rml_send_msg_callback,
                                                      msg);

    if (ret < 0) {
        OBJ_RELEASE(msg);
        OBJ_RELEASE(buffer);
    }

    return ret;
}
