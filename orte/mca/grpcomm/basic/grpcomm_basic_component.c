/* -*- C -*-
*
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
* $COPYRIGHT$
*
* Additional copyrights may follow
*
* $HEADER$
*/
/** @file:
*
* The Open MPI Name Server
*
* The Open MPI Name Server provides unique name ranges for processes
* within the universe. Each universe will have one name server
* running within the seed daemon.  This is done to prevent the
* inadvertent duplication of names.
*/

/*
 * includes
 */
#include "orte_config.h"

#include "orte/orte_constants.h"
#include "orte/orte_types.h"

#include "opal/threads/mutex.h"
#include "opal/class/opal_list.h"
#include "opal/util/output.h"

#include "opal/mca/mca.h"
#include "opal/mca/base/mca_base_param.h"

#include "orte/util/proc_info.h"
#include "orte/mca/errmgr/errmgr.h"
#include "orte/mca/rml/rml.h"

#include "grpcomm_basic.h"

#define XCAST_LINEAR_XOVER_DEFAULT     10
#define XCAST_BINOMIAL_XOVER_DEFAULT   INT_MAX


/*
 * Struct of function pointers that need to be initialized
 */
orte_grpcomm_base_component_t mca_grpcomm_basic_component = {
    {
        ORTE_GRPCOMM_BASE_VERSION_2_0_0,
        
        "basic", /* MCA module name */
        ORTE_MAJOR_VERSION,  /* MCA module major version */
        ORTE_MINOR_VERSION,  /* MCA module minor version */
        ORTE_RELEASE_VERSION,  /* MCA module release version */
        orte_grpcomm_basic_open,  /* module open */
        orte_grpcomm_basic_close /* module close */
    },
    {
        /* The component is checkpoint ready */
        MCA_BASE_METADATA_PARAM_CHECKPOINT
    },
    orte_grpcomm_basic_init,    /* component init */
    orte_grpcomm_basic_finalize /* component shutdown */
};

/*
 * instantiate globals needed within basic component
 */
orte_grpcomm_basic_globals_t orte_grpcomm_basic;

/* Open the component */
int orte_grpcomm_basic_open(void)
{
    int value;
    char *mode;
    mca_base_component_t *c = &mca_grpcomm_basic_component.grpcomm_version;
    
    /* initialize globals */
    OBJ_CONSTRUCT(&orte_grpcomm_basic.mutex, opal_mutex_t);
    OBJ_CONSTRUCT(&orte_grpcomm_basic.cond, opal_condition_t);
    orte_grpcomm_basic.num_active = 0;
    
    /* register parameters */
    mca_base_param_reg_int(c, "verbose",
                           "Verbosity level for the grpcomm basic component",
                           false, false, 0, &value);
    if (value != 0) {
        orte_grpcomm_basic.output = opal_output_open(NULL);
    } else {
        orte_grpcomm_basic.output = -1;
    }
    
    mca_base_param_reg_int(c, "xcast_linear_xover",
                           "Number of daemons where use of linear xcast mode is to begin",
                           false, false, XCAST_LINEAR_XOVER_DEFAULT, &orte_grpcomm_basic.xcast_linear_xover);
    
    mca_base_param_reg_int(c, "xcast_binomial_xover",
                           "Number of daemons where use of binomial xcast mode is to begin",
                           false, false, XCAST_BINOMIAL_XOVER_DEFAULT, &orte_grpcomm_basic.xcast_binomial_xover);
    
    mca_base_param_reg_string(c, "xcast_mode",
                              "Select xcast mode (\"linear\" | \"binomial\" | \"direct\")",
                              false, false, "none", &mode);
    if (0 == strcmp(mode, "binomial")) {
        orte_grpcomm_basic.xcast_binomial_xover = 0;
        orte_grpcomm_basic.xcast_linear_xover = 0;
    } else if (0 == strcmp(mode, "linear")) {
        orte_grpcomm_basic.xcast_linear_xover = 0;
        orte_grpcomm_basic.xcast_binomial_xover = INT_MAX;
    } else if (0 == strcmp(mode, "direct")) {
        orte_grpcomm_basic.xcast_binomial_xover = INT_MAX;
        orte_grpcomm_basic.xcast_linear_xover = INT_MAX;
    } else if (0 != strcmp(mode, "none")) {
        opal_output(0, "grpcomm_basic_xcast_mode: unknown option %s - using defaults", mode);
    }
    
    return ORTE_SUCCESS;
}

/* Close the component */
int orte_grpcomm_basic_close(void)
{
    OBJ_DESTRUCT(&orte_grpcomm_basic.mutex);
    OBJ_DESTRUCT(&orte_grpcomm_basic.cond);
    
    return ORTE_SUCCESS;
}

orte_grpcomm_base_module_t* orte_grpcomm_basic_init(int *priority)
{
    /* we are the default, so set a low priority so we can be overridden */
    *priority = 1;
    
    return &orte_grpcomm_basic_module;
}

/*
 * finalize routine
 */
int orte_grpcomm_basic_finalize(void)
{
    return ORTE_SUCCESS;
}