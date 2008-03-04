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
 * Copyright (c) 2007      Los Alamos National Security, LLC.  All rights
 *                         reserved. 
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
#include "orte_config.h"
#include "orte/constants.h"

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <errno.h>
#include <string.h>
#include <sys/stat.h>

#include "opal/class/opal_list.h"
#include "opal/util/output.h"
#include "opal/util/argv.h"
#include "opal/mca/mca.h"
#include "opal/mca/base/base.h"
#include "opal/util/show_help.h"
#include "opal/threads/mutex.h"
#include "opal/util/if.h"
#include "opal/util/os_path.h"
#include "opal/util/path.h"
#include "opal/mca/installdirs/installdirs.h"

#include "orte/util/sys_info.h"
#include "orte/mca/errmgr/errmgr.h"
#include "orte/mca/ras/ras_types.h"
#include "orte/runtime/orte_globals.h"

#include "orte/util/hostfile/hostfile_lex.h"
#include "orte/util/hostfile/hostfile.h"

static opal_mutex_t hostfile_mutex;

static const char *cur_hostfile_name = NULL;

static void hostfile_parse_error(int token)
{
    switch (token) {
    case ORTE_HOSTFILE_STRING:
        opal_show_help("help-hostfile.txt", "parse_error_string",
                       true,
                       cur_hostfile_name, 
                       orte_util_hostfile_line, 
                       token, 
                       orte_util_hostfile_value.sval);
        break;
    case ORTE_HOSTFILE_IPV4:
    case ORTE_HOSTFILE_IPV6:
    case ORTE_HOSTFILE_INT:
        opal_show_help("help-hostfile.txt", "parse_error_int",
                       true,
                       cur_hostfile_name, 
                       orte_util_hostfile_line, 
                       token, 
                       orte_util_hostfile_value.ival);
        break;
     default:
        opal_show_help("help-hostfile.txt", "parse_error",
                       true,
                       cur_hostfile_name, 
                       orte_util_hostfile_line, 
                       token );
        break;
    }
}

 /**
  * Return the integer following an = (actually may only return positive ints)
  */
static int hostfile_parse_int(void)
{
    if (ORTE_HOSTFILE_EQUAL != orte_util_hostfile_lex())
        return -1;
    if (ORTE_HOSTFILE_INT != orte_util_hostfile_lex())
        return -1;
    return orte_util_hostfile_value.ival;
}

/**
 * Return the string following an = (option to a keyword)
 */
static char *hostfile_parse_string(void)
{
    int rc;
    if (ORTE_HOSTFILE_EQUAL != orte_util_hostfile_lex()){
        return NULL;
    }
    rc = orte_util_hostfile_lex();
    if (ORTE_HOSTFILE_STRING != rc){
        return NULL;
    }
    return strdup(orte_util_hostfile_value.sval);
}

static char *hostfile_parse_string_or_int(void)
{
  int rc;
  char tmp_str[64];

  if (ORTE_HOSTFILE_EQUAL != orte_util_hostfile_lex()){
      return NULL;
  }
  rc = orte_util_hostfile_lex();
  switch (rc) {
  case ORTE_HOSTFILE_STRING:
      return strdup(orte_util_hostfile_value.sval);
  case ORTE_HOSTFILE_INT:
      sprintf(tmp_str,"%d",orte_util_hostfile_value.ival);
      return strdup(tmp_str);
  default:
      return NULL;
  }

}

static orte_node_t* hostfile_lookup(opal_list_t* nodes, const char* name)
{
    opal_list_item_t* item;
    for(item =  opal_list_get_first(nodes);
        item != opal_list_get_end(nodes);
        item =  opal_list_get_next(item)) {
        orte_node_t* node = (orte_node_t*)item;
        if(strcmp(node->name, name) == 0) {
            opal_list_remove_item(nodes, item);
            return node;
        }
    }
    return NULL;
}

