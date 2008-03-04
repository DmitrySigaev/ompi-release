/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2006 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2007      Cisco, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "ompi_config.h"
#include "ompi/constants.h"

#include <string.h>
#include <stdio.h>
#include <ctype.h>

#include "opal/util/show_help.h"
#include "opal/util/argv.h"
#include "opal/util/opal_getcwd.h"

#include "opal/dss/dss.h"
#include "orte/mca/errmgr/errmgr.h"
#include "orte/mca/grpcomm/grpcomm.h"
#include "orte/mca/plm/plm.h"
#include "orte/mca/rml/rml.h"
#include "orte/mca/rml/base/rml_contact.h"
#include "orte/mca/routed/routed.h"
#include "orte/util/name_fns.h"
#include "orte/runtime/orte_globals.h"
#include "orte/runtime/orte_data_server.h"

#include "ompi/communicator/communicator.h"
#include "ompi/proc/proc.h"
#include "ompi/mca/pml/pml.h"
#include "ompi/info/info.h"
#include "ompi/runtime/ompi_module_exchange.h"

#include "ompi/mca/pubsub/base/base.h"
#include "pubsub_orte.h"

/* Establish contact with the server
 *
 * NOTE: we do not do this automatically during init to avoid
 * forcing every process to pay the time penalty during MPI_Init
 * when only a few, if any, will ever call pub/lookup/unpub. In
 * addition, those that -do- call these functions may well only
 * use local (as opposed to global) storage, and hence will have
 * no need to talk to the server, even though a sys admin may
 * have set one up. So we do a lazy setup of the server contact
 * info - it only gets setup the first time we call a function
 * that wants to talk to the global server
 */
static bool server_setup=false;

static void setup_server(void)
{
    opal_buffer_t buf;
    orte_rml_cmd_flag_t command=ORTE_RML_UPDATE_CMD;
    int rc;
    
    if (NULL == mca_pubsub_orte_component.server_uri) {
        /* if the contact info for the server is NULL, then there
         * is nothing to do
         */
        server_setup = true;
        return;
    }
    
    /* setup the route to the server using the
     * selected routed component. This allows us
     * to tell the local daemon how to reach the
     * server, so we can still only have one connection
     * open! To do this, we need to insert the server's
     * uri into a buffer
     */
    OBJ_CONSTRUCT(&buf, opal_buffer_t);
    opal_dss.pack(&buf, &command, 1, ORTE_RML_CMD);
    opal_dss.pack(&buf, &mca_pubsub_orte_component.server_uri, 1, OPAL_STRING);
    if (ORTE_SUCCESS != (rc = orte_routed.init_routes(ORTE_PROC_MY_NAME->jobid, &buf))) {
        ORTE_ERROR_LOG(rc);
        server_setup = true;
        return;
    }
    OBJ_DESTRUCT(&buf);
    
    /* extract the server's name */
    orte_rml_base_parse_uris(mca_pubsub_orte_component.server_uri, &mca_pubsub_orte_component.server, NULL);

    /* flag the server as found */
    mca_pubsub_orte_component.server_found = true;

    /* flag setup as completed */
    server_setup = true;
}

/*
 * Init the module
 */
static int init(void)
{    
    return OMPI_SUCCESS;
}

/*
 * publish the port_name for the specified service_name. This will
 * be published under our process name, so only we will be allowed
 * to remove it later.
 */
