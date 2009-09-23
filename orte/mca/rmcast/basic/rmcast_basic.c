/*
 * Copyright (c) 2009      Cisco Systems, Inc.  All rights reserved. 
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */
#include "orte_config.h"
#include "orte/constants.h"
#include "opal/types.h"

#ifdef HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif
#ifdef HAVE_ARPA_INET_H
#include <arpa/inet.h>
#endif
#include <errno.h>

#include "opal/class/opal_list.h"
#include "opal/opal_socket_errno.h"
#include "opal/util/output.h"
#include "opal/util/argv.h"
#include "opal/util/if.h"
#include "opal/util/net.h"
#include "opal/dss/dss.h"

#include "orte/runtime/orte_globals.h"
#include "orte/runtime/orte_wait.h"
#include "orte/mca/errmgr/errmgr.h"
#include "orte/util/name_fns.h"

#include "orte/mca/rmcast/base/base.h"
#include "rmcast_basic.h"

/* LOCAL DATA */
static opal_mutex_t lock;
static opal_list_t recvs;
static opal_list_t channels;
static unsigned int next_channel;

/* LOCAL FUNCTIONS */
#define CLOSE_THE_SOCKET(socket)    \
    do {                            \
        shutdown(socket, 2);        \
        close(socket);              \
    } while(0)

static void recv_handler(int sd, short flags, void* user);

static int setup_socket(int *sd, int channel, bool bindsocket);

static void xmit_data(int sd, short flags, void* send_req);

/*
 * Data structure for tracking assigned channels
 */
typedef struct {
    opal_list_item_t item;
    char *name;
    opal_event_t recv_ev;
    uint32_t channel;
    int xmit;
    int recv;
    struct sockaddr_in addr;
    opal_event_t send_ev;
    opal_mutex_t send_lock;
    bool sends_in_progress;
    opal_list_t pending_sends;
} rmcast_basic_channel_t;

static void channel_construct(rmcast_basic_channel_t *ptr)
{
    ptr->name = NULL;
    ptr->channel = 0;
    ptr->xmit = -1;
    ptr->recv = -1;
    memset(&ptr->addr, 0, sizeof(ptr->addr));
    OBJ_CONSTRUCT(&ptr->send_lock, opal_mutex_t);
    ptr->sends_in_progress = false;
    OBJ_CONSTRUCT(&ptr->pending_sends, opal_list_t);
    OPAL_THREAD_LOCK(&lock);
    opal_list_append(&channels, &ptr->item);
    OPAL_THREAD_UNLOCK(&lock);    
}
static void channel_destruct(rmcast_basic_channel_t *ptr)
{
    /* cleanup the recv side */
    opal_event_del(&ptr->recv_ev);
    if (0 < ptr->recv) {
        CLOSE_THE_SOCKET(ptr->recv);
    }
    /* attempt to xmit any pending sends */
    /* cleanup the xmit side */
    if (0 < ptr->xmit) {
        CLOSE_THE_SOCKET(ptr->xmit);
    }
    OBJ_DESTRUCT(&ptr->send_lock);
    /* release the channel name */
    if (NULL != ptr->name) {
        free(ptr->name);
    }
}
OBJ_CLASS_INSTANCE(rmcast_basic_channel_t,
                   opal_list_item_t,
                   channel_construct,
                   channel_destruct);

/*
 * Data structure for tracking registered non-blocking recvs
 */
typedef struct {
    opal_list_item_t item;
    bool recvd;
    opal_buffer_t data;
    uint32_t channel;
    orte_rmcast_callback_fn_t cbfunc;
    void *cbdata;
} rmcast_basic_recv_t;

static void recv_construct(rmcast_basic_recv_t *ptr)
{
    ptr->recvd = false;
    OBJ_CONSTRUCT(&ptr->data, opal_buffer_t);
    ptr->cbfunc = NULL;
    ptr->cbdata = NULL;
    OPAL_THREAD_LOCK(&lock);
    opal_list_append(&recvs, &ptr->item);
    OPAL_THREAD_UNLOCK(&lock);
}
static void recv_destruct(rmcast_basic_recv_t *ptr)
{
    OBJ_DESTRUCT(&ptr->data);
}
OBJ_CLASS_INSTANCE(rmcast_basic_recv_t,
                   opal_list_item_t,
                   recv_construct,
                   recv_destruct);

/*
 * Data structure for tracking pending sends
 */
