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
 * Copyright (c) 2006      Cisco Systems, Inc.  All rights reserved.
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
#include "opal/util/output.h"
#include "opal/util/argv.h"

#include "orte/util/proc_info.h"
#include "orte/mca/errmgr/errmgr.h"

#include "orte/mca/plm/plm.h"
#include "orte/mca/plm/base/base.h"
#include "orte/mca/plm/base/plm_private.h"
#include "plm_md.h"


/*
 * Public string showing the plm ompi_tm component version number
 */
const char *mca_plm_md_component_version_string =
  "Open MPI tm plm MCA component version " ORTE_VERSION;



/*
 * Local function
 */
static int plm_md_open(void);
static int plm_md_close(void);
static orte_plm_base_module_t *plm_md_init(int *priority);


/*
 * Instantiate the public struct with all of our public information
 * and pointers to our public functions in it
 */

orte_plm_base_component_t mca_plm_md_component = {
    {
        /* Indicate that we are a plm v1.0.0 component (which also
           implies a specific MCA version) */
        ORTE_PLM_BASE_VERSION_1_0_0,

        /* Component name and version */
        "md",
        ORTE_MAJOR_VERSION,
        ORTE_MINOR_VERSION,
        ORTE_RELEASE_VERSION,

        /* Component open and close functions */
        plm_md_open,
        plm_md_close,
    },

    /* Next the MCA v1.0.0 component meta data */
    {
        /* The component is checkpoint ready */
        MCA_BASE_METADATA_PARAM_CHECKPOINT
    },

    /* Initialization / querying functions */
    plm_md_init
};


static int plm_md_open(void)
{
    return ORTE_SUCCESS;
}


static int plm_md_close(void)
{
    return ORTE_SUCCESS;
}


static orte_plm_base_module_t *plm_md_init(int *priority)
{
    /* Are we running under a TM job? */

    if (NULL != getenv("PBS_ENVIRONMENT") &&
        NULL != getenv("PBS_JOBID")) {

        /* defer to the tm and tmd modules */
        *priority = 10;
        return &orte_plm_md_module;
    }

    /* Sadly, no */
    return NULL;
}