static int publish ( char *service_name, ompi_info_t *info, char *port_name )
{
    int rc, ret, flag;
    bool global_scope = false;
    orte_process_name_t *info_host;
    opal_buffer_t buf;
    orte_data_server_cmd_t cmd=ORTE_DATA_SERVER_PUBLISH;
    orte_std_cntr_t cnt;

    ompi_info_get_bool(info, "ompi_global_scope", &global_scope, &flag);

    if (!global_scope) {
        /* if the scope is not global, then store the value on the HNP */
        info_host = ORTE_PROC_MY_HNP;
    } else {
        /* has the server been setup yet? */
        if (!server_setup) {
            setup_server();
        }
        /* store the value on the global ompi_server, but error
         * if that server wasn't contacted
         */
        if (!mca_pubsub_orte_component.server_found) {
            opal_show_help("help-ompi-pubsub-orte.txt", "pubsub-orte:no-server",
                           true, (long)ORTE_PROC_MY_NAME->vpid, "publish to");
            return OMPI_ERR_NOT_FOUND;
        }
        info_host = &mca_pubsub_orte_component.server;
    }
    
    /* construct the buffer */
    OBJ_CONSTRUCT(&buf, opal_buffer_t);
    
    /* pack the publish command */
    if (ORTE_SUCCESS != (rc = opal_dss.pack(&buf, &cmd, 1, ORTE_DATA_SERVER_CMD))) {
        ORTE_ERROR_LOG(rc);
        goto CLEANUP;
    }
    
    /* pack the service name */
    if (ORTE_SUCCESS != (rc = opal_dss.pack(&buf, &service_name, 1, OPAL_STRING))) {
        ORTE_ERROR_LOG(rc);
        goto CLEANUP;
    }
    
    /* pack the port name */
    if (ORTE_SUCCESS != (rc = opal_dss.pack(&buf, &port_name, 1, OPAL_STRING))) {
        ORTE_ERROR_LOG(rc);
        goto CLEANUP;
    }
    
    /* send the data */
    if (0 > (rc = orte_rml.send_buffer(info_host, &buf, ORTE_RML_TAG_DATA_SERVER, 0))) {
        ORTE_ERROR_LOG(rc);
        goto CLEANUP;
    }
    OBJ_DESTRUCT(&buf);

    /* get the answer */
    OBJ_CONSTRUCT(&buf, opal_buffer_t);
    if (0 > (rc = orte_rml.recv_buffer(ORTE_NAME_WILDCARD, &buf, ORTE_RML_TAG_DATA_CLIENT, 0))) {
        ORTE_ERROR_LOG(rc);
        goto CLEANUP;
    }
    
    /* unpack the result */
    cnt = 1;
    if (ORTE_SUCCESS != (rc = opal_dss.unpack(&buf, &ret, &cnt, OPAL_INT))) {
        ORTE_ERROR_LOG(rc);
        goto CLEANUP;
    }
    rc = ret;
    
    
CLEANUP:
    OBJ_DESTRUCT(&buf);
    
    return rc;
}

enum { NONE, LOCAL, GLOBAL };