typedef struct {
    opal_list_item_t item;
    bool send_complete;
    opal_buffer_t *data;
    orte_rmcast_callback_fn_t cbfunc;
    void *cbdata;
} rmcast_basic_send_t;

static void send_construct(rmcast_basic_send_t *ptr)
{
    ptr->send_complete = false;
    ptr->data = NULL;
    ptr->cbfunc = NULL;
    ptr->cbdata = NULL;
}
OBJ_CLASS_INSTANCE(rmcast_basic_send_t,
                   opal_list_item_t,
                   send_construct,
                   NULL);


/* API FUNCTIONS */
static int init(void);

static void finalize(void);

static int basic_send(unsigned int channel, opal_buffer_t *buf);

static int basic_send_nb(unsigned int channel, opal_buffer_t *buf,
                         orte_rmcast_callback_fn_t cbfunc, void *cbdata);

static int basic_recv(unsigned int channel, opal_buffer_t *buf);

static int basic_recv_nb(unsigned int channel, orte_rmcast_callback_fn_t cbfunc, void *cbdata);

static unsigned int get_channel(char *name, uint8_t direction);

/* The API's in this module are solely used to support LOCAL
 * procs - i.e., procs that are co-located to the HNP. Remote
 * procs interact with the HNP's IOF via the HNP's receive function,
 * which operates independently and is in the rmcast_basic_receive.c file
 */

orte_rmcast_module_t orte_rmcast_basic_module = {
    init,
    finalize,
    basic_send,
    basic_send_nb,
    basic_recv,
    basic_recv_nb,
    get_channel
};

