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
# Copyright (c) 2007      Cisco Systems, Inc.  All rights reserved.
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */


#include "opal_config.h"

#include "opal/constants.h"
#include "opal/util/output.h"
#include "opal/mca/mca.h"
#include "opal/mca/base/base.h"
#include "opal/mca/base/mca_base_param.h"
#include "opal/mca/carto/carto.h"
#include "opal/mca/carto/base/base.h"


/*
 * The following file was created by configure.  It contains extern
 * statements and the definition of an array of pointers to each
 * component's public mca_base_component_t struct.
 */
#include "opal/mca/carto/base/static-components.h"


/*
 * Globals
 */
int opal_carto_base_output = -1;
bool opal_carto_base_components_opened_valid = false;
opal_list_t opal_carto_base_components_opened;


/*
 * Function for finding and opening either all MCA components, or the one
 * that was specifically requested via a MCA parameter.
 */
int opal_carto_base_open(void)
{
    int value;

    /* Debugging / verbose output */

    mca_base_param_reg_int_name("carto", "base_verbose", 
                                "Verbosity level of the carto framework",
                                false, false,
                                0, &value);
    if (0 != value) {
        opal_carto_base_output = opal_output_open(NULL);
    } else {
        opal_carto_base_output = -1;
    }

    opal_carto_base_components_opened_valid = false;

    /* Open up all available components */

    if (OPAL_SUCCESS !=
        mca_base_components_open("carto", opal_carto_base_output,
                                 mca_carto_base_static_components,
                                 &opal_carto_base_components_opened, 
                                 true)) {
        return OPAL_ERROR;
    }
    opal_carto_base_components_opened_valid = true;

    /* All done */

    return OPAL_SUCCESS;
}