static char* lookup ( char *service_name, ompi_info_t *info )
{
    orte_process_name_t *info_host;
    opal_buffer_t buf;
    orte_data_server_cmd_t cmd=ORTE_DATA_SERVER_LOOKUP;
    orte_std_cntr_t cnt=0;
    char *port_name=NULL;
    int ret, rc, flag, i;
    char value[256], **tokens, *ptr;
    int lookup[2] = { LOCAL, GLOBAL };
    size_t num_tokens;

    /* Look in the MPI_Info (ompi_info_t*) for the key
     * "ompi_lookup_order".  Acceptable values are:
     *
     * - "local" -- only check the local scope
     * - "global" -- only check the global scope
     * - "local,global" -- check the local scope first, then check the
     *   global scope
     * - "global,local" -- check the global scope first, then check the
     *   local scope
     *
     * Give a little leeway in terms of whitespace in the value.
     *
     * The lookup[2] array will contain the results: lookup[0] is the
     * first scope to check, lookup[1] is the 2nd.  Either value may
     * be NONE, LOCAL, or GLOBAL.  If both are NONE, clearly that's an
     * error.  :-)
     */
    ompi_info_get(info, "ompi_lookup_order", sizeof(value) - 1, value, &flag);
    if (flag) {
        ptr = &value[0];
        while (isspace(*ptr) && (ptr - value) < (int)sizeof(value)) {
            ++ptr;
        }
        if (ptr - value < (int)sizeof(value)) {
            tokens = opal_argv_split(ptr, ',');
            if (NULL != tokens) {
                if ((num_tokens = opal_argv_count(tokens)) > 2) {
                    /* too many values in the comma-delimited list */
                    opal_show_help("help-ompi-pubsub-orte.txt",
                                   "pubsub-orte:too-many-orders",
                                   true, (long)ORTE_PROC_MY_NAME->vpid,
                                   (long)num_tokens);
                    return NULL;
                }
                for (i = 0; i < 2; ++i) {
                    if (NULL != tokens[i]) {
                        if (0 == strcasecmp(tokens[i], "local")) {
                            lookup[i] = LOCAL;
                        } else if (0 == strcasecmp(tokens[i], "global")) {
                            lookup[i] = GLOBAL;
                        } else {
                            /* unrecognized value -- that's an error */
                            opal_show_help("help-ompi-pubsub-orte.txt",
                                           "pubsub-orte:unknown-order",
                                           true, (long)ORTE_PROC_MY_NAME->vpid);
                            return NULL;
                        }
                    } else {
                        lookup[i] = NONE;
                    }
                }
                opal_argv_free(tokens);
            }
        }
    }
    
    /* check for error situations */
    
    if (NONE == lookup[0]) {
        /* if the user provided an info key, then we at least must
         * be given one place to look
         */
        opal_show_help("help-ompi-pubsub-orte.txt",
                       "pubsub-orte:unknown-order",
                       true, (long)ORTE_PROC_MY_NAME->vpid);
        return NULL;
    }
    
    if (GLOBAL == lookup[0]) {
        /* has the server been setup yet? */
        if (!server_setup) {
            setup_server();
        }
        
        if (!mca_pubsub_orte_component.server_found) {
            /* if we were told to look global first and no server is
             * present, then that is an error
             */
            opal_show_help("help-ompi-pubsub-orte.txt", "pubsub-orte:no-server",
                           true, (long)ORTE_PROC_MY_NAME->vpid, "lookup from");
            return NULL;
        }
    }

    /* go find the value */
    for (i=0; i < 2; i++) {
        if (LOCAL == lookup[i]) {
            /* if the scope is local, then lookup the value on the HNP */
            info_host = ORTE_PROC_MY_HNP;
        } else if (GLOBAL == lookup[i]) {
            /* has the server been setup yet? */
            if (!server_setup) {
                setup_server();
            }
            /* lookup the value on the global ompi_server, but error
             * if that server wasn't contacted
             */
            if (!mca_pubsub_orte_component.server_found) {
                opal_show_help("help-ompi-pubsub-orte.txt",
                               "pubsub-orte:no-server",
                               true, (long)ORTE_PROC_MY_NAME->vpid,
                               "lookup from");
                return NULL;
            }
            info_host = &mca_pubsub_orte_component.server;
        } else {
            /* unknown host! */
            opal_show_help("help-ompi-pubsub-orte.txt",
                           "pubsub-orte:unknown-order",
                           true, (long)ORTE_PROC_MY_NAME->vpid);
            return NULL;
        }
        
        /* go look it up */
        /* construct the buffer */
        OBJ_CONSTRUCT(&buf, opal_buffer_t);
        
        /* pack the lookup command */
        if (ORTE_SUCCESS != (ret = opal_dss.pack(&buf, &cmd, 1, ORTE_DATA_SERVER_CMD))) {
            ORTE_ERROR_LOG(ret);
            goto CLEANUP;
        }
        
        /* pack the service name */
        if (ORTE_SUCCESS != (ret = opal_dss.pack(&buf, &service_name, 1, OPAL_STRING))) {
            ORTE_ERROR_LOG(ret);
            goto CLEANUP;
        }
        
        /* send the cmd */
        if (0 > (ret = orte_rml.send_buffer(info_host, &buf, ORTE_RML_TAG_DATA_SERVER, 0))) {
            ORTE_ERROR_LOG(ret);
            goto CLEANUP;
        }
        OBJ_DESTRUCT(&buf);
        
        /* get the answer */
        OBJ_CONSTRUCT(&buf, opal_buffer_t);
        if (0 > (ret = orte_rml.recv_buffer(ORTE_NAME_WILDCARD, &buf, ORTE_RML_TAG_DATA_CLIENT, 0))) {
            ORTE_ERROR_LOG(ret);
            goto CLEANUP;
        }
        
        /* unpack the return code */
        cnt = 1;
        if (ORTE_SUCCESS != (ret = opal_dss.unpack(&buf, &rc, &cnt, OPAL_INT))) {
            ORTE_ERROR_LOG(ret);
            goto CLEANUP;
        }

        if (ORTE_SUCCESS == rc) {
            /* the server was able to lookup the port - unpack the port name */
            cnt=1;
            if (ORTE_SUCCESS != (ret = opal_dss.unpack(&buf, &port_name, &cnt, OPAL_STRING))) {
                ORTE_ERROR_LOG(ret);
                goto CLEANUP;
            }
            
            if (NULL != port_name) {
                /* got an answer - return it */
                OBJ_DESTRUCT(&buf);
                return port_name;
            }
        }
        
        /* if we didn't get a port_name, then continue */
        OBJ_DESTRUCT(&buf);
    }
    
    /* only get here if we tried both options and failed - since the
     * buffer will already have been cleaned up, just return
     */
    return NULL;

CLEANUP:
    OBJ_DESTRUCT(&buf);
    
    return NULL;
    
}