static int init(void)
{
    int xmitsd, recvsd;
    rmcast_basic_channel_t *chan;
    int channel;
    char *name;

    OPAL_OUTPUT_VERBOSE((2, orte_rmcast_base.rmcast_output, "%s rmcast:basic: init called",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
    
    /* setup the globals */
    OBJ_CONSTRUCT(&lock, opal_mutex_t);
    OBJ_CONSTRUCT(&recvs, opal_list_t);
    OBJ_CONSTRUCT(&channels, opal_list_t);

    /* define the starting point for new channels */
    next_channel = ORTE_RMCAST_DYNAMIC_CHANNELS;
    
    if (ORTE_PROC_IS_HNP || ORTE_PROC_IS_DAEMON) {
        channel = ORTE_RMCAST_SYS_ADDR;
        name = "system";
    } else if (ORTE_PROC_IS_APP) {
        channel = ORTE_RMCAST_APP_PUBLIC_ADDR;
        name = "app-public";
    }
    
    /* create a xmit socket */
    setup_socket(&xmitsd, channel, false);
    
    /* create a recv socket */
    setup_socket(&recvsd, channel, true);
    
    
    /* this channel is available for use, so add it to our list */
    chan = OBJ_NEW(rmcast_basic_channel_t);
    chan->name = strdup(name);
    chan->channel = channel;
    chan->xmit = xmitsd;
    chan->recv = recvsd;
    chan->addr.sin_family = AF_INET;
    chan->addr.sin_addr.s_addr = htonl(orte_rmcast_base.base_ip_addr + channel);
    chan->addr.sin_port = htons(orte_rmcast_base.ports[channel-1]);
    
    /* setup an event to catch messages */
    opal_event_set(&chan->recv_ev, chan->recv, OPAL_EV_READ, recv_handler, chan);
    opal_event_add(&chan->recv_ev, 0);
    
    /* setup the event to xmit messages, but don't activate it */
    opal_event_set(&chan->send_ev, chan->xmit, OPAL_EV_WRITE, xmit_data, chan);
    
    return ORTE_SUCCESS;
}

static void finalize(void)
{
    opal_list_item_t *item;
    
    OPAL_OUTPUT_VERBOSE((2, orte_rmcast_base.rmcast_output, "%s rmcast:basic: finalize called",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
    
    /* deconstruct the globals */
    OPAL_THREAD_LOCK(&lock);
    while (NULL != (item = opal_list_remove_first(&recvs))) {
        OBJ_RELEASE(item);
    }
    OBJ_DESTRUCT(&recvs);
    while (NULL != (item = opal_list_remove_first(&channels))) {
        OBJ_RELEASE(item);
    }
    OBJ_DESTRUCT(&channels);
    OPAL_THREAD_UNLOCK(&lock);

    OBJ_DESTRUCT(&lock);
    
    return;
}

/* internal blocking send support */
static void internal_snd_cb(int channel, opal_buffer_t *buf, void *cbdata)
{
    rmcast_basic_send_t *snd = (rmcast_basic_send_t*)cbdata;
    
    snd->send_complete = true;
}

static int basic_send(unsigned int channel, opal_buffer_t *buf)
{
    uint32_t chan;
    opal_list_item_t *item;
    rmcast_basic_channel_t *chptr, *ch;
    rmcast_basic_send_t *snd;
    
    chan = orte_rmcast_base.base_ip_addr + channel;
    OPAL_OUTPUT_VERBOSE((2, orte_rmcast_base.rmcast_output,
                         "%s rmcast:basic: send called on multicast channel %03d.%03d.%03d.%03d %0x",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), OPAL_IF_FORMAT_ADDR(chan), chan));
    
    /* do we already have this channel open? */
    ch = NULL;
    for (item = opal_list_get_first(&channels);
         item != opal_list_get_end(&channels);
         item = opal_list_get_next(item)) {
        chptr = (rmcast_basic_channel_t*)item;
        if (channel == chptr->channel) {
            ch = chptr;
            break;
        }
    }
    if (NULL == ch) {
        /* didn't find it - open the channel */
        return ORTE_ERR_NOT_IMPLEMENTED;
    }
    
    /* queue it to be sent - preserves order! */
    snd = OBJ_NEW(rmcast_basic_send_t);
    snd->data = buf;
    snd->cbfunc = internal_snd_cb;
    snd->cbdata = snd;
    
    /* add it to this channel's pending sends */
    OPAL_THREAD_LOCK(&ch->send_lock);
    opal_list_append(&ch->pending_sends, &snd->item);
    
    /* do we need to start the send event? */
    if (!ch->sends_in_progress) {
        opal_event_add(&ch->send_ev, 0);
        ch->sends_in_progress = true;
    }
    OPAL_THREAD_UNLOCK(&ch->send_lock);

    /* now wait for the send to complete */
    ORTE_PROGRESSED_WAIT(snd->send_complete, 0, 1);
    
    return ORTE_SUCCESS;
}

static int basic_send_nb(unsigned int channel, opal_buffer_t *buf,
                         orte_rmcast_callback_fn_t cbfunc, void *cbdata)
{
    uint32_t chan;
    opal_list_item_t *item;
    rmcast_basic_channel_t *chptr, *ch;
    rmcast_basic_send_t *snd;
    
    chan = orte_rmcast_base.base_ip_addr + channel;
    OPAL_OUTPUT_VERBOSE((2, orte_rmcast_base.rmcast_output,
                         "%s rmcast:basic: send called on multicast channel %03d.%03d.%03d.%03d %0x",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), OPAL_IF_FORMAT_ADDR(chan), chan));
    
    /* do we already have this channel open? */
    ch = NULL;
    for (item = opal_list_get_first(&channels);
         item != opal_list_get_end(&channels);
         item = opal_list_get_next(item)) {
        chptr = (rmcast_basic_channel_t*)item;
        if (channel == chptr->channel) {
            ch = chptr;
            break;
        }
    }
    if (NULL == ch) {
        /* didn't find it - open the channel */
        return ORTE_ERR_NOT_IMPLEMENTED;
    }
    
    /* queue it to be sent - preserves order! */
    snd = OBJ_NEW(rmcast_basic_send_t);
    snd->data = buf;
    snd->cbfunc = cbfunc;
    snd->cbdata = cbdata;
    
    /* add it to this channel's pending sends */
    OPAL_THREAD_LOCK(&ch->send_lock);
    opal_list_append(&ch->pending_sends, &snd->item);
    
    /* do we need to start the send event? */
    if (!ch->sends_in_progress) {
        opal_event_add(&ch->send_ev, 0);
        ch->sends_in_progress = true;
    }
    OPAL_THREAD_UNLOCK(&ch->send_lock);
    
    return ORTE_SUCCESS;
}

static int basic_recv(unsigned int channel, opal_buffer_t *buf)
{
    int32_t chan;
    rmcast_basic_recv_t *recvptr;

    chan = orte_rmcast_base.base_ip_addr + channel;
    OPAL_OUTPUT_VERBOSE((2, orte_rmcast_base.rmcast_output,
                         "%s rmcast:basic: recv called on multicast channel %03d.%03d.%03d.%03d",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), OPAL_IF_FORMAT_ADDR(chan)));
    
    recvptr = OBJ_NEW(rmcast_basic_recv_t);
    recvptr->channel = channel;
    
    ORTE_PROGRESSED_WAIT(recvptr->recvd, 0, 1);
    
    opal_dss.copy_payload(buf, &recvptr->data);
    OPAL_THREAD_LOCK(&lock);
    opal_list_remove_item(&recvs, &recvptr->item);
    OPAL_THREAD_UNLOCK(&lock);
    OBJ_RELEASE(recvptr);
    
    return ORTE_SUCCESS;
}

static int basic_recv_nb(unsigned int channel, orte_rmcast_callback_fn_t cbfunc, void *cbdata)
{
    int32_t chan;
    rmcast_basic_recv_t *recvptr;

    chan = orte_rmcast_base.base_ip_addr + channel;
    OPAL_OUTPUT_VERBOSE((2, orte_rmcast_base.rmcast_output,
                         "%s rmcast:basic: recv_nb called on multicast channel %03d.%03d.%03d.%03d",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), OPAL_IF_FORMAT_ADDR(chan)));
    
    recvptr = OBJ_NEW(rmcast_basic_recv_t);
    recvptr->channel = channel;
    recvptr->cbfunc = cbfunc;
    recvptr->cbdata = cbdata;
    
    return ORTE_SUCCESS;
}

