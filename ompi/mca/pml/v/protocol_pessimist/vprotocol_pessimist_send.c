/*
 * Copyright (c) 2004-2007 The Trustees of the University of Tennessee.
 *                         All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "ompi_config.h"
#include "../pml_v.h"
#include "vprotocol_pessimist.h"
#include "vprotocol_pessimist_sender_based.h"

int mca_vprotocol_pessimist_isend(void *buf,
                       size_t count,
                       ompi_datatype_t* datatype,
                       int dst,
                       int tag,
                       mca_pml_base_send_mode_t sendmode,
                       ompi_communicator_t* comm,
                       ompi_request_t** request )
{
    int ret;
  
    V_OUTPUT_VERBOSE(50, "pessimist:\tisend\tposted\t%llx\tto %d\ttag %d\tsize %ld",
                     (long long) mca_vprotocol_pessimist.clock, dst, tag, (long) count);

    VPROTOCOL_PESSIMIST_EVENT_FLUSH();
    ret = mca_pml_v.host_pml.pml_isend(buf, count, datatype, dst, tag, sendmode, 
                                       comm, request);
    VPESSIMIST_REQ_INIT(*request);
    VPROTOCOL_PESSIMIST_SENDER_BASED_COPY(*request);
    return ret;
}

int mca_vprotocol_pessimist_send(void *buf,
                      size_t count,
                      ompi_datatype_t* datatype,
                      int dst,
                      int tag,
                      mca_pml_base_send_mode_t sendmode,
                      ompi_communicator_t* comm )
{
    ompi_request_t *request = MPI_REQUEST_NULL;
    int rc;

    V_OUTPUT_VERBOSE(110, "pessimist:\tsend\tposted\t%"PRIx64"\tto %d\ttag %d\tsize %z", 
                     mca_vprotocol_pessimist.clock, dst, tag, count);

    VPROTOCOL_PESSIMIST_EVENT_FLUSH();
    mca_pml_v.host_pml.pml_isend(buf, count, datatype, dst, tag, sendmode, 
                                 comm, &request);
    VPESSIMIST_REQ_INIT(request);
    VPROTOCOL_PESSIMIST_SENDER_BASED_COPY(request);
    VPROTOCOL_PESSIMIST_WAIT(&request, MPI_STATUS_IGNORE, rc);
    return rc;
}