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
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 *
 */

#include "orte_config.h"
#include "orte/constants.h"

#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <signal.h>
#include <errno.h>

#include "opal/util/argv.h"
#include "opal/util/output.h"
#include "opal/util/path.h"
#include "opal/util/show_help.h"
#include "opal/mca/base/mca_base_param.h"
#include "opal/mca/installdirs/installdirs.h"

#include "orte/util/proc_info.h"
#include "orte/mca/errmgr/errmgr.h"
#include "orte/mca/iof/iof.h"
#include "orte/mca/rml/rml.h"
#include "orte/mca/rml/base/rml_contact.h"
#include "orte/mca/routed/routed.h"
#include "orte/util/name_fns.h"
#include "orte/runtime/orte_globals.h"

#include "orte/mca/ess/ess.h"
#include "orte/mca/ess/base/base.h"
#include "orte/mca/ess/singleton/ess_singleton.h"

static int fork_hnp(void);

static void set_handler_default(int sig)
{
#if !defined(__WINDOWS__)
    struct sigaction act;
    
    act.sa_handler = SIG_DFL;
    act.sa_flags = 0;
    sigemptyset(&act.sa_mask);
    
    sigaction(sig, &act, (struct sigaction *)0);
#endif /* !defined(__WINDOWS__) */
}

static int rte_init(char flags);

orte_ess_base_module_t orte_ess_singleton_module = {
    rte_init,
    orte_ess_base_app_finalize,
    orte_ess_base_app_abort
};

static int rte_init(char flags)
{
    int rc;

    /*
     * If we are the selected module, then we must be a singleton
     * as it means that no other method for discovering a name
     * could be found. In this case, we need to start a daemon that
     * can support our operation. We must do this for two reasons:
     *
     * (1) if we try to play the role of the HNP, then any child processes
     * we might start via comm_spawn will rely on us for all ORTE-level
     * support. However, we can only progress those requests when the
     * the application calls into the OMPI/ORTE library! Thus, if this
     * singleton just does computation, the other processes will "hang"
     * in any calls into the ORTE layer that communicate with the HNP -
     * and most calls on application procs *do*.
     *
     * (2) daemons are used to communicate messages for administrative
     * purposes in a broadcast-like manner. Thus, daemons are expected
     * to be able to interpret specific commands. Our application process
     * doesn't have any idea how to handle those commands, thus causing
     * the entire ORTE administrative system to break down.
     *
     * For those reasons, we choose to fork/exec a daemon at this time
     * and then reconnect ourselves to it. We could just "fork" and declare
     * the child to be a daemon, but that would require we place *all* of the
     * daemon command processing code in the ORTE library, do some strange
     * mojo in a few places, etc. This doesn't seem worth it, so we'll just
     * do the old fork/exec here
     *
     * Note that Windows-based systems have to do their own special trick as
     * they don't support fork/exec. So we have to use a giant "if" here to
     * protect the Windows world. To make the results more readable, we put
     * the whole mess in a separate function below
     */
    if (ORTE_SUCCESS != (rc= fork_hnp())) {
        /* if this didn't work, then we cannot support operation any further.
        * Abort the system and tell orte_init to exit
        */
        ORTE_ERROR_LOG(rc);
        return rc;
    }

    orte_process_info.num_procs = 1;
    /* since we are a singleton, then we must have a local_rank of 0
     * and only 1 local process
     */
    orte_process_info.local_rank = 0;
    orte_process_info.num_local_procs = 1;
    
    /* use the std app init to complete the procedure */
    if (ORTE_SUCCESS != (rc = orte_ess_base_app_setup())) {
        ORTE_ERROR_LOG(rc);
        return rc;
    }
    
    /* wireup our io */
    if (ORTE_SUCCESS != (rc = orte_iof.iof_pull(ORTE_PROC_MY_NAME, ORTE_NS_CMP_JOBID, ORTE_IOF_STDOUT, 1))) {
        ORTE_ERROR_LOG(rc);
        return rc;
    }
    if (ORTE_SUCCESS != (rc = orte_iof.iof_pull(ORTE_PROC_MY_NAME, ORTE_NS_CMP_JOBID, ORTE_IOF_STDERR, 2))) {
        ORTE_ERROR_LOG(rc);
        return rc;
    }
    if (ORTE_SUCCESS != (rc = orte_iof.iof_push(ORTE_PROC_MY_NAME, ORTE_NS_CMP_JOBID, ORTE_IOF_STDIN, 0))) {
        ORTE_ERROR_LOG(rc);
        return rc;
    }
    
    return ORTE_SUCCESS;
}


