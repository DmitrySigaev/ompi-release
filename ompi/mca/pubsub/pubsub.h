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
 */
/**
 * @file
 *
 * Dynamic Process Management Interface
 *
 */

#ifndef OMPI_MCA_PUBSUB_H
#define OMPI_MCA_PUBSUB_H

#include "ompi_config.h"

#include "opal/mca/mca.h"
#include "opal/mca/base/base.h"

#include "opal/class/opal_object.h"

#include "ompi/info/info.h"
#include "ompi/communicator/communicator.h"

BEGIN_C_DECLS

/*
 * Initialize a module
 */
typedef int (*ompi_pubsub_base_module_init_fn_t)(void);

/*
 * Publish a data item
 */
typedef int (*ompi_pubsub_base_module_publish_fn_t)(char *service, ompi_info_t *info, char *port);

/*
 * Unpublish a data item
 */
typedef int (*ompi_pubsub_base_module_unpublish_fn_t)(char *service, ompi_info_t *info);

/*
 * Lookup a data item
 */
typedef char* (*ompi_pubsub_base_module_lookup_fn_t)(char *service, ompi_info_t *info);

/*
 * Finalize a module
 */
typedef int (*ompi_pubsub_base_module_finalize_fn_t)(void);

/**
* Structure for PUBSUB v1.0.0 modules
 */
struct ompi_pubsub_base_module_1_0_0_t {
    /** Initialization Function */
    ompi_pubsub_base_module_init_fn_t       init;
    /* Publish */
    ompi_pubsub_base_module_publish_fn_t    publish;
    /* Unpublish */
    ompi_pubsub_base_module_unpublish_fn_t  unpublish;
    /* Lookup */
    ompi_pubsub_base_module_lookup_fn_t     lookup;
    /* finalize */
    ompi_pubsub_base_module_finalize_fn_t   finalize;
};
typedef struct ompi_pubsub_base_module_1_0_0_t ompi_pubsub_base_module_1_0_0_t;
typedef struct ompi_pubsub_base_module_1_0_0_t ompi_pubsub_base_module_t;

OMPI_DECLSPEC extern ompi_pubsub_base_module_t ompi_pubsub;


typedef struct ompi_pubsub_base_module_1_0_0_t*
(*ompi_pubsub_base_component_init_fn_t)(int *priority);


/**
 * Structure for PUBSUB v1.0.0 components.
 */
struct ompi_pubsub_base_component_1_0_0_t {
    /** MCA base component */
    mca_base_component_t pubsub_version;
    /** MCA base data */
    mca_base_component_data_1_0_0_t pubsub_data;
    /* component selection */
    ompi_pubsub_base_component_init_fn_t pubsub_init;
};
typedef struct ompi_pubsub_base_component_1_0_0_t ompi_pubsub_base_component_1_0_0_t;
typedef struct ompi_pubsub_base_component_1_0_0_t ompi_pubsub_base_component_t;

/**
 * Macro for use in components that are of type CRCP v1.0.0
 */
#define OMPI_PUBSUB_BASE_VERSION_1_0_0 \
    /* PUBSUB v1.0 is chained to MCA v1.0 */ \
    MCA_BASE_VERSION_1_0_0, \
    /* PUBSUB v1.0 */ \
    "pubsub", 1, 0, 0


END_C_DECLS

#endif /* OMPI_MCA_PUBSUB_H */