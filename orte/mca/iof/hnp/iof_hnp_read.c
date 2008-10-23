/*
 * Copyright (c) 2004-2007 The Trustees of Indiana University and Indiana
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
 */

#include "orte_config.h"
#include "orte/constants.h"

#include <errno.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif  /* HAVE_UNISTD_H */
#ifdef HAVE_STRING_H
#include <string.h>
#endif  /* HAVE_STRING_H */

#include "orte/util/show_help.h"

#include "orte/mca/rml/rml.h"
#include "orte/mca/errmgr/errmgr.h"
#include "orte/util/name_fns.h"
#include "orte/runtime/orte_globals.h"
#include "orte/mca/ess/ess.h"

#include "orte/mca/iof/iof.h"
#include "orte/mca/iof/base/base.h"

#include "iof_hnp.h"

/* return true if we should read stdin from fd, false otherwise */
bool orte_iof_hnp_stdin_check(int fd)
{
#if !defined(__WINDOWS__) && defined(HAVE_TCGETPGRP)
    if( isatty(fd) && (getpgrp() != tcgetpgrp(fd)) ) {
        return false;
    }
#endif  /* !defined(__WINDOWS__) */
    return true;
}

void orte_iof_hnp_stdin_cb(int fd, short event, void *cbdata)
{
    bool should_process = orte_iof_hnp_stdin_check(0);
    
    if (should_process) {
        mca_iof_hnp_component.stdinev->active = true;
        opal_event_add(&(mca_iof_hnp_component.stdinev->ev), 0);
    } else {
        opal_event_del(&(mca_iof_hnp_component.stdinev->ev));
        mca_iof_hnp_component.stdinev->active = false;
    }
}

/* this is the read handler for my own child procs. In this case,
 * the data is going nowhere - I just output it myself
 */
