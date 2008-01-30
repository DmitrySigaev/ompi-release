/*
 * Copyright (c) 2004-2007 The Trustees of the University of Tennessee.
 *                         All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "ompi_config.h"

#include "opal/mca/mca.h"
#include "opal/mca/base/mca_base_param.h"
#include "vprotocol_pessimist.h"

static inline int mca_param_register_int( const char* param_name, int default_value);
static inline char *mca_param_register_string(const char* param_name, char *default_value);

static int mca_vprotocol_pessimist_component_open(void);
static int mca_vprotocol_pessimist_component_close(void);

static mca_vprotocol_base_module_t *mca_vprotocol_pessimist_component_init( int* priority, bool, bool);
static int mca_vprotocol_pessimist_component_finalize(void);

static int _priority;
static int _free_list_num;
static int _free_list_max;
static int _free_list_inc;
static int _sender_based_size;
static int _event_buffer_size;
static char *_mmap_file_name;

mca_vprotocol_base_component_1_0_0_t mca_vprotocol_pessimist_component = 
{
    /* First, the mca_base_component_t struct containing meta
     * information about the component itself */
    {
      /* Indicate that we are a vprotocol v1.0.0 component (which also implies
         a specific MCA version) */
      MCA_VPROTOCOL_BASE_VERSION_1_0_0,
      "pessimist", /* MCA component name */
      1,  /* MCA component major version */
      0,  /* MCA component minor version */
      0,  /* MCA component release version */
      mca_vprotocol_pessimist_component_open,  /* component open */
      mca_vprotocol_pessimist_component_close  /* component close */
    },

    /* Next the MCA v1.0.0 component meta data */
    {
      /* Whether the component is checkpointable or not */
      false
    },

    mca_vprotocol_pessimist_component_init,  /* component init */
    mca_vprotocol_pessimist_component_finalize   /* component finalize */
};

/** MCA level functions
  */
static int mca_vprotocol_pessimist_component_open(void)
{
    _priority = mca_param_register_int("priority", 30);
    _free_list_num = mca_param_register_int("free_list_num", 16);
    _free_list_max = mca_param_register_int("free_list_max", -1);
    _free_list_inc = mca_param_register_int("free_list_inc", 64);
    _sender_based_size = mca_param_register_int("sender_based_chunk", 256 * 1024 * 1024);
    _event_buffer_size = mca_param_register_int("event_buffer_size", 1024);
    _mmap_file_name = mca_param_register_string("sender_based_file", "vprotocol_pessimist-senderbased");
    V_OUTPUT_VERBOSE(500, "vprotocol_pessimist: component_open: read priority %d", _priority);
  return OMPI_SUCCESS;
}

static int mca_vprotocol_pessimist_component_close(void)
{
    V_OUTPUT_VERBOSE(500, "vprotocol_pessimist: component_close");
    return OMPI_SUCCESS;
}

/** VPROTOCOL level functions (same as PML one)
  */
static mca_vprotocol_base_module_t *mca_vprotocol_pessimist_component_init( int* priority,
                                                                          bool enable_progress_threads,
                                                                          bool enable_mpi_threads)
{  
    V_OUTPUT_VERBOSE(500, "vprotocol_pessimist: component_init");
    *priority = _priority;

    /* sanity check */
    if(enable_mpi_threads)
    {
        opal_output(0, "vprotocol_pessimist: component_init: threads are enabled, and not supported by vprotocol pessimist fault tolerant layer, will not load");
        return NULL;
    }

    mca_vprotocol_pessimist.clock = 1;
    mca_vprotocol_pessimist.replay = false;
    OBJ_CONSTRUCT(&mca_vprotocol_pessimist.replay_events, opal_list_t);
    OBJ_CONSTRUCT(&mca_vprotocol_pessimist.pending_events, opal_list_t);
    OBJ_CONSTRUCT(&mca_vprotocol_pessimist.events_pool, ompi_free_list_t);
    ompi_free_list_init_new(&mca_vprotocol_pessimist.events_pool,
                        sizeof(mca_vprotocol_pessimist_event_t),
                        CACHE_LINE_SIZE,
                        OBJ_CLASS(mca_vprotocol_pessimist_event_t),
                        0,CACHE_LINE_SIZE,
                        _free_list_num,
                        _free_list_max,
                        _free_list_inc,
                        NULL);
    mca_vprotocol_pessimist.event_buffer_max_length = 
                _event_buffer_size / sizeof(vprotocol_pessimist_mem_event_t);
    mca_vprotocol_pessimist.event_buffer_length = 0;
    mca_vprotocol_pessimist.event_buffer = 
                (vprotocol_pessimist_mem_event_t *) malloc(_event_buffer_size);

    return &mca_vprotocol_pessimist.super;
}
                                                                          
static int mca_vprotocol_pessimist_component_finalize(void)
{
    V_OUTPUT_VERBOSE(500, "vprotocol_pessimist_finalize");
    free(mca_vprotocol_pessimist.event_buffer);
    OBJ_DESTRUCT(&mca_vprotocol_pessimist.replay_events);
    OBJ_DESTRUCT(&mca_vprotocol_pessimist.pending_events);
    OBJ_DESTRUCT(&mca_vprotocol_pessimist.events_pool);
    return OMPI_SUCCESS;
}

int mca_vprotocol_pessimist_enable(bool enable) {
    if(enable) {
        int ret; 
        if((ret = vprotocol_pessimist_sender_based_init(_mmap_file_name, 
                                                 _sender_based_size)) < OPAL_SUCCESS)
            return ret;
    }
    else {
        vprotocol_pessimist_sender_based_finalize();
    }
    return OMPI_SUCCESS;
}


static inline int mca_param_register_int( const char* param_name,
                                                  int default_value )
{
    int id = mca_base_param_register_int("vprotocol", "pessimist", param_name, NULL, default_value);
    int param_value = default_value;
    mca_base_param_lookup_int(id, &param_value);
    return param_value;
}

static inline char *mca_param_register_string( const char* param_name,
                                                  char *default_value )
{
    int id = mca_base_param_register_string("vprotocol", "pessimist", param_name, NULL, default_value);
    char *param_value = default_value;
    mca_base_param_lookup_string(id, &param_value);
    return param_value;
}