static int validate_slot_list (char *cpu_list)
{
    int i,list_len = strlen(cpu_list);

    for(i=0; i < list_len; i++){
        if ('0' <= cpu_list[i] && '9' >= cpu_list[i]) {
            continue;
        }
        else if (':' == cpu_list[i]) {
            continue;
        }
        else if (',' == cpu_list[i]) {
            continue;
        }
        else if ('*' == cpu_list[i]) {
            continue;
        }
        else if ('-' == cpu_list[i]) {
            continue;
        }
        return ORTE_ERROR;
    }
    return ORTE_SUCCESS;
}

static int hostfile_parse_line(int token, opal_list_t* updates, opal_list_t* procs)
{
    char buff[64];
    char *tmp;
    int rc;
    orte_node_t* node;
    orte_ras_proc_t* proc;
    bool update = false;
    bool got_count = false;
    bool got_max = false;
    char* value;
    char** argv;
    char* node_name = NULL;
    char* username = NULL;
    int cnt;
    int number_of_slots = 0;

    if (ORTE_HOSTFILE_STRING == token ||
        ORTE_HOSTFILE_HOSTNAME == token ||
        ORTE_HOSTFILE_INT == token ||
        ORTE_HOSTFILE_IPV4 == token ||
        ORTE_HOSTFILE_IPV6 == token) {
        
        if(ORTE_HOSTFILE_INT == token) {
            sprintf(buff,"%d", orte_util_hostfile_value.ival);
            value = buff;
        } else {
            value = orte_util_hostfile_value.sval;
        }
        argv = opal_argv_split (value, '@');
        
        cnt = opal_argv_count (argv);
        if (1 == cnt) {
            node_name = strdup(argv[0]);
        } else if (2 == cnt) {
            username = strdup(argv[0]);
            node_name = strdup(argv[1]);
        } else {
            opal_output(0, "WARNING: Unhandled user@host-combination\n"); /* XXX */
        }
        opal_argv_free (argv);

        /* convert this into something globally unique */
        if (strcmp(node_name, "localhost") == 0 || opal_ifislocal(node_name)) {
            /* Nodename has been allocated, that is for sure */
            free (node_name);
            node_name = strdup(orte_system_info.nodename);
        }

        /* Do we need to make a new node object?  First check to see
           if it's already in the updates list */
        if (NULL == (node = hostfile_lookup(updates, node_name))) {
            node = OBJ_NEW(orte_node_t);
            node->name = node_name;
            node->username = username;
            node->slots = 0;
            
            /* Note that we need to set update to true regardless of
                whether the node was found on the updates list or not.
                If it was found, we just removed it (in hostfile_lookup()),
                so the update puts it back
                (potentially after updating it, of course).  If it was
                not found, then we have a new node instance that needs
                to be added to the updates list. */
            update = true;
        }
    } else {
        hostfile_parse_error(token);
        return ORTE_ERROR;
    }

    got_count = false;
    while (!orte_util_hostfile_done) {
        token = orte_util_hostfile_lex();

        switch (token) {
        case ORTE_HOSTFILE_DONE:
            goto done;

        case ORTE_HOSTFILE_NEWLINE:
            goto done;

        case ORTE_HOSTFILE_USERNAME:
            node->username = hostfile_parse_string();
            break;

        case ORTE_HOSTFILE_COUNT:
        case ORTE_HOSTFILE_CPU:
        case ORTE_HOSTFILE_SLOTS:
            rc = hostfile_parse_int();
            if (rc < 0) {
                opal_show_help("help-hostfile.txt", "slots",
                               true,
                               cur_hostfile_name, rc);
                OBJ_RELEASE(node);
                return ORTE_ERROR;
            }
            node->slots += rc;
            update = true;
            got_count = true;

            /* Ensure that slots_max >= slots */
            if (node->slots_max != 0 && node->slots_max < node->slots) {
                node->slots_max = node->slots;
            }
            break;

        case ORTE_HOSTFILE_SLOT:
            tmp = hostfile_parse_string_or_int();
            proc = OBJ_NEW(orte_ras_proc_t);
            proc->node_name = strdup(node_name);
            argv = opal_argv_split (tmp, '@');
            cnt = opal_argv_count (argv);
            switch (cnt) {
            case 1:
                proc->cpu_list = strdup(argv[0]);
                if (ORTE_SUCCESS != (rc = validate_slot_list(proc->cpu_list))) {
                    OBJ_RELEASE(proc);
                    OBJ_RELEASE(node);
                    opal_argv_free (argv);
                    ORTE_ERROR_LOG(ORTE_ERR_BAD_PARAM);
                    hostfile_parse_error(token);
                    return ORTE_ERROR;
                }
                break;
            case 2:
                proc->rank = strtol(argv[0],(char **)NULL,0);
                proc->cpu_list = strdup(argv[1]);
                if (ORTE_SUCCESS != (rc = validate_slot_list(proc->cpu_list))) {
                    OBJ_RELEASE(proc);
                    OBJ_RELEASE(node);
                    opal_argv_free (argv);
                    ORTE_ERROR_LOG(ORTE_ERR_BAD_PARAM);
                    hostfile_parse_error(token);
                    return ORTE_ERROR;
                }
                break;
            default:
                OBJ_RELEASE(proc);
                OBJ_RELEASE(node);
                opal_argv_free (argv);
                ORTE_ERROR_LOG(ORTE_ERR_BAD_PARAM);
                hostfile_parse_error(token);
                return ORTE_ERROR;
            }
            opal_argv_free (argv);
            opal_list_append(procs, &proc->super);
            number_of_slots++;
            break;
        case ORTE_HOSTFILE_SLOTS_MAX:
            rc = hostfile_parse_int();
            if (rc < 0) {
                opal_show_help("help-hostfile.txt", "max_slots",
                               true,
                               cur_hostfile_name, ((size_t) rc));
                OBJ_RELEASE(node);
                return ORTE_ERROR;
            }
            /* Only take this update if it puts us >= node_slots */
            if (rc >= node->slots) {
                if (node->slots_max != rc) {
                    node->slots_max = rc;
                    update = true;
                    got_max = true;
                }
            } else {
                opal_show_help("help-hostfile.txt", "max_slots_lt",
                               true,
                               cur_hostfile_name, node->slots, rc);
                ORTE_ERROR_LOG(ORTE_ERR_BAD_PARAM);
                OBJ_RELEASE(node);
                return ORTE_ERROR;
            }
            break;

        default:
            hostfile_parse_error(token);
            OBJ_RELEASE(node);
            return ORTE_ERROR;
        }
        if (number_of_slots > node->slots) {
            ORTE_ERROR_LOG(ORTE_ERR_BAD_PARAM);
            OBJ_RELEASE(node);
            return ORTE_ERROR;
        }
    }

done:
    if (update) {
        if (!got_count) {
            if (got_max) {
                node->slots = node->slots_max;
            } else {
                ++node->slots;
            }
        }
        opal_list_append(updates, &node->super);
    } else {
        OBJ_RELEASE(node);
    }
    return ORTE_SUCCESS;
}


