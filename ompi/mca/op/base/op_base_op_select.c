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
 * Copyright (c) 2007      Lawrence Livermore National Security, LLC.  All
 *                         rights reserved.
 * Copyright (c) 2008      Sun Microsystems, Inc.  All rights reserved.
 * Copyright (c) 2008-2009 Cisco Systems, Inc.  All rights reserved.
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

#include "ompi_config.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "mpi.h"
#include "ompi/constants.h"

#include "opal/util/argv.h"
#include "opal/class/opal_list.h"
#include "opal/class/opal_object.h"
#include "opal/mca/mca.h"
#include "opal/mca/base/base.h"

#include "orte/util/show_help.h"

#include "ompi/op/op.h"
#include "ompi/mca/op/op.h"
#include "ompi/mca/op/base/base.h"
#include "ompi/mca/op/base/functions.h"


/*
 * Local types
 */
typedef struct avail_op_t {
    opal_list_item_t super;

    int ao_priority;
    ompi_op_base_module_1_0_0_t *ao_module;
} avail_op_t;


/*
 * Local functions
 */
static opal_list_t *check_components(opal_list_t *components, 
                                     ompi_op_t *op);
static int check_one_component(ompi_op_t *op, 
                               const mca_base_component_t *component,
                               ompi_op_base_module_1_0_0_t **module);

static int query(const mca_base_component_t *component, 
                 ompi_op_t *op, int *priority,
                 ompi_op_base_module_1_0_0_t **module);

static int query_1_0_0(const ompi_op_base_component_1_0_0_t *op_component, 
                       ompi_op_t *op, int *priority,
                       ompi_op_base_module_1_0_0_t **module);

/*
 * Stuff for the OBJ interface
 */
static OBJ_CLASS_INSTANCE(avail_op_t, opal_list_item_t, NULL, NULL);


/*
 * This function is called at the initialization time of every
 * *intrinsic* MPI_Op (it is *not* used for user-defined MPI_Ops!).
 * It is used to select which op component(s) will be active for a
 * given MPI_Op.
 *
 * This selection logic is not for the weak.
 */
int ompi_op_base_op_select(ompi_op_t *op)
{
    int i, ret;
    char name[MPI_MAX_OBJECT_NAME + 32];
    opal_list_t *selectable;
    opal_list_item_t *item;
    ompi_op_base_module_t *module;

    /* Announce */
    snprintf(name, sizeof(name), "%s", op->o_name);
    name[sizeof(name) - 1] = '\0';
    opal_output_verbose(10, ompi_op_base_output,
                        "op:base:op_select: new op: %s", 
                        name);

    /* Make a module for all the base functions so that other modules
       can RETAIN it (vs. having NULL for the base function modules,
       and forcing all other modules to check for NULL before calling
       RETAIN). */
    module = OBJ_NEW(ompi_op_base_module_t);

    /* Initialize all functions to point to the corresponding base
       functions.  Set the corresponding module pointers to NULL,
       indicating that these are base functions with no corresponding
       module. */
    memset(&op->o_func, 0, sizeof(op->o_func));
    memset(&op->o_3buff_intrinsic, 0, sizeof(op->o_3buff_intrinsic));
    for (i = 0; i < OMPI_OP_BASE_TYPE_MAX; ++i) {
        op->o_func.intrinsic.fns[i] = 
            ompi_op_base_functions[op->o_f_to_c_index][i];
        op->o_func.intrinsic.modules[i] = module;
        OBJ_RETAIN(module);
        op->o_3buff_intrinsic.fns[i] =
            ompi_op_base_3buff_functions[op->o_f_to_c_index][i];
        op->o_3buff_intrinsic.modules[i] = module;
        OBJ_RETAIN(module);
    }

    /* Offset the initial OBJ_NEW */
    OBJ_RELEASE(module);

    /* Check for any components that want to run.  It's not an error
       if there are none; we'll just use all the base functions in
       this case. */
    opal_output_verbose(10, ompi_op_base_output, 
                        "op:base:op_select: Checking all available components");
    selectable = check_components(&ompi_op_base_components_available, op);

    /* Do the selection loop.  The selectable list is in priority
       order; lowest priority first. */
    for (item = opal_list_remove_first(selectable);
         NULL != item; 
         item = opal_list_remove_first(selectable)) {
        avail_op_t *avail = (avail_op_t*) item;

        /* Enable the module */
        if (NULL != avail->ao_module->opm_enable) {
            ret = avail->ao_module->opm_enable(avail->ao_module, op);
            if (OMPI_SUCCESS != ret) {
                /* If the module fails to enable, just release it and move
                   on */
                OBJ_RELEASE(avail->ao_module);
                OBJ_RELEASE(avail);
                continue;
            }
        }

        /* Copy over the non-NULL pointers */
        for (i = 0; i < OMPI_OP_BASE_TYPE_MAX; ++i) {
            /* 2-buffer variants */
            if (NULL != avail->ao_module->opm_fns[i]) {
                if (NULL != op->o_func.intrinsic.modules[i]) {
                    OBJ_RELEASE(op->o_func.intrinsic.modules[i]);
                }
                op->o_func.intrinsic.fns[i] = avail->ao_module->opm_fns[i];
                op->o_func.intrinsic.modules[i] = avail->ao_module;
                OBJ_RETAIN(avail->ao_module);
            }

            /* 3-buffer variants */
            if (NULL != avail->ao_module->opm_3buff_fns[i]) {
                if (NULL != op->o_3buff_intrinsic.modules[i]) {
                    OBJ_RELEASE(op->o_func.intrinsic.modules[i]);
                }
                op->o_3buff_intrinsic.fns[i] = 
                    avail->ao_module->opm_3buff_fns[i];
                op->o_3buff_intrinsic.modules[i] = avail->ao_module;
                OBJ_RETAIN(avail->ao_module);
            }
        }

        /* release the original module reference and the list item */
        OBJ_RELEASE(avail->ao_module);
        OBJ_RELEASE(avail);
    }

    /* Done with the list from the check_components() call so release it. */
    OBJ_RELEASE(selectable);

    /* Sanity check: for intrinsic MPI_Ops, we should have exactly the
       same pointers non-NULL as the corresponding initial table row
       in ompi_op_base_functions / ompi_op_base_3buff_functions.  The
       values may be different, of course, but the pattern of
       NULL/non-NULL should be exactly the same. */
    for (i = 0; i < OMPI_OP_BASE_TYPE_MAX; ++i) {
        if ((NULL == ompi_op_base_functions[op->o_f_to_c_index][i] &&
             NULL != op->o_func.intrinsic.fns[i]) ||
            (NULL != ompi_op_base_functions[op->o_f_to_c_index][i] &&
             NULL == op->o_func.intrinsic.fns[i])) {
            /* Oops -- we found a mismatch.  This shouldn't happen; so
               go release everything and return an error (yes, re-use
               the "i" index because we're going to return without
               completing the outter loop). */
            for (i = 0; i < OMPI_OP_BASE_TYPE_MAX; ++i) {
                if (NULL != op->o_func.intrinsic.modules[i]) {
                    OBJ_RELEASE(op->o_func.intrinsic.modules[i]);
                    op->o_func.intrinsic.modules[i] = NULL;
                }
                op->o_func.intrinsic.fns[i] = NULL;
                return OMPI_ERR_NOT_FOUND;
            }
        }
    }

    return OMPI_SUCCESS;
}


