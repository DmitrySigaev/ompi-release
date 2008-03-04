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
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 *
 * These symbols are in a file by themselves to provide nice linker
 * semantics.  Since linkers generally pull in symbols by object
 * files, keeping these symbols as the only symbols in this file
 * prevents utility programs such as "ompi_info" from having to import
 * entire components just to query their version and parameters.
 */

#include "orte_config.h"
#include "orte/constants.h"

#include "opal/mca/base/mca_base_param.h"

#include "orte/util/proc_info.h"

#include "orte/mca/ess/ess.h"
#include "orte/mca/ess/hnp/ess_hnp.h"

extern orte_ess_base_module_t orte_ess_hnp_module;

/*
 * Instantiate the public struct with all of our public information
 * and pointers to our public functions in it
 */
orte_ess_base_component_t mca_ess_hnp_component = {
    {
        /* Indicate that we are a ess v1.0.0 component (which also
           implies a specific MCA version) */
        ORTE_ESS_BASE_VERSION_1_0_0,

        /* Component name and version */
        "hnp",
        ORTE_MAJOR_VERSION,
        ORTE_MINOR_VERSION,
        ORTE_RELEASE_VERSION,

        /* Component open and close functions */
        orte_ess_hnp_component_open,
        orte_ess_hnp_component_close
    },

    /* Next the MCA v1.0.0 component meta data */
    {
        /* The component is checkpoint ready */
        MCA_BASE_METADATA_PARAM_CHECKPOINT
    },

    /* Initialization / querying functions */
    orte_ess_hnp_component_init
};


int
orte_ess_hnp_component_open(void)
{
    return ORTE_SUCCESS;
}


orte_ess_base_module_t *
orte_ess_hnp_component_init(int *priority)
{

    /* we are the hnp module - we need to be selected
     * IFF we are designated as the hnp
     */
    if (orte_process_info.hnp) {
        *priority = 100;
        return &orte_ess_hnp_module;
    }
    
    /* else, we are not */
    *priority = -1;
    return NULL;
}


int
orte_ess_hnp_component_close(void)
{
    return ORTE_SUCCESS;
}