/**
 * Parse the specified file into a node list.
 */

static int hostfile_parse(const char *hostfile, opal_list_t* updates, opal_list_t* procs)
{
    int token;
    int rc = ORTE_SUCCESS;

    OPAL_THREAD_LOCK(&hostfile_mutex);
    
    cur_hostfile_name = hostfile;
    
    orte_util_hostfile_done = false;
    orte_util_hostfile_in = fopen(hostfile, "r");
    if (NULL == orte_util_hostfile_in) {
        opal_show_help("help-hostfile.txt", "no-hostfile", true, hostfile);
        rc = ORTE_ERR_NOT_FOUND;
        goto unlock;
    }

    while (!orte_util_hostfile_done) {
        token = orte_util_hostfile_lex();

        switch (token) {
        case ORTE_HOSTFILE_DONE:
            orte_util_hostfile_done = true;
            break;

        case ORTE_HOSTFILE_NEWLINE:
            break;

        /*
         * This looks odd, since we have several forms of host-definitions:
         *   hostname              just plain as it is, being a ORTE_HOSTFILE_STRING
         *   IP4s and user@IPv4s
         *   hostname.domain and user@hostname.domain
         */
        case ORTE_HOSTFILE_STRING:
        case ORTE_HOSTFILE_INT:
        case ORTE_HOSTFILE_HOSTNAME:
        case ORTE_HOSTFILE_IPV4:
        case ORTE_HOSTFILE_IPV6:
            rc = hostfile_parse_line(token, updates, procs);
            if (ORTE_SUCCESS != rc) {
                goto unlock;
            }
            break;

        default:
            hostfile_parse_error(token);
            goto unlock;
        }
    }
    fclose(orte_util_hostfile_in);
    orte_util_hostfile_in = NULL;

unlock:
    cur_hostfile_name = NULL;

    OPAL_THREAD_UNLOCK(&hostfile_mutex);
    return rc;
}