/*
 * For each module in the list, check and see if it wants to run, and
 * do the resulting priority comparison.  Make a list of modules to be
 * only those who returned that they want to run, and put them in
 * priority order (lowest to highest).
 */
static opal_list_t *check_components(opal_list_t *components, 
                                     ompi_op_t *op)
{
    int priority;
    const mca_base_component_t *component;
    opal_list_item_t *item, *item2;
    ompi_op_base_module_1_0_0_t *module;
    opal_list_t *selectable;
    avail_op_t *avail, *avail2;
  
    /* Make a list of the components that query successfully */
    selectable = OBJ_NEW(opal_list_t);

    /* Scan through the list of components.  This nested loop is O(N^2),
       but we should never have too many components and/or names, so this
       *hopefully* shouldn't matter... */
  
    for (item = opal_list_get_first(components); 
         item != opal_list_get_end(components); 
         item = opal_list_get_next(item)) {
        component = ((mca_base_component_priority_list_item_t *) 
                     item)->super.cli_component;

        priority = check_one_component(op, component, &module);
        if (priority >= 0) {
            
            /* We have a component that indicated that it wants to run by
               giving us a module */
            avail = OBJ_NEW(avail_op_t);
            avail->ao_priority = priority;
            avail->ao_module = module;
            
            /* Put this item on the list in priority order (lowest
               priority first).  Should it go first? */
            for (item2 = opal_list_get_first(selectable);
                 item2 != opal_list_get_end(selectable);
                 item2 = opal_list_get_next(item2)) {
                avail2 = (avail_op_t*)item2;
                if(avail->ao_priority < avail2->ao_priority) {
                    opal_list_insert_pos(selectable,
                                         item2, (opal_list_item_t*)avail);
                    break;
                }
            }
            
            if (opal_list_get_end(selectable) == item2) {
                opal_list_append(selectable, (opal_list_item_t*)avail);
            }
        }
    }

    /* All done (even if the list is empty; that's ok) */
    return selectable;
}


/*
 * Check a single component
 */
static int check_one_component(ompi_op_t *op, 
                               const mca_base_component_t *component,
                               ompi_op_base_module_1_0_0_t **module)
{
    int err;
    int priority = -1;

    err = query(component, op, &priority, module);

    if (OMPI_SUCCESS == err) {
        priority = (priority < 100) ? priority : 100;
        opal_output_verbose(10, ompi_op_base_output, 
                            "op:base:op_select: component available: %s, priority: %d", 
                            component->mca_component_name, priority);

    } else {
        priority = -1;
        opal_output_verbose(10, ompi_op_base_output, 
                            "op:base:op_select: component not available: %s",
                            component->mca_component_name);
    }

    return priority;
}


/**************************************************************************
 * Query functions
 **************************************************************************/

/*
 * Take any version of a op module, query it, and return the right
 * module struct
 */
static int query(const mca_base_component_t *component, 
                 ompi_op_t *op, 
                 int *priority, ompi_op_base_module_1_0_0_t **module)
{
    *module = NULL;
    if (1 == component->mca_type_major_version &&
        0 == component->mca_type_minor_version &&
        0 == component->mca_type_release_version) {
        const ompi_op_base_component_1_0_0_t *op100 = 
            (ompi_op_base_component_1_0_0_t *) component;

        return query_1_0_0(op100, op, priority, module);
    } 

    /* Unknown op API version -- return error */

    return OMPI_ERROR;
}


static int query_1_0_0(const ompi_op_base_component_1_0_0_t *component,
                       ompi_op_t *op, int *priority,
                       ompi_op_base_module_1_0_0_t **module)
{
    ompi_op_base_module_1_0_0_t *ret;

    /* There's currently no need for conversion */

    ret = component->opc_op_query(op, priority);
    if (NULL != ret) {
        *module = ret;
        return OMPI_SUCCESS;
    }

    return OMPI_ERROR;
}