/*
 * delete the entry. Only the process who has published
 * the service_name has the right to remove this
 * service - the server will verify and report the result
 */
static int unpublish ( char *service_name, ompi_info_t *info )
{
    int rc, ret, flag;
    bool global_scope;
    orte_process_name_t *info_host;
    opal_buffer_t buf;
    orte_data_server_cmd_t cmd=ORTE_DATA_SERVER_UNPUBLISH;
    orte_std_cntr_t cnt;
    
    ompi_info_get_bool(info, "ompi_global_scope", &global_scope, &flag);

    if (!global_scope) {
        /* if the scope is not global, then unpublish the value from the HNP */
        info_host = ORTE_PROC_MY_HNP;
    } else {
        /* has the server been setup yet? */
        if (!server_setup) {
            setup_server();
        }
        /* unpublish the value from the global ompi_server, but error
        * if that server wasn't contacted
        */
        if (!mca_pubsub_orte_component.server_found) {
            opal_show_help("help-ompi-pubsub-orte.txt", "pubsub-orte:no-server",
                           true);
            return OMPI_ERR_NOT_FOUND;
        }
        info_host = &mca_pubsub_orte_component.server;
    }
    
    /* construct the buffer */
    OBJ_CONSTRUCT(&buf, opal_buffer_t);
    
    /* pack the unpublish command */
    if (ORTE_SUCCESS != (rc = opal_dss.pack(&buf, &cmd, 1, ORTE_DATA_SERVER_CMD))) {
        ORTE_ERROR_LOG(rc);
        goto CLEANUP;
    }
    
    /* pack the service name */
    if (ORTE_SUCCESS != (rc = opal_dss.pack(&buf, &service_name, 1, OPAL_STRING))) {
        ORTE_ERROR_LOG(rc);
        goto CLEANUP;
    }
    
    /* send the command */
    if (0 > (rc = orte_rml.send_buffer(info_host, &buf, ORTE_RML_TAG_DATA_SERVER, 0))) {
        ORTE_ERROR_LOG(rc);
        goto CLEANUP;
    }
    OBJ_DESTRUCT(&buf);
    
    /* get the answer */
    OBJ_CONSTRUCT(&buf, opal_buffer_t);
    if (0 > (rc = orte_rml.recv_buffer(ORTE_NAME_WILDCARD, &buf, ORTE_RML_TAG_DATA_CLIENT, 0))) {
        ORTE_ERROR_LOG(rc);
        goto CLEANUP;
    }
    
    /* unpack the result */
    cnt = 1;
    if (ORTE_SUCCESS != (rc = opal_dss.unpack(&buf, &ret, &cnt, OPAL_INT))) {
        ORTE_ERROR_LOG(rc);
        goto CLEANUP;
    }
    rc = ret;
    
CLEANUP:
    OBJ_DESTRUCT(&buf);
    
    return rc;
}


/*
 * finalize the module
 */
static int finalize(void)
{
    return OMPI_SUCCESS;
}

/*
 * instantiate the module
 */
ompi_pubsub_base_module_t ompi_pubsub_orte_module = {
    init,
    publish,
    unpublish,
    lookup,
    finalize
};