/**
 * Parse the provided hostfile and add the nodes to the list.
 */

int orte_util_add_hostfile_nodes(opal_list_t *nodes,
                                 bool *override_oversubscribed,
                                 char *hostfile)
{
    opal_list_t procs;
    opal_list_item_t *item;
    int rc;

    OBJ_CONSTRUCT(&procs, opal_list_t);
    
    /* parse the hostfile and add the contents to the list */
    if (ORTE_SUCCESS != (rc = hostfile_parse(hostfile, nodes, &procs))) {
        goto cleanup;
    }
    
    /* indicate that ORTE should override any oversubscribed conditions
     * based on local hardware limits since the user (a) might not have
     * provided us any info on the #slots for a node, and (b) the user
     * might have been wrong! If we don't check the number of local physical
     * processors, then we could be too aggressive on our sched_yield setting
     * and cause performance problems.
     */
    *override_oversubscribed = true;

cleanup:
    while(NULL != (item = opal_list_remove_first(&procs))) {
        OBJ_RELEASE(item);
    }

    OBJ_DESTRUCT(&procs);

    return rc;
}

/* Parse the provided hostfile and filter the nodes that are
 * on the input list, removing those that
 * are not found in the hostfile
 */
int orte_util_filter_hostfile_nodes(opal_list_t *nodes,
                                    char *hostfile)
{
    opal_list_t newnodes;
    opal_list_item_t *item1, *item2;
    orte_node_t *node_from_list, *node_from_file;
    bool node_found;
    int rc;
    
    /* parse the hostfile and create local list of findings */
    OBJ_CONSTRUCT(&newnodes, opal_list_t);
    if (ORTE_SUCCESS != (rc = hostfile_parse(hostfile, &newnodes, NULL))) {
        OBJ_DESTRUCT(&newnodes);
        return rc;
    }
    
    /* now check our nodes and remove any that don't match. We can
     * destruct our hostfile list as we go since this won't be needed
     */
    for (item2 = opal_list_get_first(nodes);
         item2 != opal_list_get_end(nodes);
         item2 = opal_list_get_next(item2)) {
        node_from_list = (orte_node_t*)item2;
        node_found = false;

        for (item1 = opal_list_get_first(&newnodes);
             item1 != opal_list_get_end(&newnodes);
             item1 = opal_list_get_next(item1)) {
            node_from_file = (orte_node_t*)item1;
            /* since the name in the hostfile might not match
             * our local name, and yet still be intended to match,
             * we have to check for localhost and local interfaces
             */
            if (0 == strcmp(node_from_file->name, node_from_list->name) ||
                (0 == strcmp(node_from_list->name, orte_system_info.nodename) &&
                 (0 == strcmp(node_from_file->name, "localhost") || 
                  opal_ifislocal(node_from_file->name)))) {
                node_found = true;
                opal_list_remove_item(&newnodes, item1);
                OBJ_RELEASE( item1 );
                break;
            }
        }
        if( false == node_found ) {
            opal_list_remove_item(nodes, item2);
            OBJ_RELEASE( item2 );
        }
    }
    
    /* if we still have entries on our hostfile list, then
     * there were requested hosts that were not in our allocation.
     * This is an error - report it to the user and return an error
     */
    if (0 != opal_list_get_size(&newnodes)) {
        opal_show_help("help-hostfile.txt", "not-all-mapped-alloc",
                       true, hostfile);
        while (NULL != (item1 = opal_list_remove_first(&newnodes))) {
            OBJ_RELEASE(item1);
        }
        OBJ_DESTRUCT(&newnodes);
        return ORTE_ERR_SILENT;
    }

    OBJ_DESTRUCT(&newnodes);

    return ORTE_SUCCESS;
}