/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2008 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart, 
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 *
 */

#include "orte_config.h"
#include "orte/constants.h"

#include <sys/types.h>
#include <stdio.h>
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include "opal/event/event.h"
#include "opal/util/output.h"
#include "opal/util/show_help.h"
#include "opal/runtime/opal.h"
#include "opal/runtime/opal_cr.h"

#include "opal/util/os_path.h"
#include "opal/util/malloc.h"

#include "orte/mca/rml/base/base.h"
#include "orte/mca/rml/base/rml_contact.h"
#include "orte/mca/routed/base/base.h"
#include "orte/mca/routed/routed.h"
#include "orte/mca/errmgr/base/base.h"
#include "orte/mca/grpcomm/base/base.h"
#include "orte/mca/iof/base/base.h"
#include "orte/mca/ras/base/base.h"
#include "orte/mca/plm/base/base.h"
#include "orte/mca/odls/base/base.h"

#include "orte/mca/rmaps/base/base.h"
#if OPAL_ENABLE_FT == 1
#include "orte/mca/snapc/base/base.h"
#endif
#include "orte/mca/filem/base/base.h"
#include "orte/util/proc_info.h"
#include "orte/util/session_dir.h"
#include "orte/util/sys_info.h"
#include "orte/util/hnp_contact.h"
#include "orte/util/name_fns.h"

#include "orte/runtime/runtime.h"
#include "orte/runtime/orte_wait.h"
#include "orte/runtime/orte_globals.h"

#include "orte/runtime/orte_cr.h"
#include "orte/mca/ess/ess.h"
#include "orte/mca/ess/base/base.h"
#include "orte/mca/ess/hnp/ess_hnp.h"

static int rte_init(char flags);
static int rte_finalize(void);
static void rte_abort(int status, bool report) __opal_attribute_noreturn__;


orte_ess_base_module_t orte_ess_hnp_module = {
    rte_init,
    rte_finalize,
    rte_abort
};