static unsigned int get_channel(char *name, uint8_t direction)
{
    opal_list_item_t *item;
    rmcast_basic_channel_t *nchan, *chan;
    
    /* see if this name has already been assigned a channel */
    chan = NULL;
    for (item = opal_list_get_first(&channels);
         item != opal_list_get_end(&channels);
         item = opal_list_get_next(item)) {
        nchan = (rmcast_basic_channel_t*)item;
        
        if (0 == strcmp(nchan->name, name)) {
            chan = nchan;
            break;
        }
    }
    
    if (NULL != chan) {
        /* already exists - check that the requested
         * socket is setup
         */
        if (0 > chan->xmit && ORTE_RMCAST_XMIT & direction) {
            setup_socket(&chan->xmit, chan->channel, false);
        }
        if (0 > chan->recv && ORTE_RMCAST_RECV & direction) {
            setup_socket(&chan->recv, chan->channel, true);
            /* setup an event to catch messages */
            opal_event_set(&chan->recv_ev, chan->recv, OPAL_EV_READ, recv_handler, chan);
            opal_event_add(&chan->recv_ev, 0);
        }
        return chan->channel;
    }
    
    /* doesn't exist - create it */
    chan = OBJ_NEW(rmcast_basic_channel_t);  /* puts it on list */
    chan->name = strdup(name);
    chan->channel = next_channel++;
    
    /* open requested sockets */
    if (ORTE_RMCAST_XMIT & direction) {
        setup_socket(&chan->xmit, chan->channel, false);
    }
    if (ORTE_RMCAST_RECV & direction) {
        setup_socket(&chan->recv, chan->channel, true);
        /* setup an event to catch messages */
        opal_event_set(&chan->recv_ev, chan->recv, OPAL_EV_READ, recv_handler, chan);
        opal_event_add(&chan->recv_ev, 0);
    }
    
    return chan->channel;
}

static void recv_handler(int sd, short flags, void* user)
{
    opal_list_item_t *item;
    rmcast_basic_channel_t *nchan, *chan;
    rmcast_basic_recv_t *recvptr, *ptr;

    /* find this socket */
    chan = NULL;
    for (item = opal_list_get_first(&channels);
         item != opal_list_get_end(&channels);
         item = opal_list_get_next(item)) {
        nchan = (rmcast_basic_channel_t*)item;
        
        if (sd == nchan->recv) {
            chan = nchan;
            break;
        }
    }
    
    if (NULL == chan) {
        /* didn't find it */
        ORTE_ERROR_LOG(ORTE_ERR_NOT_FOUND);
        return;
    }
    
    /* find the recv for this channel */
    recvptr = NULL;
    for (item = opal_list_get_first(&recvs);
         item != opal_list_get_end(&recvs);
         item = opal_list_get_next(item)) {
        ptr = (rmcast_basic_recv_t*)item;
        
        if (chan->channel == ptr->channel) {
            recvptr = ptr;
            break;
        }
    }
    
    if (NULL == recvptr) {
        /* didn't find it */
        ORTE_ERROR_LOG(ORTE_ERR_NOT_FOUND);
        return;
    }
    
    if (NULL != recvptr->cbfunc) {
        recvptr->cbfunc(recvptr->channel, NULL, recvptr->cbdata);
    }

    /* flag it as recvd */
    recvptr->recvd = true;
    
    return;
}

