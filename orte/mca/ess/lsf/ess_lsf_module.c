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
 * Copyright (c) 2007      Cisco, Inc.  All rights reserved.
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 *
 */

#include "orte_config.h"
#include "orte/constants.h"

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif  /* HAVE_UNISTD_H */
#ifdef HAVE_STRING_H
#include <string.h>
#endif  /* HAVE_STRING_H */
#include <ctype.h>

#include <lsf/lsbatch.h>

#include "opal/util/argv.h"
#include "opal/util/opal_environ.h"

#include "orte/util/name_fns.h"
#include "orte/runtime/orte_globals.h"
#include "opal/mca/base/mca_base_param.h"
#include "orte/mca/errmgr/errmgr.h"
#include "orte/util/sys_info.h"

#include "orte/mca/ess/ess.h"
#include "orte/mca/ess/base/base.h"
#include "orte/mca/ess/lsf/ess_lsf.h"

static int lsf_set_name(void);

static int rte_init(char flags);
static int rte_finalize(void);

orte_ess_base_module_t orte_ess_lsf_module = {
    rte_init,
    rte_finalize,
    orte_ess_base_app_abort
};

static int rte_init(char flags)
{
    int ret;
    char *error = NULL;
    
    /* Start by getting a unique name */
    lsf_set_name();
    
    /* if I am a daemon, complete my setup using the
     * default procedure
     */
    if (orte_process_info.daemon) {
        if (ORTE_SUCCESS != (ret = orte_ess_base_orted_setup())) {
            ORTE_ERROR_LOG(ret);
            error = "orte_ess_base_orted_setup";
            goto error;
        }
    } else if (orte_process_info.tool) {
        /* otherwise, if I am a tool proc, use that procedure */
        if (ORTE_SUCCESS != (ret = orte_ess_base_tool_setup())) {
            ORTE_ERROR_LOG(ret);
            error = "orte_ess_base_tool_setup";
            goto error;
        }
    } else {
        /* otherwise, I must be an application process, so
         * use that default procedure
         */
        if (ORTE_SUCCESS != (ret = orte_ess_base_app_setup())) {
            ORTE_ERROR_LOG(ret);
            error = "orte_ess_base_app_setup";
            goto error;
        }
    }
    
    return ORTE_SUCCESS;
    
error:
    opal_show_help("help-orte-runtime.txt",
                   "orte_init:startup:internal-failure",
                   true, error, ORTE_ERROR_NAME(ret), ret);
    
    return ret;
}

static int rte_finalize(void)
{
    int ret;
    
    /* if I am a daemon, finalize using the default procedure */
    if (orte_process_info.daemon) {
        if (ORTE_SUCCESS != (ret = orte_ess_base_orted_finalize())) {
            ORTE_ERROR_LOG(ret);
        }
    } else if (orte_process_info.tool) {
        /* otherwise, if I am a tool proc, use that procedure */
        if (ORTE_SUCCESS != (ret = orte_ess_base_tool_finalize())) {
            ORTE_ERROR_LOG(ret);
        }
    } else {
        /* otherwise, I must be an application process, so
         * use that default procedure
         */
        if (ORTE_SUCCESS != (ret = orte_ess_base_app_finalize())) {
            ORTE_ERROR_LOG(ret);
        }
    }
    
    return ret;    
}

static int lsf_set_name(void)
{
    int rc;
    int id;
    char* name_string = NULL;
    int lsf_nodeid;

    /* start by getting our jobid, and vpid (which is the
       starting vpid for the list of daemons) */
    id = mca_base_param_register_string("orte", "ess", "name", NULL, NULL);
    mca_base_param_lookup_string(id, &name_string);

    if (name_string != NULL) {
        if (ORTE_SUCCESS != 
            (rc = orte_util_convert_string_to_process_name(&ORTE_PROC_MY_NAME, name_string))) {
            ORTE_ERROR_LOG(rc);
            free(name_string);
            return rc;
        }
        free(name_string);
    } else {
        orte_jobid_t jobid;
        orte_vpid_t vpid;
        char* jobid_string;
        char* vpid_string;
      
        id = mca_base_param_register_string("orte", "ess", "jobid", NULL, NULL);
        mca_base_param_lookup_string(id, &jobid_string);
        if (NULL == jobid_string) {
            ORTE_ERROR_LOG(ORTE_ERR_NOT_FOUND);
            return ORTE_ERR_NOT_FOUND;
        }
        if (ORTE_SUCCESS != 
            (rc = orte_util_convert_string_to_jobid(&jobid, jobid_string))) {
            ORTE_ERROR_LOG(rc);
            return(rc);
        }
        
        id = mca_base_param_register_string("orte", "ess", "vpid", NULL, NULL);
        mca_base_param_lookup_string(id, &vpid_string);
        if (NULL == vpid_string) {
            ORTE_ERROR_LOG(ORTE_ERR_NOT_FOUND);
            return ORTE_ERR_NOT_FOUND;
        }
        if (ORTE_SUCCESS !=
            (rc = orte_util_convert_string_to_vpid(&vpid, vpid_string))) {
            ORTE_ERROR_LOG(rc);
            return(rc);
        }

        ORTE_PROC_MY_NAME->jobid;
        ORTE_PROC_MY_NAME->vpid = vpid;
    }
    
    /* fix up the base name and make it the "real" name */
    lsf_nodeid = atoi(getenv("LSF_PM_TASKID"));
    ORTE_PROC_MY_NAME->vpid = lsf_nodeid;

    /* get the non-name common environmental variables */
    if (ORTE_SUCCESS != (rc = orte_ess_env_get())) {
        ORTE_ERROR_LOG(rc);
        return rc;
    }
    
    return ORTE_SUCCESS;
}