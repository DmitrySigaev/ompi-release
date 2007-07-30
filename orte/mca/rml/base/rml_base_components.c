/*
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

#include "orte_config.h"

#include "opal/mca/mca.h"
#include "opal/util/output.h"
#include "opal/mca/base/mca_base_component_repository.h"
#include "orte/mca/rml/rml.h"
#include "orte/mca/rml/base/base.h"
#include "orte/dss/dss.h"
#include "orte/dss/dss_types.h"
#include "orte/mca/errmgr/errmgr.h"


/* The following file was created by configure.  It contains extern
 * statements and the definition of an array of pointers to each
 * component's public mca_base_component_t struct. */
#include "orte/mca/rml/base/static-components.h"


int               orte_rml_base_output = -1;
opal_list_t       orte_rml_base_subscriptions;
orte_rml_module_t orte_rml;
opal_list_t       orte_rml_base_components;
orte_rml_component_t *orte_rml_component = NULL;

static bool                  component_open_called = false;

int
orte_rml_base_open(void)
{
    int ret;
    orte_data_type_t tmp;

    /* Initialize globals */
    OBJ_CONSTRUCT(&orte_rml_base_components, opal_list_t);
    OBJ_CONSTRUCT(&orte_rml_base_subscriptions, opal_list_t);

    /* register the base system types with the DPS */
    tmp = ORTE_RML_TAG;
    if (ORTE_SUCCESS != (ret = orte_dss.register_type(orte_rml_base_pack_tag,
                                       orte_rml_base_unpack_tag,
                                       (orte_dss_copy_fn_t)orte_rml_base_copy_tag,
                                       (orte_dss_compare_fn_t)orte_rml_base_compare_tags,
                                       (orte_dss_size_fn_t)orte_rml_base_size_tag,
                                       (orte_dss_print_fn_t)orte_rml_base_print_tag,
                                       (orte_dss_release_fn_t)orte_rml_base_std_obj_release,
                                       ORTE_DSS_UNSTRUCTURED,
                                       "ORTE_RML_TAG", &tmp))) {
        ORTE_ERROR_LOG(ret);
        return ret;
    }

    /* 
     * Which RML Wrapper component to use, if any
     *  - NULL or "" = No wrapper
     *  - ow. select that specific wrapper component
     */
    mca_base_param_reg_string_name("rml", "wrapper",
                                   "Use a Wrapper component around the selected RML component",
                                   false, false,
                                   NULL, NULL);
    
    /* Open up all available components */
    ret = mca_base_components_open("rml",
                                   orte_rml_base_output,
                                   mca_rml_base_static_components, 
                                   &orte_rml_base_components,
                                   true);
    component_open_called = true;

    return ret;
}


int
orte_rml_base_select(void)
{
    opal_list_item_t *item;

    int selected_priority = -1;
    orte_rml_component_t *selected_component = NULL;
    orte_rml_module_t *selected_module = NULL;

    int wrapper_priority = -1;
    orte_rml_component_t *wrapper_component = NULL;
    orte_rml_module_t *wrapper_module = NULL;
    char *rml_wrapper = NULL;

    mca_base_param_reg_string_name("rml", "wrapper",
                                   "Use a Wrapper component around the selected RML component",
                                   false, false,
                                   NULL, &rml_wrapper);
    
    for (item = opal_list_get_first(&orte_rml_base_components);
         item != opal_list_get_end(&orte_rml_base_components) ;
         item = opal_list_get_next(item)) {
        mca_base_component_list_item_t *cli;
        orte_rml_component_t* component;
 
        cli = (mca_base_component_list_item_t *) item;
        component = (orte_rml_component_t *) cli->cli_component;

        opal_output_verbose(10, orte_rml_base_output, 
                            "orte_rml_base_select: initializing %s component %s",
                            component->rml_version.mca_type_name,
                            component->rml_version.mca_component_name);

        if (NULL == component->rml_init) {
            opal_output_verbose(10, orte_rml_base_output, 
                                "orte_rml_base_select: no init function; ignoring component");
        } else {
            int priority = 0;

            orte_rml_module_t* module = component->rml_init(&priority);
            if (NULL == module) {
                opal_output_verbose(10, orte_rml_base_output,
                                    "orte_rml_base_select: init returned failure");
                continue;
            }

            if(NULL != rml_wrapper &&
               /* If this is a wrapper component then save it for later */
               RML_SELECT_WRAPPER_PRIORITY >= priority) {
                if( 0 == strncmp(component->rml_version.mca_component_name, 
                                 rml_wrapper,
                                 strlen(rml_wrapper) ) ) {
                    wrapper_priority  = priority;
                    wrapper_component = component;
                    wrapper_module    = module;
                }
            } else if (priority > selected_priority) {
                /* Otherwise this is a normal module and subject to normal selection */
                if (NULL != selected_module && NULL != selected_module->finalize) {
                    selected_module->finalize();
                }

                selected_priority = priority;
                selected_component = component;
                selected_module = module;
            }
        }
    }

    /* 
     * Unload all components that were not selected
     */
    item = opal_list_get_first(&orte_rml_base_components);
    while (item != opal_list_get_end(&orte_rml_base_components)) {
        opal_list_item_t* next = opal_list_get_next(item);
        orte_rml_component_t* component;
        mca_base_component_list_item_t *cli;

        cli = (mca_base_component_list_item_t *) item;
        component = (orte_rml_component_t *) cli->cli_component;

        /* Keep it if it is the wrapper component */
        if (NULL != wrapper_component &&
            component == wrapper_component) {
            item = next;
            continue;
        }
        /* Not the selected component */
        if (component != selected_component) {
            opal_output_verbose(10, orte_rml_base_output,
                                "orte_rml_base_select: module %s unloaded",
                                component->rml_version.mca_component_name);

            mca_base_component_repository_release((mca_base_component_t *) component);
            opal_list_remove_item(&orte_rml_base_components, item);
            OBJ_RELEASE(item);
        }
        item = next;
    }

    /* setup reference to selected module */
    if (NULL != selected_module) {
        orte_rml = *selected_module;
        orte_rml_component = selected_component;
    }

    /* If a wrapper component was requested then 
     * Make sure it can switch out the selected module
     */
    if( NULL != wrapper_component) {
        wrapper_component->rml_init(NULL);
    }

    if( NULL != rml_wrapper ) {
        free(rml_wrapper);
    }

    if (NULL == selected_component) return ORTE_ERROR;
    
    return ORTE_SUCCESS;
}


int
orte_rml_base_close(void)
{
    /* shutdown any remaining opened components */
    if (component_open_called) {
        mca_base_components_close(orte_rml_base_output, 
                                  &orte_rml_base_components, NULL);
    }

    OBJ_DESTRUCT(&orte_rml_base_components);
    OBJ_DESTRUCT(&orte_rml_base_subscriptions);

    return ORTE_SUCCESS;
}