#define ORTE_URI_MSG_LGTH   256

static int fork_hnp(void)
{
#if !defined(__WINDOWS__)
    int p[2], death_pipe[2];
    char *cmd;
    char **argv = NULL;
    int argc;
    char *param;
    sigset_t sigs;
    int buffer_length, num_chars_read, chunk;
    char *orted_uri;
    int rc;
    
    /* A pipe is used to communicate between the parent and child to
        indicate whether the exec ultiimately succeeded or failed.  The
        child sets the pipe to be close-on-exec; the child only ever
        writes anything to the pipe if there is an error (e.g.,
        executable not found, exec() fails, etc.).  The parent does a
        blocking read on the pipe; if the pipe closed with no data,
        then the exec() succeeded.  If the parent reads something from
        the pipe, then the child was letting us know that it failed.
        */
    if (pipe(p) < 0) {
        ORTE_ERROR_LOG(ORTE_ERR_SYS_LIMITS_PIPES);
        return ORTE_ERR_SYS_LIMITS_PIPES;
    }
    
    /* we also have to give the HNP a pipe it can watch to know when
     * we terminated. Since the HNP is going to be a child of us, it
     * can't just use waitpid to see when we leave - so it will watch
     * the pipe instead
     */
    if (pipe(death_pipe) < 0) {
        ORTE_ERROR_LOG(ORTE_ERR_SYS_LIMITS_PIPES);
        return ORTE_ERR_SYS_LIMITS_PIPES;
    }
    
    /* find the orted binary using the install_dirs support - this also
     * checks to ensure that we can see this executable and it *is* executable by us
     */
    cmd = opal_path_access("orted", opal_install_dirs.bindir, X_OK);
    if (NULL == cmd) {
        /* guess we couldn't do it - best to abort */
        ORTE_ERROR_LOG(ORTE_ERR_FILE_NOT_EXECUTABLE);
        close(p[0]);
        close(p[1]);
        return ORTE_ERR_FILE_NOT_EXECUTABLE;
    }
    
    /* okay, setup an appropriate argv */
    opal_argv_append(&argc, &argv, "orted");
    
    /* tell the daemon it is to be the HNP */
    opal_argv_append(&argc, &argv, "--hnp");

    /* tell the daemon to get out of our process group */
    opal_argv_append(&argc, &argv, "--set-sid");
    
    /* tell the daemon to report back its uri so we can connect to it */
    opal_argv_append(&argc, &argv, "--report-uri");
    asprintf(&param, "%d", p[1]);
    opal_argv_append(&argc, &argv, param);
    free(param);
    
    /* give the daemon a pipe it can watch to tell when we have died */
    opal_argv_append(&argc, &argv, "--singleton-died-pipe");
    asprintf(&param, "%d", death_pipe[0]);
    opal_argv_append(&argc, &argv, param);
    free(param);
    
    /* add any debug flags */
    if (orte_debug_flag) {
        opal_argv_append(&argc, &argv, "--debug");
    }

    if (orte_debug_daemons_flag) {
        opal_argv_append(&argc, &argv, "--debug-daemons");
    }
    
    if (orte_debug_daemons_file_flag) {
        if (!orte_debug_daemons_flag) {
            opal_argv_append(&argc, &argv, "--debug-daemons");
        }
        opal_argv_append(&argc, &argv, "--debug-daemons-file");
    }
    
    /* Fork off the child */
    orte_process_info.hnp_pid = fork();
    if(orte_process_info.hnp_pid < 0) {
        ORTE_ERROR_LOG(ORTE_ERR_SYS_LIMITS_CHILDREN);
        close(p[0]);
        close(p[1]);
        close(death_pipe[0]);
        close(death_pipe[1]);
        free(cmd);
        return ORTE_ERR_SYS_LIMITS_CHILDREN;
    }
    
    if (orte_process_info.hnp_pid == 0) {
        close(p[0]);
        close(death_pipe[1]);
        /* I am the child - exec me */
        
        /* Set signal handlers back to the default.  Do this close
           to the execve() because the event library may (and likely
           will) reset them.  If we don't do this, the event
           library may have left some set that, at least on some
           OS's, don't get reset via fork() or exec().  Hence, the
           orted could be unkillable (for example). */
        set_handler_default(SIGTERM);
        set_handler_default(SIGINT);
        set_handler_default(SIGHUP);
        set_handler_default(SIGPIPE);
        set_handler_default(SIGCHLD);
        
        /* Unblock all signals, for many of the same reasons that
           we set the default handlers, above.  This is noticable
           on Linux where the event library blocks SIGTERM, but we
           don't want that blocked by the orted (or, more
           specifically, we don't want it to be blocked by the
           orted and then inherited by the ORTE processes that it
           forks, making them unkillable by SIGTERM). */
        sigprocmask(0, 0, &sigs);
        sigprocmask(SIG_UNBLOCK, &sigs, 0);
        
        execv(cmd, argv);
        
        /* if I get here, the execv failed! */
        opal_show_help("help-ess-base.txt", "ess-base:execv-error",
                       true, cmd, strerror(errno));
        exit(1);
        
    } else {
       /* I am the parent - wait to hear something back and
         * report results
         */
        close(p[1]);  /* parent closes the write - orted will write its contact info to it*/
        close(death_pipe[0]);  /* parent closes the death_pipe's read */
        
        /* setup the buffer to read the name + uri */
        buffer_length = ORTE_URI_MSG_LGTH;
        chunk = ORTE_URI_MSG_LGTH-1;
        num_chars_read = 0;
        orted_uri = (char*)malloc(buffer_length);

        while (chunk == (rc = read(p[0], &orted_uri[num_chars_read], chunk))) {
            /* we read an entire buffer - better get more */
            num_chars_read += chunk;
            buffer_length += ORTE_URI_MSG_LGTH;
            orted_uri = realloc((void*)orted_uri, buffer_length);
        }
        num_chars_read += rc;

        if (num_chars_read <= 0) {
            /* we didn't get anything back - this is bad */
            ORTE_ERROR_LOG(ORTE_ERR_HNP_COULD_NOT_START);
            free(orted_uri);
            return ORTE_ERR_HNP_COULD_NOT_START;
        }
        
        /* parse the name from the returned info */
        if (']' != orted_uri[strlen(orted_uri)-1]) {
            ORTE_ERROR_LOG(ORTE_ERR_COMM_FAILURE);
            free(orted_uri);
            return ORTE_ERR_COMM_FAILURE;
        }
        orted_uri[strlen(orted_uri)-1] = '\0';
        if (NULL == (param = strrchr(orted_uri, '['))) {
            ORTE_ERROR_LOG(ORTE_ERR_COMM_FAILURE);
            free(orted_uri);
            return ORTE_ERR_COMM_FAILURE;
        }
        *param = '\0';  /* terminate the string */
        param++;
        if (ORTE_SUCCESS != (rc = orte_util_convert_string_to_process_name(ORTE_PROC_MY_NAME, param))) {
            ORTE_ERROR_LOG(rc);
            free(orted_uri);
            return rc;
        }
        /* save the daemon uri - we will process it later */
        orte_process_info.my_daemon_uri = strdup(orted_uri);
        
        /* likewise, since this is also the HNP, set that uri too */
        orte_process_info.my_hnp_uri = strdup(orted_uri);
        
       /* indicate we are a singleton so orte_init knows what to do */
        orte_process_info.singleton = true;
        /* all done - report success */
        free(orted_uri);
        return ORTE_SUCCESS;
    }
#else
    /* someone will have to devise a Windows equivalent */
#endif    
    
    return ORTE_SUCCESS;
}