void orte_iof_hnp_read_local_handler(int fd, short event, void *cbdata)
{
    orte_iof_read_event_t *rev = (orte_iof_read_event_t*)cbdata;
    unsigned char data[ORTE_IOF_BASE_MSG_MAX];
    int32_t numbytes;
    opal_list_item_t *item;
    
    OPAL_THREAD_LOCK(&mca_iof_hnp_component.lock);
    
    /* read up to the fragment size */
#if !defined(__WINDOWS__)
    numbytes = read(fd, data, sizeof(data));
#else
    {
        DWORD readed;
        HANDLE handle = (HANDLE)_get_osfhandle(fd);
        ReadFile(handle, data, sizeof(data), &readed, NULL);
        numbytes = (int)readed;
    }
#endif  /* !defined(__WINDOWS__) */
    
    if (numbytes < 0) {
        /* either we have a connection error or it was a non-blocking read */
        
        /* non-blocking, retry */
        if (EAGAIN == errno || EINTR == errno) {
            OPAL_THREAD_UNLOCK(&mca_iof_hnp_component.lock);
            return;
        } 

        OPAL_OUTPUT_VERBOSE((1, orte_iof_base.iof_output,
                             "%s iof:hnp:read handler %s Error on connection:%d",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                             ORTE_NAME_PRINT(&rev->name), fd));

        opal_event_del(&rev->ev);
        goto CLEAN_RETURN;
    }
    
    /* is this read from our stdin? */
    if (ORTE_IOF_STDIN & rev->tag) {
        /* cycle through our list of sinks */
        for (item = opal_list_get_first(&mca_iof_hnp_component.sinks);
             item != opal_list_get_end(&mca_iof_hnp_component.sinks);
             item = opal_list_get_next(item)) {
            orte_iof_sink_t* sink = (orte_iof_sink_t*)item;
            
            /* only look at stdin sinks */
            if (!(ORTE_IOF_STDIN & sink->tag)) {
                continue;
            }
            
            /* if the daemon is me, then this is a local sink */
            if (ORTE_PROC_MY_NAME->jobid == sink->daemon.jobid &&
                ORTE_PROC_MY_NAME->vpid == sink->daemon.vpid) {
                OPAL_OUTPUT_VERBOSE((1, orte_iof_base.iof_output,
                                     "%s read %d bytes from stdin - writing to %s",
                                     ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), numbytes,
                                     ORTE_NAME_PRINT(&rev->name)));
                /* if stdin was closed, we need to close it too so the proc
                 * knows it is done
                 */
                if (0 == numbytes) {
                    /* make sure the write event is off */
                    if (!sink->wev.pending) {
                        opal_event_del(&(sink->wev.ev));
                    }
                    close(sink->wev.fd);
                    sink->wev.fd =-1;
                } else if (sink->wev.fd < 0) {
                    /* the fd has already been closed or this doesn't refer to a local
                     * sink - skip this entry */
                    continue;
                } else {
                    /* send the bytes down the pipe */
                    if (ORTE_IOF_MAX_INPUT_BUFFERS < orte_iof_base_write_output(&rev->name, rev->tag, data, numbytes, &sink->wev)) {
                        /* getting too backed up - stop the read event for now if it is still active */
                        if (!mca_iof_hnp_component.stdinev->active) {
                            opal_event_del(&(mca_iof_hnp_component.stdinev->ev));
                            mca_iof_hnp_component.stdinev->active = false;
                        }
                    }
                }
            } else {
                OPAL_OUTPUT_VERBOSE((1, orte_iof_base.iof_output,
                                     "%s sending data to daemon %s",
                                     ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                     ORTE_NAME_PRINT(&sink->daemon)));
                
                /* send the data to the daemon so it can
                 * write it to the proc's fd - in this case,
                 * we pass sink->name to indicate who is to
                 * receive the data. If the connection closed,
                 * numbytes will be zero so zero bytes will be
                 * sent - this will tell the daemon to close
                 * the fd for stdin to that proc
                 */
                orte_iof_hnp_send_data_to_endpoint(&sink->daemon, &sink->name, ORTE_IOF_STDIN, data, numbytes);
            }
        }
        /* check if stdin was closed */
        if (0 == numbytes) {
            opal_event_del(&rev->ev);
        }
        /* nothing more to do */
        goto CLEAN_RETURN;
    }
    
    /* this must be output from one of my local procs - see
     * if anyone else has requested a copy of this info
     */
    for (item = opal_list_get_first(&mca_iof_hnp_component.sinks);
         item != opal_list_get_end(&mca_iof_hnp_component.sinks);
         item = opal_list_get_next(item)) {
        orte_iof_sink_t *sink = (orte_iof_sink_t*)item;
        if (sink->tag & rev->tag &&
            sink->name.jobid == rev->name.jobid &&
            (ORTE_VPID_WILDCARD == sink->name.vpid || sink->name.vpid == rev->name.vpid)) {
            /* need to send the data to the remote endpoint - if
             * the connection closed, numbytes will be zero, so
             * the remote endpoint will know to close its local fd.
             * In this case, we pass rev->name to indicate who the
             * data came from.
             */
            OPAL_OUTPUT_VERBOSE((1, orte_iof_base.iof_output,
                                 "%s sending data to tool %s",
                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                 ORTE_NAME_PRINT(&sink->daemon)));
            orte_iof_hnp_send_data_to_endpoint(&sink->daemon, &rev->name, rev->tag, data, numbytes);
            if (0 == numbytes) {
                opal_event_del(&rev->ev);
            }
        }
    }

    OPAL_OUTPUT_VERBOSE((1, orte_iof_base.iof_output,
                         "%s read %d bytes from %s of %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), numbytes,
                         (ORTE_IOF_STDOUT & rev->tag) ? "stdout" : ((ORTE_IOF_STDERR & rev->tag) ? "stderr" : "stddiag"),
                         ORTE_NAME_PRINT(&rev->name)));
    
    /* if we read 0 bytes from the stdout/err/diag, there is
     * nothing to output - we do not close these file descriptors,
     * but we do terminate the event
     */
    if (0 == numbytes) {
        opal_event_del(&rev->ev);
        goto CLEAN_RETURN;
    }
    
    data[numbytes] = '\0';
    
    if (ORTE_IOF_STDOUT & rev->tag) {
        orte_iof_base_write_output(&rev->name, rev->tag, data, numbytes, &orte_iof_base.iof_write_stdout);
    } else {
        orte_iof_base_write_output(&rev->name, rev->tag, data, numbytes, &orte_iof_base.iof_write_stderr);

    }

CLEAN_RETURN:
    OPAL_THREAD_UNLOCK(&mca_iof_hnp_component.lock);
    
    /* since the event is persistent, we do not need to re-add it */
    return;
}