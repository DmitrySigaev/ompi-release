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

#include "opal/util/output.h"
#include "opal/mca/base/mca_base_param.h"

#include "orte/util/name_fns.h"
#include "orte/runtime/orte_globals.h"

#include "orte/mca/plm/plm.h"
#include "orte/mca/plm/base/plm_private.h"
#include "plm_slurmd.h"


/*
 * Public string showing the plm ompi_slurmd component version number
 */
const char *mca_plm_slurmd_component_version_string =
  "Open MPI slurmd plm MCA component version " ORTE_VERSION;


/*
 * Local functions
 */
static int plm_slurmd_open(void);
static int plm_slurmd_close(void);
static orte_plm_base_module_t *plm_slurmd_init(int *priority);


/*
 * Instantiate the public struct with all of our public information
 * and pointers to our public functions in it
 */

orte_plm_slurmd_component_t mca_plm_slurmd_component = {

    {
        /* First, the mca_component_t struct containing meta
           information about the component itself */

        {
            /* Indicate that we are a plm v1.0.0 component (which also
               implies a specific MCA version) */

            ORTE_PLM_BASE_VERSION_1_0_0,
            
            /* Component name and version */
            
            "slurmd",
            ORTE_MAJOR_VERSION,
            ORTE_MINOR_VERSION,
            ORTE_RELEASE_VERSION,
            
            /* Component open and close functions */
            
            plm_slurmd_open,
            plm_slurmd_close
        },
        
        /* Next the MCA v1.0.0 component meta data */
        
        {
            /* The component is checkpoint ready */
            MCA_BASE_METADATA_PARAM_CHECKPOINT
        },
        
        /* Initialization / querying functions */
        
        plm_slurmd_init
    }

    /* Other orte_plm_slurmd_component_t items -- left uninitialized
       here; will be initialized in plm_slurmd_open() */
};


static int plm_slurmd_open(void)
{
    mca_base_component_t *comp = &mca_plm_slurmd_component.super.plm_version;

    mca_base_param_reg_int(comp, "priority", "Default selection priority",
                           false, false, 50,  /* keep this below indirect slurm */
                           &mca_plm_slurmd_component.priority);

    mca_base_param_reg_string(comp, "orted",
                              "Command to use to start proxy orted",
                              false, false, "orted",
                              &mca_plm_slurmd_component.orted);

    mca_base_param_reg_string(comp, "args",
                              "Custom arguments to srun",
                              false, false, NULL,
                              &mca_plm_slurmd_component.custom_args);

    return ORTE_SUCCESS;
}


static orte_plm_base_module_t *plm_slurmd_init(int *priority)
{
    /* Are we running under a SLURM job? */

    if (NULL != getenv("SLURM_JOBID")) {
        *priority = mca_plm_slurmd_component.priority;

        OPAL_OUTPUT_VERBOSE((1, orte_plm_globals.output,
                             "%s plm:slurmd: available for selection", 
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));

        return &orte_plm_slurmd_module;
    }

    /* Sadly, no */

    return NULL;
}


static int plm_slurmd_close(void)
{
    if (NULL != mca_plm_slurmd_component.orted) {
        free(mca_plm_slurmd_component.orted);
    }

    if (NULL != mca_plm_slurmd_component.custom_args) {
        free(mca_plm_slurmd_component.custom_args);
    }

    return ORTE_SUCCESS;
}