static int setup_socket(int *sd, int channel, bool bindsocket)
{
    uint8_t ttl = 1;
    struct sockaddr_in inaddr;
    struct ip_mreq req;
    int addrlen;
    int target_sd;
    int flags;
    uint32_t chan;

    target_sd = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if(target_sd < 0) {
        if (EAFNOSUPPORT != opal_socket_errno) {
            opal_output(0,"rmcast:init: socket() failed: %s (%d)", 
                        strerror(opal_socket_errno), opal_socket_errno);
        }
        return ORTE_ERR_IN_ERRNO;
    }
    
    /* set the multicast flags */
    if ((setsockopt(target_sd, IPPROTO_IP, IP_MULTICAST_TTL, 
                    (void *)&ttl, sizeof(ttl))) < 0) {
        opal_output(0,"rmcast:init: socketopt() failed: %s (%d)", 
                    strerror(opal_socket_errno), opal_socket_errno);
        return ORTE_ERR_IN_ERRNO;
    }
    
    /* enable port sharing */
    flags = 1;
    if (setsockopt (target_sd, SOL_SOCKET, SO_REUSEADDR, (const char *)&flags, sizeof(flags)) < 0) {
        opal_output(0, "rmcast:basic: unable to set the "
                    "SO_REUSEADDR option (%s:%d)\n",
                    strerror(opal_socket_errno), opal_socket_errno);
        CLOSE_THE_SOCKET(target_sd);
        return ORTE_ERROR;
    }
    
    chan = orte_rmcast_base.base_ip_addr + channel;
    
    /* Bind the socket if requested */
    if (bindsocket) {
        if (AF_INET != orte_rmcast_base.af_family) {
            return ORTE_ERROR;
        }
        memset(&inaddr, 0, sizeof(inaddr));
        inaddr.sin_family = AF_INET;
        inaddr.sin_addr.s_addr = htonl(chan);
        inaddr.sin_port = htons(orte_rmcast_base.ports[channel-1]);
        addrlen = sizeof(struct sockaddr_in);
        
        /* bind the socket */
        if (bind(target_sd, (struct sockaddr*)&inaddr, addrlen) < 0) {
            opal_output(0, "%s rmcast:init: bind() failed: %s (%d)",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                        strerror(opal_socket_errno), opal_socket_errno);
            CLOSE_THE_SOCKET(target_sd);
            return ORTE_ERROR;
        }
    }
    
    /* set membership to "any" */
    memset(&req, 0, sizeof (req));
    req.imr_multiaddr.s_addr = htonl(chan);
    req.imr_interface.s_addr = htonl(INADDR_ANY);
    
    if ((setsockopt(target_sd, IPPROTO_IP, IP_ADD_MEMBERSHIP, 
                    (void *)&req, sizeof (req))) < 0) {
        opal_output(0, "%s rmcast:init: setsockopt() failed: %s (%d)",
                    ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                    strerror(opal_socket_errno), opal_socket_errno);
        CLOSE_THE_SOCKET(target_sd);
        return ORTE_ERROR;
    }
    
    /* return the socket */
    *sd = target_sd;
    
    return ORTE_SUCCESS;
}

static void xmit_data(int sd, short flags, void* send_req)
{
    rmcast_basic_channel_t *chan = (rmcast_basic_channel_t*)send_req;
    rmcast_basic_send_t *snd;
    opal_list_item_t *item;
    char *bytes;
    int32_t sz;
    int rc;
    
    OPAL_THREAD_LOCK(&chan->send_lock);
    while (NULL != (item = opal_list_remove_first(&chan->pending_sends))) {
        snd = (rmcast_basic_send_t*)item;
        
        /* extract the payload */
        opal_dss.unload(snd->data, (void**)&bytes, &sz);
        
        /* send it */
        rc = sendto(chan->xmit, bytes, sz, 0,
                    (struct sockaddr *)&(chan->addr), sizeof(struct sockaddr_in));
        
        /* reload into original buffer */
        opal_dss.load(snd->data, (void*)bytes, sz);
        
        /* call the cbfunc if required */
        if (NULL != snd->cbfunc) {
            snd->cbfunc(chan->channel, snd->data, snd->cbdata);
        }
        /* cleanup */
        OBJ_RELEASE(item);
    }
    
    /* cleanup */
    opal_event_del(&chan->send_ev);
    chan->sends_in_progress = false;

    OPAL_THREAD_UNLOCK(&chan->send_lock);
}