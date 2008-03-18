/*
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2008 High Performance Computing Center Stuttgart, 
 *                         University of Stuttgart.  All rights reserved.
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 *
 */

#include "orte_config.h"

#include "opal/mca/base/mca_base_param.h"
#include "opal/util/output.h"
#include "opal/util/argv.h"
#include "orte/constants.h"

#include "orte/util/proc_info.h"
#include "orte/mca/errmgr/errmgr.h"

#include "orte/mca/plm/plm.h"
#include "orte/mca/plm/base/base.h"
#include "orte/mca/plm/base/plm_private.h"
#include "plm_ccp.h"

/* Import the Windows CCP API. */
#import "ccpapi.tlb" named_guids no_namespace raw_interfaces_only   \
    rename("SetEnvironmentVariable","SetEnvVar")                    \
    rename("GetJob", "GetSingleJob")                                \
    rename("AddJob", "AddSingleJob")


/*
 * Public string showing the plm ompi_ccp component version number
 */
const char *mca_plm_ccp_component_version_string =
  "Open MPI ccp plm MCA component version " ORTE_VERSION;



/*
 * Local function
 */
static int plm_ccp_open(void);
static int plm_ccp_close(void);
static orte_plm_base_module_t *plm_ccp_init(int *priority);


/*
 * Instantiate the public struct with all of our public information
 * and pointers to our public functions in it
 */

orte_plm_ccp_component_t mca_plm_ccp_component = {
    {
        /* First, the mca_component_t struct containing meta information
           about the component itself */

        {
            /* Indicate that we are a plm v1.3.0 component (which also
               implies a specific MCA version) */
            ORTE_PLM_BASE_VERSION_1_0_0,

            /* Component name and version */
            "ccp",
            ORTE_MAJOR_VERSION,
            ORTE_MINOR_VERSION,
            ORTE_RELEASE_VERSION,

            /* Component open and close functions */
            plm_ccp_open,
            plm_ccp_close,
        },

        /* Next the MCA v1.0.0 component meta data */
        {
            /* The component is checkpoint ready */
            MCA_BASE_METADATA_PARAM_CHECKPOINT
        },

        /* Initialization / querying functions */
        plm_ccp_init
    }
};


static int plm_ccp_open(void)
{
    int tmp, value;
    mca_base_component_t *comp = &mca_plm_ccp_component.super.plm_version;

    mca_base_param_reg_int(comp, "debug", "Enable debugging of the CCP plm",
                           false, false, 0, &mca_plm_ccp_component.debug);
    mca_base_param_reg_int(comp, "verbose", "Enable verbose output of the ccp plm",
                           false, false, 0, &mca_plm_ccp_component.verbose);

    mca_base_param_reg_int(comp, "priority", "Default selection priority",
                           false, false, 75, &mca_plm_ccp_component.priority);

    mca_base_param_reg_string(comp, "orted",
                              "Command to use to start proxy orted",
                              false, false, "orted",
                              &mca_plm_ccp_component.orted);
    mca_base_param_reg_int(comp, "want_path_check",
                           "Whether the launching process should check for the plm_ccp_orted executable in the PATH before launching (the CCP API does not give an indication of failure; this is a somewhat-lame workaround; non-zero values enable this check)",
                           false, false, (int) true, &tmp);
    mca_plm_ccp_component.want_path_check = OPAL_INT_TO_BOOL(tmp);
    
    tmp = mca_base_param_reg_int_name("orte", "timing",
                                        "Request that critical timing loops be measured",
                                        false, false, 0, &value);
    if (value != 0) {
        mca_plm_ccp_component.timing = true;
    } else {
        mca_plm_ccp_component.timing = false;
    }
    
    mca_plm_ccp_component.checked_paths = NULL;

    return ORTE_SUCCESS;
}


static int plm_ccp_close(void)
{
    return ORTE_SUCCESS;
}


static orte_plm_base_module_t *plm_ccp_init(int *priority)
{
    int rc;
    ICluster* pCluster = NULL;
    HRESULT hr = S_OK;

    /* CCP is not thread safe. Use the apartment model. */
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    /* Try to create the Cluster object. */
    hr = CoCreateInstance( __uuidof(Cluster),
                           NULL,
                           CLSCTX_INPROC_SERVER,
                           __uuidof(ICluster),
                           reinterpret_cast<void **> (&pCluster) );
    if (FAILED(hr)) {
        /* We are not Windows clusters, don't select us.*/
        return NULL;
    }

    /* if we are NOT an HNP, then don't select us */
    if (!orte_process_info.hnp) {
        return NULL;
    }

    /* We are Windows clusters and this is HNP. */
    pCluster->Release();
    *priority = mca_plm_ccp_component.priority;
    return &orte_plm_ccp_module;
}