static int rte_init(char flags)
{
    int ret;
    char *error = NULL;
    char *jobid_str, *procid_str, *contact_path;
    orte_job_t *jdata;
    orte_node_t *node;
    orte_proc_t *proc;
    
    /* Since we are the HNP, then responsibility for
     * defining the name falls to the PLM component for our
     * respective environment - hence, we have to open the PLM
     * first and select that component. Note that ONLY the
     * HNP ever uses a PLM component anyway
     */
    if (ORTE_SUCCESS != (ret = orte_plm_base_open())) {
        ORTE_ERROR_LOG(ret);
        error = "orte_plm_base_open";
        goto error;
    }
    
    if (ORTE_SUCCESS != (ret = orte_plm_base_select())) {
        ORTE_ERROR_LOG(ret);
        error = "orte_plm_base_select";
        goto error;
    }
    if (ORTE_SUCCESS != (ret = orte_plm.set_hnp_name())) {
        ORTE_ERROR_LOG(ret);
        error = "orte_plm_set_hnp_name";
        goto error;
    }
    
    /* Setup the communication infrastructure */
    /*
     * Runtime Messaging Layer
     */
    if (ORTE_SUCCESS != (ret = orte_rml_base_open())) {
        ORTE_ERROR_LOG(ret);
        error = "orte_rml_base_open";
        goto error;
    }
    if (ORTE_SUCCESS != (ret = orte_rml_base_select())) {
        ORTE_ERROR_LOG(ret);
        error = "orte_rml_base_select";
        goto error;
    }
    /*
     * Routed system
     */
    if (ORTE_SUCCESS != (ret = orte_routed_base_open())) {
        ORTE_ERROR_LOG(ret);
        error = "orte_routed_base_open";
        goto error;
    }
    if (ORTE_SUCCESS != (ret = orte_routed_base_select())) {
        ORTE_ERROR_LOG(ret);
        error = "orte_routed_base_select";
        goto error;
    }
    /*
     * Group communications
     */
    if (ORTE_SUCCESS != (ret = orte_grpcomm_base_open())) {
        ORTE_ERROR_LOG(ret);
        error = "orte_grpcomm_base_open";
        goto error;
    }
    if (ORTE_SUCCESS != (ret = orte_grpcomm_base_select())) {
        ORTE_ERROR_LOG(ret);
        error = "orte_grpcomm_base_select";
        goto error;
    }
    
    /* Now provide a chance for the PLM
     * to perform any module-specific init functions. This
     * needs to occur AFTER the communications are setup
     * as it may involve starting a non-blocking recv
     */
    if (ORTE_SUCCESS != (ret = orte_plm.init())) {
        ORTE_ERROR_LOG(ret);
        error = "orte_plm_init";
        goto error;
    }

    /*
     * Setup the remaining resource
     * management and errmgr frameworks - application procs
     * and daemons do not open these frameworks as they only use
     * the hnp proxy support in the PLM framework.
     */
    if (ORTE_SUCCESS != (ret = orte_ras_base_open())) {
        ORTE_ERROR_LOG(ret);
        error = "orte_ras_base_open";
        goto error;
    }
    
    if (ORTE_SUCCESS != (ret = orte_ras_base_select())) {
        ORTE_ERROR_LOG(ret);
        error = "orte_ras_base_find_available";
        goto error;
    }
    
    if (ORTE_SUCCESS != (ret = orte_rmaps_base_open())) {
        ORTE_ERROR_LOG(ret);
        error = "orte_rmaps_base_open";
        goto error;
    }
    
    if (ORTE_SUCCESS != (ret = orte_rmaps_base_select())) {
        ORTE_ERROR_LOG(ret);
        error = "orte_rmaps_base_find_available";
        goto error;
    }
    
    if (ORTE_SUCCESS != (ret = orte_errmgr_base_open())) {
        error = "orte_errmgr_base_open";
        goto error;
    }
    if (ORTE_SUCCESS != (ret = orte_errmgr_base_select())) {
        ORTE_ERROR_LOG(ret);
        error = "orte_errmgr_base_select";
        goto error;
    }
    
    /* Open/select the odls */
    if (ORTE_SUCCESS != (ret = orte_odls_base_open())) {
        ORTE_ERROR_LOG(ret);
        error = "orte_odls_base_open";
        goto error;
    }
    if (ORTE_SUCCESS != (ret = orte_odls_base_select())) {
        ORTE_ERROR_LOG(ret);
        error = "orte_odls_base_select";
        goto error;
    }
    
    /* enable communication with the rml */
    if (ORTE_SUCCESS != (ret = orte_rml.enable_comm())) {
        ORTE_ERROR_LOG(ret);
        error = "orte_rml.enable_comm";
        goto error;
    }

    /* setup my session directory */
    if (ORTE_SUCCESS != (ret = orte_util_convert_jobid_to_string(&jobid_str, ORTE_PROC_MY_NAME->jobid))) {
        ORTE_ERROR_LOG(ret);
        error = "orte_convert_jobid_to_string";
        goto error;
    }
    if (ORTE_SUCCESS != (ret = orte_util_convert_vpid_to_string(&procid_str, ORTE_PROC_MY_NAME->vpid))) {
        ORTE_ERROR_LOG(ret);
        error = "orte_convert_vpid_to_string";
        goto error;
    }

    OPAL_OUTPUT_VERBOSE((2, orte_debug_output,
                         "%s setting up session dir with\n\ttmpdir: %s\n\tuser %s\n\thost %s\n\tjobid %s\n\tprocid %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         (NULL == orte_process_info.tmpdir_base) ? "UNDEF" : orte_process_info.tmpdir_base,
                         orte_system_info.user, orte_system_info.nodename, jobid_str, procid_str));

    if (ORTE_SUCCESS != (ret = orte_session_dir(true,
                                orte_process_info.tmpdir_base,
                                orte_system_info.user,
                                orte_system_info.nodename, NULL,
                                jobid_str, procid_str))) {
        if (jobid_str != NULL) free(jobid_str);
        if (procid_str != NULL) free(procid_str);
        ORTE_ERROR_LOG(ret);
        error = "orte_session_dir";
        goto error;
    }
    if (NULL != jobid_str) {
        free(jobid_str);
    }
    if (NULL != procid_str) {
        free(procid_str);
    }

    /* Once the session directory location has been established, set
       the opal_output hnp file location to be in the
       proc-specific session directory. */
    opal_output_set_output_file_info(orte_process_info.proc_session_dir,
                                     "output-", NULL, NULL);

    /* save my contact info in a file for others to find */
    contact_path = opal_os_path(false, orte_process_info.job_session_dir,
                "contact.txt", NULL);
    
    OPAL_OUTPUT_VERBOSE((2, orte_debug_output,
                         "%s writing contact file %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         contact_path));
    
    if (ORTE_SUCCESS != (ret = orte_write_hnp_contact_file(contact_path))) {
        OPAL_OUTPUT_VERBOSE((2, orte_debug_output,
                             "%s writing contact file failed with error %s",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                             ORTE_ERROR_NAME(ret)));
    } else {
        OPAL_OUTPUT_VERBOSE((2, orte_debug_output,
                             "%s wrote contact file",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
    }
    free(contact_path);

    /* Setup the job data object for the daemons */        
    /* create and store the job data object */
    jdata = OBJ_NEW(orte_job_t);
    jdata->jobid = ORTE_PROC_MY_NAME->jobid;
    opal_pointer_array_add(orte_job_data, jdata);
   
    /* create and store a node object where we are */
    node = OBJ_NEW(orte_node_t);
    node->name = strdup(orte_system_info.nodename);
    node->index = opal_pointer_array_add(orte_node_pool, node);
    
    /* create and store a proc object for us */
    proc = OBJ_NEW(orte_proc_t);
    proc->name.jobid = ORTE_PROC_MY_NAME->jobid;
    proc->name.vpid = ORTE_PROC_MY_NAME->vpid;
    proc->pid = orte_process_info.pid;
    proc->rml_uri = orte_rml.get_contact_info();
    proc->state = ORTE_PROC_STATE_RUNNING;
    OBJ_RETAIN(node);  /* keep accounting straight */
    proc->node = node;
    opal_pointer_array_add(jdata->procs, proc);

    /* record that the daemon (i.e., us) is on this node 
     * NOTE: we do not add the proc object to the node's
     * proc array because we are not an application proc.
     * Instead, we record it in the daemon field of the
     * node object
     */
    OBJ_RETAIN(proc);   /* keep accounting straight */
    node->daemon = proc;
    node->daemon_launched = true;
    node->state = ORTE_NODE_STATE_UP;
    
    /* record that the daemon job is running */
    jdata->num_procs = 1;
    jdata->state = ORTE_JOB_STATE_RUNNING;
    
    /* setup the routed info - the selected routed component
     * will know what to do. 
     */
    if (ORTE_SUCCESS != (ret = orte_routed.init_routes(ORTE_PROC_MY_NAME->jobid, NULL))) {
        ORTE_ERROR_LOG(ret);
        error = "orte_routed.init_routes";
        goto error;
    }
    
    /*
     * setup I/O forwarding system - must come after we init routes */
    if (ORTE_SUCCESS != (ret = orte_iof_base_open())) {
        ORTE_ERROR_LOG(ret);
        error = "orte_iof_base_open";
        goto error;
    }
    if (ORTE_SUCCESS != (ret = orte_iof_base_select())) {
        ORTE_ERROR_LOG(ret);
        error = "orte_iof_base_select";
        goto error;
    }
    
    /* setup the FileM */
    if (ORTE_SUCCESS != (ret = orte_filem_base_open())) {
        ORTE_ERROR_LOG(ret);
        error = "orte_filem_base_open";
        goto error;
    }
    
    if (ORTE_SUCCESS != (ret = orte_filem_base_select())) {
        ORTE_ERROR_LOG(ret);
        error = "orte_filem_base_select";
        goto error;
    }

#if OPAL_ENABLE_FT == 1
    /*
     * Setup the SnapC
     */
    if (ORTE_SUCCESS != (ret = orte_snapc_base_open())) {
        ORTE_ERROR_LOG(ret);
        error = "orte_snapc_base_open";
        goto error;
    }

    if (ORTE_SUCCESS != (ret = orte_snapc_base_select(orte_process_info.hnp, !orte_process_info.daemon))) {
        ORTE_ERROR_LOG(ret);
        error = "orte_snapc_base_select";
        goto error;
    }

    /* For HNP, ORTE doesn't need the OPAL CR stuff */
    opal_cr_set_enabled(false);
#else
    opal_cr_set_enabled(false);
#endif

    /*
     * Initalize the CR setup
     * Note: Always do this, even in non-FT builds.
     * If we don't some user level tools may hang.
     */
    if (ORTE_SUCCESS != (ret = orte_cr_init())) {
        ORTE_ERROR_LOG(ret);
        error = "orte_cr_init";
        goto error;
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
    char *contact_path;
    
    /* remove my contact info file */
    contact_path = opal_os_path(false, orte_process_info.top_session_dir,
                                "contact.txt", NULL);
    unlink(contact_path);
    free(contact_path);
    
    orte_cr_finalize();
    
#if OPAL_ENABLE_FT == 1
    orte_snapc_base_close();
#endif
    orte_filem_base_close();
    
    orte_odls_base_close();
    
    orte_wait_finalize();
    orte_iof_base_close();
    
    /* finalize selected modules so they can de-register
     * any receives
     */
    orte_ras_base_close();
    orte_rmaps_base_close();
    orte_plm_base_close();
    orte_errmgr_base_close();
    
    /* now can close the rml and its friendly group comm */
    orte_grpcomm_base_close();
    orte_routed_base_close();
    orte_rml_base_close();
    
    orte_session_dir_finalize(ORTE_PROC_MY_NAME);
    
    /* clean out the global structures */
    orte_sys_info_finalize();
    orte_proc_info_finalize();
    
    return ORTE_SUCCESS;    
}

/*
 * For application procs, we do NOT call the regular
 * C-library "abort" function, even though that would have
 * alerted us to the fact that this is an abnormal termination,
 * because it would automatically cause  a core file to be
 * generated. On large systems, that can be overwhelming
 * (imagine a few thousand Gbyte-sized files hitting
 * a shared file system simultaneously...ouch!).
 *
 * However, the HNP is only ONE process, so we can do it
 * here as the core file might prove useful.
 */
static void rte_abort(int status, bool report)
{
    /* do NOT do a normal finalize as this will very likely
     * hang the process. We are aborting due to an abnormal condition
     * that precludes normal cleanup 
     *
     * We do need to do the following bits to make sure we leave a 
     * clean environment. Taken from orte_finalize():
     * - Assume errmgr cleans up child processes before we exit.
     */
    
    /* CRS cleanup since it may have a named pipe and thread active */
    orte_cr_finalize();
    
    /* - Clean out the global structures 
     * (not really necessary, but good practice)
     */
    orte_sys_info_finalize();
    orte_proc_info_finalize();
    
    /* Now abort */
    abort();
}
