/*
 * Copyright (c) 2010 Cisco Systems, Inc.  All rights reserved.
 * $COPYRIGHT$
 */


#if       __INTEL_COMPILER
#pragma warning(disable:593)
#pragma warning(disable:869)
#pragma warning(disable:981)
#pragma warning(disable:1338)
#pragma warning(disable:2259)
#endif // __INTEL_COMPILER

#include <pmi.h>
#include "cci/config.h"
#include "cci.h"
#include "plugins/core/core.h"
#include "core_gni.h"

volatile int32_t                gni_shut_down=0;
gni_globals_t *                 gglobals=NULL;
pthread_t                       gni_tid;
size_t                          gni_page;    // Page size
size_t                          gni_line;    // Data cacheline size
#ifdef    USE_PMI
char *                          cpPTAG="PMI_GNI_PTAG";
char *                          cpCOOKIE="PMI_GNI_COOKIE";
char *                          cpLOC_ADDR="PMI_GNI_LOC_ADDR";
#else  // USE_PMI
char *                          cpPTAG="SHARED_PD_PTAG";
char *                          cpCOOKIE="SHARED_PD_COOKIE";
#endif // USE_PMI


// Cycle count sampling code -- START
#define   ENABLE_GNI_SAMPLING 0
#if       ENABLE_GNI_SAMPLING
#define   GNI_NUM_SAMPLES     1000
uint64_t *                      gni_start_ns;
uint64_t *                      gni_end_ns;
static int32_t                  gni_num_samples=GNI_NUM_SAMPLES;
static int32_t                  gni_sample=0;
int32_t                         gni_debug_is_server=0;

static inline void gni_sample_init(void) {

    int32_t                     i;

    gni_start_ns=calloc( GNI_NUM_SAMPLES, sizeof(*gni_start_ns) );
    gni_end_ns=calloc( GNI_NUM_SAMPLES, sizeof(*gni_end_ns) );
    if( !gni_start_ns || !gni_end_ns ) {

        if(gni_start_ns)
            free(gni_start_ns);
        else
            free(gni_end_ns);
        gni_num_samples=0;
        return;
    }
    for( i=0; i<GNI_NUM_SAMPLES; i++ ) {

        gni_start_ns[i]=0;
        gni_end_ns[i]=0;
    }
    gni_sample=0;
}

#define   GNI_SAMPLE_START                                            \
do {                                                                  \
    if(!gni_start_ns)                                                 \
        break;                                                        \
    if( gni_sample<gni_num_samples )                                  \
        gni_start_ns[gni_sample]=gni_get_nsecs();                     \
} while(0)

#define   GNI_SAMPLE_END                                              \
do {                                                                  \
    if(!gni_end_ns)                                                   \
        break;                                                        \
    if( gni_sample<gni_num_samples )                                  \
        gni_end_ns[gni_sample++]=gni_get_nsecs();                     \
} while(0)

#define   GNI_IS_SERVER                                               \
do {                                                                  \
    gni_debug_is_server=1;                                            \
} while(0)

#define   GNI_SAMPLE_INIT                                             \
do {                                                                  \
    gni_sample_init();                                                \
} while(0)

#define   GNI_SAMPLE_FREE                                             \
do {                                                                  \
    if(gni_start_ns)                                                  \
        free(gni_start_ns);                                           \
    if(gni_end_ns)                                                    \
        free(gni_end_ns);                                             \
} while(0)

#define   GNI_SAMPLE_PRINT                                            \
do {                                                                  \
    int32_t                     i;                                    \
    for( i=0; i<GNI_NUM_SAMPLES; i++ ) {                              \
        debug( CCI_DB_WARN, "%4d %6llu", i,                           \
              (unsigned long long)(gni_end_ns[i]-gni_start_ns[i]) );  \
    }                                                                 \
} while(0)

#else  // ENABLE_GNI_SAMPLING
#define   GNI_SAMPLE_INIT
#define   GNI_SAMPLE_START
#define   GNI_SAMPLE_END
#define   GNI_SAMPLE_PRINT
#define   GNI_SAMPLE_FREE
#define   GNI_IS_SERVER
#endif // ENABLE_GNI_SAMPLING
// Cycle count sampling code -- FINISH


// Local functions
static int         gni_init(
    uint32_t                    abi_ver,
    uint32_t                    flags,
    uint32_t *                  caps );
static const char *gni_strerror(
    enum cci_status             gRv );
static int         gni_get_devices(
    cci_device_t const ***      devices );
static int         gni_free_devices(
    cci_device_t const **       devices );
static int         gni_create_endpoint(
    cci_device_t *              device,
    int32_t                     flags,
    cci_endpoint_t **           endpoint,
    cci_os_handle_t *           fd );
static int         gni_destroy_endpoint(
    cci_endpoint_t *            endpoint );
static int         gni_accept(
    union cci_event *           conn_req,
    void *                      context,
    cci_connection_t **         connection );
static int         gni_reject(
    union cci_event *           conn_req );
static int         gni_connect(
    cci_endpoint_t *            endpoint,
    char *                      server_uri,
    void *                      data_ptr,
    uint32_t                    data_len,
    cci_conn_attribute_t        attribute,
    void *                      context,
    int32_t                     flags,
    struct timeval *            timeout );
static int         gni_disconnect(
    cci_connection_t *          connection );
static int         gni_set_opt(
    cci_opt_handle_t *          handle,
    cci_opt_level_t             level,
    cci_opt_name_t              name,
    const void *                val,
    int32_t                     len );
static int         gni_get_opt(
    cci_opt_handle_t *          handle,
    cci_opt_level_t             level,
    cci_opt_name_t              name,
    void **                     val,
    int32_t *                   len );
static int         gni_arm_os_handle(
    cci_endpoint_t *            endpoint,
    int32_t                     flags );
static int         gni_get_event(
    cci_endpoint_t *            endpoint,
    cci_event_t ** const        event );
static int         gni_return_event(
    cci_event_t *               event );
static int         gni_send(
    cci_connection_t *          connection,
    void *                      ptr,
    uint32_t                    len,
    void *                      context,
    int32_t                     flags );
static int         gni_sendv(
    cci_connection_t *          connection,
    struct iovec *              data,
    uint32_t                    iovcnt,
    void *                      context,
    int32_t                     flags );
static int         gni_rma_register(
    cci_endpoint_t *            endpoint,
    cci_connection_t *          connection,
    void *                      start,
    uint64_t                    length,
    uint64_t *                  rma_handle );
static int         gni_rma_deregister(
    uint64_t                  rma_handle );
static int         gni_rma(
    cci_connection_t *          connection,
    void *                      msg_ptr,
    uint32_t                    msg_len,
    uint64_t                    local_handle,
    uint64_t                    local_offset,
    uint64_t                    remote_handle,
    uint64_t                    remote_offset,
    uint64_t                    len,
    void *                      context,
    int32_t                     flags );

static void        gni_get_ep_id(
    gni_dev_t *                 gdev,  
    uint32_t *                  id );
static void        gni_put_ep_id(
    gni_dev_t *                 gdev,
    uint32_t                    id );
static int gni_add_rx(
    int                         i,
    cci__ep_t *                 ep );
static int gni_add_tx(
    int                         i,
    cci__ep_t *                 ep );
static void *      gni_progress_thread(
    void *                      arg );
static inline void gni_reap_send(
    cci__dev_t *                dev );
static inline int gni_reap_recv(
    cci__dev_t *                dev );


static char *      gni_cci_attribute_to_str(
    const cci_conn_attribute_t  attr ) {

    static char *str[]={

        "CCI_CONN_ATTR_RO",
        "CCI_CONN_ATTR_RU",
        "CCI_CONN_ATTR_UU",
        "CCI_CONN_ATTR_UU_MC_TX",
        "CCI_CONN_ATTR_UU_MC_RX"
    };

    return str[attr];
}


static char *      gni_cci_event_to_str(
    const cci_event_type_t      event ) {

    static char *str[]={

        "CCI_EVENT_NONE",
        "CCI_EVENT_SEND",
        "CCI_EVENT_RECV",
        "CCI_EVENT_CONNECT_ACCEPTED",
        "CCI_EVENT_CONNECT_TIMEDOUT",
        "CCI_EVENT_CONNECT_REJECTED",
        "CCI_EVENT_CONNECT_REQUEST",
        "CCI_EVENT_KEEPALIVE_TIMEDOUT",
        "CCI_EVENT_ENDPOINT_DEVICE_FAILED"
    };

    return str[event];
}


/*
static char *      gni_cci_opt_to_str(
    const cci_opt_name_t        opt ) {

    static char *str[]={

        "CCI_OPT_ENDPT_SEND_TIMEOUT",
        "CCI_OPT_ENDPT_RECV_BUF_COUNT",
        "CCI_OPT_ENDPT_SEND_BUF_COUNT",
        "CCI_OPT_ENDPT_KEEPALIVE_TIMEOUT",
        "CCI_OPT_CONN_SEND_TIMEOUT"
    };

    return str[opt];
}
*/


static char * gni_conn_status_to_str(
    const gni_conn_status_t     status ) {

    static char *str[]={
        "GNI_CONN_PENDING_REQUEST",
        "GNI_CONN_PENDING_REPLY",
        "GNI_CONN_ACCEPTED",
        "GNI_CONN_REJECTED",
        "GNI_CONN_FAILED",
        "GNI_CONN_DISCONNECTED"
    };

    return str[status];
}


static void gni_log_sys(                     // Convenience function to
    const char *                pcW,         // .. system errors
    const char *                pcA ) {

    const cci_device_t *        device;
    cci__dev_t *                dev;
    gni_dev_t *                 gdev;

    CCI_ENTER;

    if(!gglobals)                             // No globalS
        goto FAIL;

    device=*(gglobals->devices);
    dev=container_of( device, cci__dev_t, device );
    gdev=dev->priv;

    if( errno!=0 )                           // Report only if error
        debug( CCI_DB_WARN, "%8s.%5d %s: %s: %s\n", gdev->nodename,
               gdev->inst_id, pcW, pcA, strerror(errno) );

    FAIL:
    CCI_EXIT;
    return;
}


static void gni_log_gni(                     // Convenience function to
    const char *                pcW,         // .. report GNI errors
    const char *                pcA,
    gni_return_t                gRv ) {      // GNI API return value

    const cci_device_t *        device;
    cci__dev_t *                dev;
    gni_dev_t *                 gdev;

    CCI_ENTER;

    if(!gglobals)                             // No globalS
        goto FAIL;

    device=*(gglobals->devices);
    dev=container_of( device, cci__dev_t, device );
    gdev=dev->priv;

    if( gRv!=GNI_RC_SUCCESS )                // Report only if error
        debug( CCI_DB_WARN, "%8s.%5d %s: %s: %s\n", gdev->nodename,
               gdev->inst_id, pcW, pcA, gni_err_str[gRv] );

    FAIL:
    CCI_EXIT;
    return;
}


#ifdef    USE_PMI
static char * colon_tok(                     // Return i'th token
    char *                      cpPtr,       // String to tokenize
    int                         i ) {        // entry to search out

    char *                      cp;          // Character temp
    char *                      cpTok;       // Return value
    int                         j=-1;        // Initialize counter to -1

    CCI_ENTER;

    cp=cpPtr;                                // Colon-delimited list
    while( (cpTok=strtok( cp, ":" )) ) {     // Search list

        cp=NULL;                             // Continue with list
        if( (++j)<i )                        // Skip to i'th entry
            continue;
        break;
    }

    CCI_EXIT;
    return(cpTok);
}
#endif // USE_PMI


static uint8_t gni_get_ptag(void) {          // Return ptag

    uint8_t                     ptag;        // Return value
    char *                      cp;          // Character temp

    CCI_ENTER;

    assert( (cp=getenv(cpPTAG)) );           // Environment must exist
#ifdef    USE_PMI
    ptag=(uint8_t)atoi(colon_tok( cp, 0 ));
#else  // USE_PMI
    ptag=(uint8_t)strtol( cp, NULL, 0 );
#endif // USE_PMI

    CCI_EXIT;
    return(ptag);
}


static uint32_t gni_get_cookie(void) {       // Return cookie

    uint32_t                    cookie;      // Return value
    char *                      cp;          // Character temp

    CCI_ENTER;

    assert( (cp=getenv(cpCOOKIE)) );         // Environment must exist
#ifdef    USE_PMI
    cookie=(uint32_t)atoi(colon_tok( cp, 0 ));
#else  // USE_PMI
    cookie=(uint32_t)strtol( cp, NULL, 0 );
#endif // USE_PMI

    CCI_EXIT;
    return(cookie);
}


static uint32_t gni_get_nic_addr(            // Return NIC address
    const uint8_t               i ) {        // i'th GNI kernel driver

    uint32_t                    nic_addr;    // Return value
#ifdef    USE_PMI
    char *                      cp;          // Character temp
#else  // USE_PMI
    uint32_t                    iPE;
    gni_return_t                gRv;
#endif // USE_PMI

    CCI_ENTER;

    if(!gglobals)                             // No globalS
        goto FAIL;

#ifdef    USE_PMI
    assert( (cp=getenv(cpLOC_ADDR)) );       // Environment must exist
    assert( (cp=colon_tok( cp, i )) );       // Bad if entry not found
    nic_addr=(uint32_t)atoi(cp);
#else  // USE_PMI
    gRv=GNI_CdmGetNicAddress( i,             // device kernel ID
                              &nic_addr,     // Only physical address
                              &iPE );        // PE directly connected
    gni_log_gni( __func__, "GNI_CdmGetNicAddress", gRv );
    assert( gRv==GNI_RC_SUCCESS );
#endif // USE_PMI

    FAIL:
    CCI_EXIT;
    return(nic_addr);
}


static int gni_get_socket(                   // To initialize GNI, we
    cci_device_t *              device ) {   //    need socket to get
                                             //    remote gni_mailbox_t
    int                         iRv;
    int                         flags;
    struct sockaddr_in          sin;
    struct ifaddrs *            pif;

    int                         sd=-1;       // Socket descriptor
    int                         backlog=128;
    struct ifaddrs *            pif0=NULL;
    socklen_t                   is=sizeof(sin);
    cci__dev_t *                dev=container_of( device, cci__dev_t,
                                                  device );
    gni_dev_t *                 gdev=dev->priv;
    cci_status_t                cRv=CCI_ENODEV;

    CCI_ENTER;

    if(!gglobals)                            // Sanity check
        goto FAIL;

    sd=socket( AF_INET, SOCK_STREAM, 0 );    // Try to create socket
    if( sd==-1 ) {                           // .. failed

        gni_log_sys( __func__, "socket" );
        goto FAIL;
    }

    if( getifaddrs(&pif0)==-1 ) {            // Get list of interfaces

        gni_log_sys( __func__, "getifaddrs" );
        goto FAIL;
    }

    for( pif=pif0; pif!=NULL;                // Search list
         pif=pif->ifa_next ) {

        if( strncmp( pif->ifa_name, GNI_IP_IF, strlen(GNI_IP_IF) ) )
            continue;                        // Skip unless names match

        if( pif->ifa_addr->sa_family!=AF_INET )
            continue;                        // Skip if not TCP/IP

        if( pif->ifa_flags & IFF_UP );
            break;                           // Stop if up
    }

    if(!pif) {                               // Search failed

        errno=ENODEV;                        // Set errno
        gni_log_sys( __func__, "Search failed" );
        goto FAIL;
    }

    memcpy( &sin, pif->ifa_addr, is );       // Get address of interface
    if(gdev->port)                           // Set port
        sin.sin_port=gdev->port;             // .. not ephemeral port

    iRv=bind( sd,                            // Bind socket
              (const struct sockaddr *)&sin,
              is );
    if( iRv==-1 ) {                          // .. failed

        gni_log_sys( __func__, "bind" );
        assert(!iRv);                        // critical failure
    }

    flags=fcntl( sd, F_GETFL, 0 );           // Get socket flags
    if( flags==-1 )                          // .. failed .. reset
        flags=0;

    iRv=fcntl( sd, F_SETFL,                  // Try to set non-blocking
               flags | O_NONBLOCK );         // .. want asynchronous
    if( iRv==-1 ) {                          // .. failed

        gni_log_sys( __func__, "fcntl" );
        goto FAIL;
    }

    iRv=getsockname( sd,                     // If socket was ephemeral
                     (struct sockaddr *)&sin,// .. need to get updated
                     &is );                  // .. address (port)
    if( iRv==-1 ) {                          // .. failed

        gni_log_sys( __func__, "getsockname" );
        goto FAIL;
    }

    if( listen( sd, backlog )==-1 ) {        // Set socket to listen

        gni_log_sys( __func__, "listen" );
        goto FAIL;
    }

    gdev->port=sin.sin_port;                 // Update (ephemeral port)
    gdev->sd=sd;                             // sd for listen port
    debug( CCI_DB_INFO, "%8s.%5d %s: listen on if=%s addr=%s:%d sd=%d",
           gdev->nodename, gdev->inst_id, __func__, pif->ifa_name,
           inet_ntoa(sin.sin_addr), gdev->port, gdev->sd );

    cRv=CCI_SUCCESS;

    FAIL:
    if(pif0)
        freeifaddrs(pif0);

    if( cRv!=CCI_SUCCESS ) {

        if( sd!=-1 )
            close(sd);
        gdev->sd=-1;
    }

    CCI_EXIT;
    return(cRv);
}


static int gni_create_src_box( 
    cci__conn_t *               conn ) {     //    

    const cci_device_t *        device;
    cci_connection_t *          connection;
    cci__dev_t *                dev;
    gni_dev_t *                 gdev;
    gni_conn_t *                gconn;

    uint64_t *                  buffer;       // Address of SMSG mailbox
    uint32_t                    Size;         // Size of mailbox
    gni_return_t                gRv;

    CCI_ENTER;

    if(!gglobals)                             // No globalS
        goto FAIL;

    if(!conn)                                 // Connection unallocated
        goto FAIL;

    if(!conn->priv)                           // No GNI part
        goto FAIL;

    device=*(gglobals->devices);
    dev=container_of( device, cci__dev_t, device );
    gdev=dev->priv;
    connection=&conn->connection;
    gconn=conn->priv;

    gconn->credits=GNI_MBOX_MAX_CREDIT;      // Set limit on credits
    debug( CCI_DB_INFO, "%8s.%5d %s: credits=               %8d",
           gdev->nodename, gdev->inst_id, __func__, gconn->credits );

    gconn->vmd_index=-1;                     // Use next available entry
                                             //   in Memory Domain
                                             //   Desciptor Block
    debug( CCI_DB_INFO, "%8s.%5d %s: vmd_index=             %8d",
           gdev->nodename, gdev->inst_id, __func__, gconn->vmd_index );

    gconn->vmd_flags=GNI_MEM_READWRITE;      // memory region attributes
    debug( CCI_DB_INFO, "%8s.%5d %s: vmd_flags=           0x%.8x",
           gdev->nodename, gdev->inst_id, __func__, gconn->vmd_flags );

    gRv=GNI_CqCreate(                        // Create local CQ
        gdev->nic_hndl,                      // NIC handle
        GNI_MBOX_MAX_CREDIT,                 // max events
        0,                                   // interrupt on every event
        GNI_CQ_NOBLOCK,                      // interrupt on every event
        NULL,                                // address of event handler
        NULL,                                // context for handler
        &(gconn->src_cq_hndl) );             // Get CQ (sends) handle
    if( gRv!=GNI_RC_SUCCESS ) {

        gni_log_gni( __func__, "GNI_CqCreate[src_cq]", gRv );
        goto FAIL;
    }
    debug( CCI_DB_INFO,
           "%8s.%5d %s: src_cq_hndl=         0x%.8zx depth=  %8d",
           gdev->nodename, gdev->inst_id, __func__, gconn->src_cq_hndl,
           GNI_MBOX_MAX_CREDIT );

    gRv=GNI_CqCreate(                        // Create destination CQ
        gdev->nic_hndl,                      // NIC handle
        GNI_EP_RX_CNT,                       // max events
        0,                                   // interrupt on every event
        GNI_CQ_NOBLOCK,                      // interrupt on every event
        NULL,                                // address of event handler
        NULL,                                // context for handler
        &(gconn->dst_cq_hndl) );             // Get CQ (receives) handle
    if( gRv!=GNI_RC_SUCCESS ) {

        gni_log_gni( __func__, "GNI_CqCreate[dst_cq]", gRv );
        goto FAIL;
    }
    debug( CCI_DB_INFO,
           "%8s.%5d %s: dst_cq_hndl=         0x%.8zx depth=  %8d",
           gdev->nodename, gdev->inst_id, __func__, gconn->dst_cq_hndl,
           GNI_EP_RX_CNT );

//  Set up mailbox.
    gconn->src_box.nic_addr=gdev->nic_addr;
    gconn->src_box.inst_id=gdev->inst_id;
    gconn->src_box.attr.msg_type=GNI_SMSG_TYPE_MBOX_AUTO_RETRANSMIT;
    gconn->src_box.attr.mbox_maxcredit=GNI_MBOX_MAX_CREDIT;
    gconn->src_box.attr.msg_maxsize=dev->device.max_send_size+
                                  GNI_MAX_HDR_SIZE;
    gRv=GNI_SmsgBufferSizeNeeded( 
        &(gconn->src_box.attr),              // SMSG attributes
        &Size );                             // Get mailbox size
    if( gRv!=GNI_RC_SUCCESS ) {

        gni_log_gni( __func__, "GNI_SmsgBufferSizeNeeded",
                     gRv );
        goto FAIL;
    }
    Size+=(gni_line-(Size%gni_line));        // Align to page boundary
    gconn->src_box.attr.buff_size=Size;      // mbox is page-aligned
    gconn->src_box.attr.mbox_offset=0;
    posix_memalign(                          // Allocate mailbox buffer
        (void **)&buffer,                    // pointer to VMD
        gni_line,                            // put VMD on page boundary
        Size );
    if(!buffer)
        goto FAIL;

    gconn->src_box.attr.msg_buffer=buffer;
    gRv=GNI_MemRegister(
        gdev->nic_hndl,                      // NIC handle
        (uint64_t)buffer,                    // Memory block
        Size,                                // Size of memory block
        gconn->dst_cq_hndl,                  // Note cq handle
        gconn->vmd_flags,                    // Memory region attributes
        gconn->vmd_index,                    // Allocation option
        &(gconn->src_box.attr.mem_hndl) );   // Memory handle
    if( gRv!=GNI_RC_SUCCESS ) {

        gni_log_gni( __func__, "GNI_MemRegister", gRv );
        goto FAIL;
    }
    debug( CCI_DB_CONN, 
           "%8s.%5d %s: 0x%.8x 0x%.4x %d %zp %d %x %x %d %d %d",
           gdev->nodename, gdev->inst_id, __func__,
           gconn->src_box.nic_addr, gconn->src_box.inst_id,
           gconn->src_box.attr.msg_type, gconn->src_box.attr.msg_buffer,
           gconn->src_box.attr.buff_size, gconn->src_box.attr.mem_hndl,
           gconn->src_box.attr.mbox_offset,
           gconn->src_box.attr.mbox_maxcredit,
           gconn->src_box.attr.msg_maxsize );

//  Note info, gconn, and cci_attr have nothing to do with the SMSG
//  mailbox; they are placed in structure to simplify the connection
//  request.
    gconn->src_box.cci_attr=connection->attribute;
    gconn->src_box.gconn=gconn;
    gconn->src_box.info.length=gconn->data_len;

    FAIL:
    return(CCI_SUCCESS);
}


static int gni_initialize_smsg( 
    cci__conn_t *               conn ) {     //    

    const cci_device_t *        device;
    cci__dev_t *                dev;
    gni_dev_t *                 gdev;
    gni_conn_t *                gconn;
    gni_return_t                gRv;
    cci_connection_t *          connection;

    CCI_ENTER;

    connection=&conn->connection;
    device=*(gglobals->devices);
    dev=container_of( device, cci__dev_t, device );
    gdev=dev->priv;
    gconn=conn->priv;

    if(!gglobals)                             // No globalS
        goto FAIL;

    if(!conn)                                 // Connection unallocated
        goto FAIL;

    if(!conn->priv)                           // No GNI part
        goto FAIL;

    gRv=GNI_EpCreate( gdev->nic_hndl,        // Create GNI endpoint
                      gconn->src_cq_hndl,
                      &(gconn->ep_hndl) );
    if( gRv!=GNI_RC_SUCCESS ) {

        gni_log_gni( __func__, "GNI_EpCreate", gRv );
        goto FAIL;
    }

    gRv=GNI_EpBind( gconn->ep_hndl,          // Bind to remote instance
                    gconn->dst_box.nic_addr,
                    gconn->dst_box.inst_id );
    if( gRv!=GNI_RC_SUCCESS ) {

        gni_log_gni( __func__, "GNI_EpBind", gRv );
        goto FAIL;
    }

    gRv=GNI_SmsgInit( gconn->ep_hndl,        // Initialize mailbox
                      &(gconn->src_box.attr),
                      &(gconn->dst_box.attr) );
    if( gRv!=GNI_RC_SUCCESS ) {

        gni_log_gni( __func__, "GNI_SmsgInit", gRv );
        goto FAIL;
    }

//  Arbitrate connection to smaller of send sizes.
    if( gconn->dst_box.attr.msg_maxsize<dev->device.max_send_size)
        connection->max_send_size=gconn->dst_box.attr.msg_maxsize;
    else
        connection->max_send_size=dev->device.max_send_size;

    debug( CCI_DB_CONN, "%8s.%5d %s: created SMSG",
           gdev->nodename, gdev->inst_id, __func__ );

    FAIL:
    return(CCI_SUCCESS);
}


// Public plugin structure.
//
// The name of this structure must be of the following form:
//
//    cci_core_<your_plugin_name>_plugin
//
// This allows the symbol to be found after the plugin is dynamically
// opened.
//
// Note that your_plugin_name should match the direct name where the
// plugin resides.
cci_plugin_core_t cci_core_gni_plugin= {
    {
//      Logistics
        CCI_ABI_VERSION,
        CCI_CORE_API_VERSION,
        "gni",
        CCI_MAJOR_VERSION, CCI_MINOR_VERSION, CCI_RELEASE_VERSION,
        5,

//      Bootstrap function pointers
        cci_core_gni_post_load,
        cci_core_gni_pre_unload,
    },

//  API function pointers
    gni_init,
    gni_strerror,
    gni_get_devices,
    gni_free_devices,
    gni_create_endpoint,
    gni_destroy_endpoint,
    gni_accept,
    gni_reject,
    gni_connect,
    gni_disconnect,
    gni_set_opt,
    gni_get_opt,
    gni_arm_os_handle,
    gni_get_event,
    gni_return_event,
    gni_send,
    gni_sendv,
    gni_rma_register,
    gni_rma_deregister,
    gni_rma
};


static int gni_init(
    uint32_t                    abi_ver,
    uint32_t                    flags,
    uint32_t *                  caps ) {

    int32_t                     iTmp;        // integer temporary
    uint32_t                    iPE;
    struct utsname              uBuf;
    pid_t                       pid;
    cci__dev_t *                dev;
    gni_return_t                gRv;

    int32_t                     iReject=1;   // Default to no device
    cci_status_t                cRv=CCI_ENOMEM;
    cci_device_t **             dl=NULL;

    CCI_ENTER;
    GNI_SAMPLE_INIT;

    uname(&uBuf);                            // Get nodename
    pid=getpid();                            // Get PID
    iTmp=strlen(uBuf.nodename);              // We will need this later
    debug( CCI_DB_FUNC, "%8s.%5d In gni_init()", uBuf.nodename, pid );

//  Allocate container for GNI devices.
    if( !(gglobals=calloc( 1, sizeof(*gglobals) )) )
        goto FAIL;

//  Allocate array of GNI devices.
    if( !(dl=calloc( CCI_MAX_DEVICES, sizeof(*gglobals->devices) )) )
        goto FAIL;

//  Get page size.
#ifdef    linux
    gni_page=sysconf(_SC_PAGESIZE);          // Get page size attribute
#else  // linux
    gni_page=GNI_PAGE_SIZE;                  // Default if no OS tuning
#endif
    debug( CCI_DB_INFO,
           "%8s.%5d %s: PAGE_SIZE=                      %10zdB",
           uBuf.nodename, pid, __func__, gni_page );

//  Get L1 Dcache size (not needed at present).
#ifdef    linux
    gni_line=                                // Get L1 dcache line size
        sysconf(_SC_LEVEL1_DCACHE_LINESIZE);
#else  // linux
    gni_line=GNI_LINE_SIZE;                  // Default if no OS tuning
#endif
    debug( CCI_DB_INFO,
           "%8s.%5d %s: DCACHE_LINESIZE=                       %3zdB",
           uBuf.nodename, pid, __func__, gni_line );

//  Step 1.  Extract Gemini device(s) from global configuration.
    srandom( (unsigned int)gni_get_usecs() );
    TAILQ_FOREACH( dev, &globals->devs, entry ) {

        const char **           arg;  
        cci_device_t *          device;
        gni_dev_t *             gdev;  

        if(strcmp( "gni", dev->driver ))     // Go to next device if not
            continue;                        // .. using "gni" driver

        gdev=calloc( 1, sizeof(*gdev) );     // Try to create GNI device
        if(!gdev)                            // .. failed
            goto FAIL;

        gdev->ep_ids=calloc( GNI_NUM_BLOCKS, sizeof(*gdev->ep_ids) );
        if(!gdev->ep_ids)
            goto FAIL;

        iReject=0;                           // Gemini configured
        dev->priv=gdev;                      // Set to GNI device

        device=&dev->device;                 // Select this device
        device->rate=160000000000;           // per Gemini spec
        device->pci.domain=-1;               // per CCI spec
        device->pci.bus=-1;                  // per CCI spec
        device->pci.dev=-1;                  // per CCI spec
        device->pci.func=-1;                 // per CCI spec
        device->max_send_size=GNI_DEFAULT_MSS;

        gdev->progressing=0;                 // Initialize progress flag
        gdev->nodename=calloc( 1, iTmp+1 );  // Allocate nodename
        memset( gdev->nodename, iTmp+1, 0 ); // Clear nodename
        strcpy( gdev->nodename,              // Set nodename
                uBuf.nodename );
        gdev->inst_id=pid;                   // Use PID for instance ID

        if(*caps)                            // Server
            gdev->port=GNI_LISTEN_PORT;      // Use default port
        else                                 // Client
            gdev->port=0;                    // Use ephemeral port


//      Only kernel interface available on Cray is 0.
        gdev->kid=0;
        debug( CCI_DB_INFO,
               "%8s.%5d %s: kid=                                  %4u",
               gdev->nodename, gdev->inst_id, __func__, gdev->kid );

        gdev->ptag=gni_get_ptag();           // Retrieve ptag
        debug( CCI_DB_INFO,
               "%8s.%5d %s: ptag=                               0x%.4x",
               gdev->nodename, gdev->inst_id, __func__, gdev->ptag );

        gdev->cookie=gni_get_cookie();       // Retrieve cookie
        debug( CCI_DB_INFO,
               "%8s.%5d %s: cookie=                         0x%.8zx",
               gdev->nodename, gdev->inst_id, __func__, gdev->cookie );

        gdev->modes=GNI_CDM_MODE_FORK_NOCOPY |\
                    GNI_CDM_MODE_FMA_SHARED  |\
                    0;                       // Set flags on CD
        debug( CCI_DB_INFO,
               "%8s.%5d %s: modes=                          0x%.8zx",
               gdev->nodename, gdev->inst_id, __func__, gdev->modes );

        gdev->nic_addr=gni_get_nic_addr(gdev->kid);

        debug( CCI_DB_INFO,
               "%8s.%5d %s: nic_addr=                       0x%.8zx",
               gdev->nodename, gdev->inst_id, __func__,
               gdev->nic_addr );

        gRv=GNI_CdmCreate(                   // Get Communication Domain
            gdev->inst_id,                   // instance ID
            gdev->ptag,                      // ptag
            gdev->cookie,                    // cookie
            gdev->modes,                     // CD bit-wise flags
            &(gdev->cd_hndl) );              // Get CD handle
        if( gRv!=GNI_RC_SUCCESS ) {

            gni_log_gni( __func__, "GNI_CdmCreate", gRv );
            cRv=CCI_ENODEV;
            goto FAIL;
        }
        debug( CCI_DB_INFO,
               "%8s.%5d %s: cd_hndl=                        0x%.8zx",
               gdev->nodename, gdev->inst_id, __func__, gdev->cd_hndl );

        gRv=GNI_CdmAttach(                   // Attach to CD
            gdev->cd_hndl,                   // CD handle
            gdev->kid,                       // device kernel ID
            &iPE,                            // PE directly connected
            &(gdev->nic_hndl) );             // Get NIC handle
        if( gRv!=GNI_RC_SUCCESS ) {

            gni_log_gni( __func__, "GNI_CdmAttach", gRv );
            cRv=CCI_ENODEV;
            goto FAIL;
        }
        debug( CCI_DB_INFO,
               "%8s.%5d %s: nic_hndl=                       0x%.8zx",
               gdev->nodename, gdev->inst_id, __func__,
               gdev->nic_hndl );

        dl[gglobals->count]=device;
        gglobals->count++;
        dev->is_up=1;

//      Parse conf_argv (configuration file parameters).
        for( arg=device->conf_argv; *arg!=NULL; arg++ ) {

            if(!strncmp( "mtu=", *arg, 4 )) {// Config file override

                const char *    mss_str=*arg+4;
                uint32_t        mss=strtol( mss_str, NULL, 0 );

                if( mss>GNI_MAX_MSS )        // Conform to upper limit
                    mss=GNI_MAX_MSS;
                else if( mss<GNI_MIN_MSS )   // Conform to lower limit
                    mss=GNI_MIN_MSS;
                device->max_send_size=mss;   // Override max_send_size
            }

//          For the purpose of establishing a "connection" with another
//          instance on the Gemini network, the instance requesting the
//          connection is defined to be a client; the instance receiving
//          this request is a server.  The client needs to know the URI
//          of the server in order to make this request.  The server
//          just needs to listen for requests.
//
//          So, the client must specify the IP part of the URI of the
//          server in its connection request.  For example:
//
//              ip://nodename:port
//
//          This allows the client to send its GNI address information
//          and mailbox attributes (along with the CCI connection
//          attributes, length of payload, and optional payload) in the
//          form of a connection request.  In turn, the server will
//          reply with its accept or reject and mailbox attributes.
            if( *caps &&
                !strncmp( "server=", *arg, 7 ) ) {

                char *          server=strdup(*arg+7);
                char *          port;

                for( port=server;
                     *port!=' ' && *port!='\t' &&
                     *port!=':' && *port!='\0';
                     port++ );

                if( *port!=':' )             // Not a delimiter
                    gdev->port=GNI_LISTEN_PORT;
                else                         // Get port override
                    gdev->port=atoi(++port);

                free(server);
            }
        }

//      Increment list of devices.
        dl=realloc( dl, (gglobals->count+1)*sizeof(cci_device_t *) );
        dl[gglobals->count]=NULL;
        *((cci_device_t ***)&gglobals->devices)=dl;

        gni_get_socket(device);              // initialization socket
    }

    if(iReject) {                            // Gemini not configured

        cRv=CCI_ENODEV;
        goto FAIL;
    }

//  Try to create progress thread.
    errno=pthread_create( &gni_tid, NULL, gni_progress_thread, NULL );
    if(errno) {

        gni_log_sys( __func__, "pthread_create" );
        goto FAIL;
    }
    cRv=CCI_SUCCESS;

    FAIL:
    if( cRv!=CCI_SUCCESS) {                  // Failed

        if(dl){                              // Free GNI device(s)

            cci_device_t *      device;
            gni_dev_t *         gdev;

            for( device=dl[0]; device!=NULL; device++ ) {

                dev=container_of( device, cci__dev_t, device );
                if(dev->priv) {

                    gdev=dev->priv;
                    gRv=GNI_CdmDestroy(gdev->cd_hndl);
                    gni_log_gni( __func__, "GNI_CdmDestroy", gRv );
                    assert( gRv==GNI_RC_SUCCESS );

                    close(gdev->sd);         // close listen socket

                    if(gdev->ep_ids)
                        free(gdev->ep_ids);

                    if(gdev->nodename)
                        free(gdev->nodename);

                    free(gdev);
                }
            }
        }
        free(dl);                            // Free devices list

        if(gglobals) {

            free(gglobals);
            gglobals=NULL;
        }
    }

    CCI_EXIT;
    return(cRv);
}


static const char *gni_strerror(  enum cci_status        gRv ) {

    debug( CCI_DB_FUNC, "In gni_strerror()" );
    return(gni_err_str[(enum cci_status)gRv]);
}


static int gni_get_devices(
    cci_device_t const ***      devices ) {

    cci_device_t const *        device;
    cci__dev_t *                dev;
    gni_dev_t *                 gdev;

    CCI_ENTER;

    if(!gglobals) {

        CCI_EXIT;
        return(CCI_ENODEV);
    }

    *devices=gglobals->devices;
    device=**devices;
    dev=container_of( device, cci__dev_t, device );
    gdev=dev->priv;

    debug( CCI_DB_FUNC, "%8s.%5d In gni_get_devices()",
           gdev->nodename, gdev->inst_id );
    debug( CCI_DB_INFO, "%8s.%5d %s: devices=                   %8d",
           gdev->nodename, gdev->inst_id, __func__, gglobals->count );

    CCI_EXIT;
    return(CCI_SUCCESS);
}


static int gni_free_devices(
    cci_device_t const **       devices ) {

    cci_device_t const *        device;
    cci__dev_t *                dev;
    gni_dev_t *                 gdev;
    gni_return_t                gRv;

    CCI_ENTER;

    if(!gglobals) {

        CCI_EXIT;
        return(CCI_ENODEV);
    }
    device=*devices;
    dev=container_of( device, cci__dev_t, device );
    gdev=dev->priv;

    debug( CCI_DB_FUNC, "%8s.%5d In gni_free_devices()", gdev->nodename,
           gdev->inst_id );

    pthread_mutex_lock(&globals->lock);      // Set shutdown
    gni_shut_down=1;                         // Signal other thread(s)
    pthread_mutex_unlock(&globals->lock);
    pthread_join( gni_tid, NULL );           // Other thread(s) gone

    TAILQ_FOREACH( dev, &globals->devs, entry ) {

        gdev=dev->priv;
        if(gdev) {

            debug( CCI_DB_INFO, "%8s.%5d Closing listen socket: %d",
                   gdev->nodename, gdev->inst_id, gdev->sd );

            close(gdev->sd);                 // close listen socket

            if(gdev->ep_ids)
                free(gdev->ep_ids);

            if(gdev->nodename)
                free(gdev->nodename);

            gRv=GNI_CdmDestroy(gdev->cd_hndl);
            assert( gRv==GNI_RC_SUCCESS );

            free(gdev);
        }
    }

    free(gglobals->devices);
    free((void *)gglobals);

#ifdef    USE_PMI
    fflush(stdout);
    PMI_Finalize();
    PMI_Barrier();                           // Ensure everyone is done
#endif // USE_PMI

    GNI_SAMPLE_PRINT;
    GNI_SAMPLE_FREE;
    CCI_EXIT;
    return(CCI_SUCCESS);
}


static void gni_get_ep_id(
    gni_dev_t *                 gdev,  
    uint32_t *                  id ) { 

    uint32_t                    n;      
    uint32_t                    block;  
    uint32_t                    offset; 
    uint64_t *                  b;     

    while(1) {

        n=random()%GNI_MAX_EP_ID;
        if( n==0 )
            continue;
        block=n/GNI_BLOCK_SIZE;
        offset=n%GNI_BLOCK_SIZE;
        b=&gdev->ep_ids[block];

        if( (*b & (1ULL<<offset))==0 ) {

            *b|=(1ULL<<offset);
            *id=(block*GNI_BLOCK_SIZE)+offset;
            debug( CCI_DB_CONN, "%8s.%5d %s: id=%u block=%"PRIx64"",
                   gdev->nodename, gdev->inst_id, __func__, *id, *b );
            break;  
        }       
    }
    return; 
}

static void gni_put_ep_id(
    gni_dev_t *                 gdev,
    uint32_t                    id ) {

    uint32_t                    block; 
    uint32_t                    offset; 
    uint64_t *                  b;

    block=id/GNI_BLOCK_SIZE;
    offset=id%GNI_BLOCK_SIZE;
    b=&gdev->ep_ids[block];

    debug( CCI_DB_CONN, "%8s.%5d %s: id=%u block=%"PRIx64"",
           gdev->nodename, gdev->inst_id, __func__, id, *b );
    assert( ((*b>>offset)&0x1)==1 );
    *b&=~(1ULL<<offset);

    return; 
}


static int gni_add_rx(                       // Caller holds ep->lock
    int                         i,
    cci__ep_t *                 ep ) {

    gni_ep_t *                  gep=ep->priv;
    gni_rx_t *                  rx;

    int                         ret=1;

    rx=calloc( 1, sizeof(*rx) );
    if(!rx) {

        ret=0;
        goto FAIL;
    }

    rx->evt.ep=ep;                           // static; does not change
    rx->evt.event.recv.type=CCI_EVENT_RECV;  // static; does not change
    *((void **)&rx->evt.event.recv.ptr)=     // static; does not change
        gep->rxbuf+i*ep->buffer_len;         // .. buffer address
    memset( rx->evt.event.recv.ptr, 0,       // Clear/touch buffer
            ep->buffer_len );
    rx->evt.event.recv.connection=NULL;
    *((uint32_t *)&rx->evt.event.recv.len)=0;
    TAILQ_INSERT_TAIL(&gep->rxs_all, rx, centry);
    TAILQ_INSERT_TAIL(&gep->rxs, rx, entry);

    FAIL:
        if(!ret) {
            if(rx)
                free(rx);
        }
        return(ret);
}


static int gni_add_tx(                       // Caller holds ep->lock
    int                         i,
    cci__ep_t *                 ep ) {

    gni_ep_t *                  gep=ep->priv;
    gni_tx_t *                  tx;

    int                         ret=1;

    tx=calloc( 1, sizeof(*tx) );
    if(!tx) {

        ret=0;
        goto FAIL;
    }

    tx->evt.ep=ep;                           // static; does not change
    tx->evt.event.send.type=CCI_EVENT_SEND;  // static; does not change
    tx->id=i;                                // static; does not change
    tx->ptr=gep->txbuf+i*ep->buffer_len;     // address does not change
    memset( tx->ptr, 0, ep->buffer_len );    // Clear/touch buffer
    tx->evt.event.send.connection=NULL;
    tx->evt.event.send.context=NULL;
    tx->len=0;
    tx->zero_copy=1;                         // Default to zero_copy
    TAILQ_INSERT_TAIL(&gep->txs_all, tx, centry);
    TAILQ_INSERT_TAIL(&gep->txs, tx, entry);

    FAIL:
        if(!ret) {
            if(tx) {
                if(tx->evt.event.recv.ptr)
                    free(tx->evt.event.recv.ptr);
                free(tx);
            }
        }
        return(ret);
}


static int gni_create_endpoint(
    cci_device_t *              device,
    int32_t                     flags,
    cci_endpoint_t **           endpoint,
    cci_os_handle_t *           fd ) {

    int32_t                     i;      

    char *                      name=NULL;
    cci__dev_t *                dev=NULL;
    cci__ep_t *                 ep=NULL;
    gni_ep_t *                  gep=NULL;
    gni_dev_t *                 gdev=NULL;
    cci_status_t                cRv=CCI_ENODEV;    

    CCI_ENTER;

    if(!gglobals)
        goto FAIL;

    dev=container_of(device, cci__dev_t, device);
    if(strcmp( "gni", dev->driver )) {

        cRv=CCI_EINVAL;
        goto FAIL;
    }

    gdev=dev->priv;
    debug(CCI_DB_FUNC, "%8s.%5d In gni_create_endpoint()",
          gdev->nodename, gdev->inst_id );

    ep=container_of( *endpoint, cci__ep_t, endpoint );
    ep->priv=calloc( 1, sizeof(*gep) );
    if(!ep->priv) {

        cRv=CCI_ENOMEM;
        goto FAIL;
    }
    debug( CCI_DB_INFO,
           "%8s.%5d %s: device->name=%18s",
           gdev->nodename, gdev->inst_id, __func__, device->name );

    gep=ep->priv;
    (*endpoint)->max_recv_buffer_count=GNI_EP_RX_CNT;
    ep->rx_buf_cnt=GNI_EP_RX_CNT;
    ep->tx_buf_cnt=GNI_EP_TX_CNT;
    ep->buffer_len=dev->device.max_send_size;
    ep->tx_timeout=0;

//  Get endpoint id.
    pthread_mutex_lock(&ep->lock);           // get_ep_id
    gni_get_ep_id( gdev, &gep->id );
    pthread_mutex_unlock(&ep->lock);
    debug( CCI_DB_INFO, "%8s.%5d %s: id=                  0x%.8x",
           gdev->nodename, gdev->inst_id, __func__, gep->id );

//  Initialize queues.
    TAILQ_INIT(&gep->gconns);
    TAILQ_INIT(&gep->rxs_all);
    TAILQ_INIT(&gep->rxs);
    TAILQ_INIT(&gep->txs_all);
    TAILQ_INIT(&gep->txs);
    TAILQ_INIT(&gep->txs_queue);
    TAILQ_INIT(&gep->rma_hndls);
    TAILQ_INIT(&gep->rma_ops);

//  Allocate rx buffer space and assign short message receive buffers.
    pthread_mutex_lock(&ep->lock);           // Create rx/tx
    gep->rxbuf=calloc( ep->rx_buf_cnt, ep->buffer_len );
    for( i=0; i<ep->rx_buf_cnt; i++ )
        if( (cRv=gni_add_rx( i, ep ))!=1 ) {
  
            cRv=CCI_ENOMEM;
            pthread_mutex_unlock(&ep->lock); // fail rx
            goto FAIL;
        }
    debug( CCI_DB_INFO, "%8s.%5d %s: gni_add_rx:    buffers= %8d",
           gdev->nodename, gdev->inst_id, __func__, ep->rx_buf_cnt );

//  Allocate tx buffer space and assign short message send buffers.
    gep->txbuf=calloc( ep->tx_buf_cnt, ep->buffer_len );
    for( i=0; i<ep->tx_buf_cnt; i++ )
        if( (cRv=gni_add_tx( i, ep ))!=1 ) {
  
            cRv=CCI_ENOMEM;
            pthread_mutex_unlock(&ep->lock); // fail tx
            goto FAIL;
        }
    pthread_mutex_unlock(&ep->lock);         // end create tx/rx
    debug( CCI_DB_INFO, "%8s.%5d %s: gni_add_tx:    buffers= %8d",
           gdev->nodename, gdev->inst_id, __func__, ep->tx_buf_cnt );

    name=calloc( 1, GNI_URI_MAX_LENGTH+1 );
    if(!name) {

        cRv=CCI_ENOMEM;
        goto FAIL;
    }
    sprintf( name, "%s0x%.8zx:0x%.4x:%s:0x%.4x", GNI_URI,
             gdev->nic_addr, gdev->inst_id, gdev->nodename,
             gdev->port );
    *((char **)&((*endpoint)->name))=strdup(name);
    free(name);
    debug( CCI_DB_CONN, "%8s.%5d %s: %s",
           gdev->nodename, gdev->inst_id, __func__,
           (*endpoint)->name );
    cRv=CCI_SUCCESS;

    FAIL:
    if( cRv!=CCI_SUCCESS && gep ) {

        gni_rx_t *              rx;
        gni_tx_t *              tx;

        if( *((char **)&((*endpoint)->name)) )
            free( *((char **)&((*endpoint)->name)) );

        if(gep->id)
            gni_put_ep_id( gdev, gep->id );

//      Remove all rx from list of available.
        TAILQ_FOREACH( rx, &gep->rxs, entry )
            TAILQ_REMOVE( &gep->rxs, rx, entry );

//      Clean up.
        TAILQ_FOREACH( rx, &gep->rxs_all, centry ) {

            TAILQ_REMOVE( &gep->rxs_all, rx, centry );
            free(rx);
        }
        if(gep->rxbuf)
            free(gep->rxbuf);
  
//      Remove all tx from list of available.
        TAILQ_FOREACH( tx, &gep->txs, entry )
            TAILQ_REMOVE( &gep->txs, tx, entry );

//     Clean up.
        TAILQ_FOREACH( tx, &gep->txs_all, centry ) {

            TAILQ_REMOVE( &gep->txs_all, tx, centry );
            free(tx);
        }
        if(gep->txbuf)
            free(gep->txbuf);

        free(gep);
    }

    CCI_EXIT;
    return(cRv);
}


static int gni_destroy_endpoint(  cci_endpoint_t *       endpoint ) {

    cci_device_t const *        device;
    cci__dev_t *                dev;
    gni_dev_t *                 gdev;
    gni_return_t                gRv;
    cci__ep_t *                 ep=container_of( endpoint, cci__ep_t,
                                                 endpoint );
    gni_ep_t *                  gep=ep->priv;
    cci__evt_t *                evt;
    gni_conn_t *                gconn;
    gni_rx_t *                  rx;
    gni_tx_t *                  tx;
    gni_rma_hndl_t *            rma_hndl;
    gni_rma_op_t *              rma_op;

    CCI_ENTER;

    if(!gglobals) {

        CCI_EXIT;
        return(CCI_ENODEV);
    }
    device=*(gglobals->devices);
    dev=container_of( device, cci__dev_t, device );
    gdev=dev->priv;
    debug( CCI_DB_FUNC,
           "%8s.%5d In gni_destroy_endpoint()",
           gdev->nodename, gdev->inst_id );

    if( gni_shut_down==1 ) {

        CCI_EXIT;
        return(CCI_SUCCESS);
    }

    free( *((char **)&(endpoint->name)) );

//  Destroy any and all connections.
    TAILQ_FOREACH( gconn, &gep->gconns, entry ) {

        gRv=GNI_EpDestroy(gconn->ep_hndl);
        assert( gRv==GNI_RC_SUCCESS );

        gRv=GNI_MemDeregister( gdev->nic_hndl,
                               &(gconn->src_box.attr.mem_hndl) );
        assert( gRv==GNI_RC_SUCCESS );
        free(gconn->src_box.attr.msg_buffer);

        gRv=GNI_CqDestroy(gconn->src_cq_hndl);
        assert( gRv==GNI_RC_SUCCESS );
        gRv=GNI_CqDestroy(gconn->dst_cq_hndl);
        assert( gRv==GNI_RC_SUCCESS );

        TAILQ_REMOVE( &gep->gconns, gconn, entry );
    }

    if(gep->id)
        gni_put_ep_id( gdev, gep->id );

//  Clear rma operations.
    TAILQ_FOREACH( rma_op, &gep->rma_ops, entry )
        TAILQ_REMOVE( &gep->rma_ops, rma_op, entry );

//  Clear rma handles.
    TAILQ_FOREACH( rma_hndl, &gep->rma_hndls, entry )
        TAILQ_REMOVE( &gep->rma_hndls, rma_hndl, entry );

//  Clear any and all pending events.
    TAILQ_FOREACH( evt, &ep->evts, entry )
        TAILQ_REMOVE( &ep->evts, evt, entry );

//  Remove all rx from list of available.
    TAILQ_FOREACH( rx, &gep->rxs, entry )
        TAILQ_REMOVE( &gep->rxs, rx, entry );

// Clean up.
    TAILQ_FOREACH( rx, &gep->rxs_all, centry ) {

        TAILQ_REMOVE( &gep->rxs_all, rx, centry );
        free(rx);
    }
    free(gep->rxbuf);

//  There should be no entries left; but, clear nonetheless.
    TAILQ_FOREACH( tx, &gep->txs_queue, qentry )
        TAILQ_REMOVE( &gep->txs_queue, tx, qentry );

//  Remove all tx from list of available.
    TAILQ_FOREACH( tx, &gep->txs, entry )
        TAILQ_REMOVE( &gep->txs, tx, entry );

// Clean up.
    TAILQ_FOREACH( tx, &gep->txs_all, centry ) {

        TAILQ_REMOVE( &gep->txs_all, tx, centry );
        free(tx);
    }
    free(gep->txbuf);

    free(gep);

    CCI_EXIT;
    return(CCI_SUCCESS);
}


static int gni_accept(
    union cci_event *           event,
    void *                      context,
    cci_connection_t **         connection ) {

    cci_device_t const *        device;
    gni_dev_t *                 gdev;
    int                         sd;
    gni_mailbox_t *             dst_box;

    int                         sz=sizeof(gni_mailbox_t);
    cci__evt_t *                evt=container_of( event, cci__evt_t,
                                                  event );
    cci__dev_t *                dev=NULL;
    cci__ep_t *                 ep=evt->ep;
    cci_endpoint_t *            endpoint=&ep->endpoint;
    gni_ep_t *                  gep=ep->priv;
    cci__conn_t *               conn=NULL;
    gni_conn_t *                gconn=NULL;
    cci_status_t                cRv=CCI_ENODEV;

    CCI_ENTER;

    if(!gglobals)
        goto FAIL;

    device=*(gglobals->devices);
    dev=container_of( device, cci__dev_t, device );
    gdev=dev->priv;
    debug( CCI_DB_FUNC,
           "%8s.%5d In gni_accept()", gdev->nodename, gdev->inst_id );

    pthread_mutex_lock(&ep->lock);           // Get sd & dst_box
    sd=gep->sd;
    dst_box=gep->dst_box;
    gep->sd=-1;
    gep->dst_box=NULL;
    pthread_mutex_unlock(&ep->lock);         //

    cRv=CCI_SUCCESS;

    conn=calloc( 1, sizeof(*conn) );
    if(!conn)
        goto FAIL;

    gconn=calloc( 1, sizeof(*gconn) );       // Now, the GNI part
    if(!gconn)
        goto FAIL;

    (*connection)=&conn->connection;
    conn->tx_timeout=ep->tx_timeout;         // timeout presently unused
    conn->connection.endpoint=endpoint;
    conn->connection.context=context;
    conn->priv=gconn;
    gconn->status=GNI_CONN_FAILED;
    gconn->conn=conn;                        // point back to conn

    gconn->vmd_index=-1;                     // Use next available entry
                                             //   in Memory Domain
                                             //   Desciptor Block
    debug( CCI_DB_INFO, "%8s.%5d %s: vmd_index=             %8d",
           gdev->nodename, gdev->inst_id, __func__, gconn->vmd_index );

    gconn->data_len=0;
    memcpy( &(gconn->dst_box), dst_box, sz );

    debug( CCI_DB_CONN, 
           "%8s.%5d %s: recv=%d 0x%.8x 0x%.4x %d %zp %d %x %x"
           " %d %d %d %s %zp %d",
           gdev->nodename, gdev->inst_id, __func__, sz,
           gconn->dst_box.nic_addr,
           gconn->dst_box.inst_id,
           gconn->dst_box.attr.msg_type,
           gconn->dst_box.attr.msg_buffer,
           gconn->dst_box.attr.buff_size,
           gconn->dst_box.attr.mem_hndl,
           gconn->dst_box.attr.mbox_offset,
           gconn->dst_box.attr.mbox_maxcredit,
           gconn->dst_box.attr.msg_maxsize,
           gni_cci_attribute_to_str(gconn->src_box.cci_attr),
           gconn->dst_box.gconn,
           gconn->dst_box.info.length );

    gni_create_src_box(conn);              //    
    gni_initialize_smsg(conn);

//  Accept connection request; send reply.
    gconn->src_box.info.reply=GNI_CONN_ACCEPTED;
    gconn->src_box.gconn=gconn;
    if( sz!=send( sd, &(gconn->src_box), sz, 0 ) ) {

        gni_log_sys( __func__, "send" );
        goto FAIL;
    }

    gconn->status=GNI_CONN_ACCEPTED;

    evt=calloc( 1, sizeof(*evt) );           // Create CCI event
    evt->ep=ep;
    evt->event.type=CCI_EVENT_CONNECT_ACCEPTED;
    evt->event.accepted.context=context;
    evt->event.accepted.connection=&conn->connection;
    (*connection)=&conn->connection;

    debug( CCI_DB_CONN, "%8s.%5d %s: posting event: %s",
           gdev->nodename, gdev->inst_id, __func__,
           gni_cci_event_to_str(evt->event.type) );
    pthread_mutex_lock(&ep->lock);           // Queue evt, gconn
    TAILQ_INSERT_TAIL( &ep->evts, evt, entry );
    TAILQ_INSERT_TAIL( &gep->gconns, gconn, entry );
    pthread_mutex_unlock(&ep->lock);

    FAIL:
    close(sd);
    if(dst_box)
        free(dst_box);

    CCI_EXIT;
    return(cRv);
}


static int gni_reject(            union cci_event *      conn_req ) {

    cci_device_t const *        device;
    cci__dev_t *                dev;
    gni_dev_t *                 gdev;

    CCI_ENTER;

    if(!gglobals) {

        CCI_EXIT;
        return(CCI_ENODEV);
    }
    device=*(gglobals->devices);
    dev=container_of( device, cci__dev_t, device );
    gdev=dev->priv;
    debug( CCI_DB_WARN,
           "%8s.%5d In gni_reject()", gdev->nodename, gdev->inst_id );

    CCI_EXIT;
    return(CCI_ERR_NOT_IMPLEMENTED);
}


static int gni_connect(
    cci_endpoint_t *            endpoint,
    char *                      server_uri,
    void *                      data_ptr,
    uint32_t                    data_len,
    cci_conn_attribute_t        attribute,
    void *                      context,
    int32_t                     flags,
    struct timeval *            timeout  ) {

    int                         iRv;
    char *                      port;
    struct sockaddr_in          sin;
    struct addrinfo *           info;
    struct addrinfo             hint;
    cci_connection_t *          connection;

    char *                      hostname=NULL;
    socklen_t                   is=sizeof(sin);
    cci_device_t const *        device=gglobals->devices[0];
    cci__dev_t *                dev=container_of( device, cci__dev_t,
                                                  device );
    gni_dev_t *                 gdev=dev->priv;
    cci__ep_t *                 ep=container_of( endpoint, cci__ep_t,
                                                 endpoint );
    gni_ep_t *                  gep=ep->priv;
    cci__conn_t *               conn=NULL;
    gni_conn_t *                gconn=NULL;
    cci_status_t                cRv=CCI_ENODEV;

    CCI_ENTER;

    if(!gglobals)
        goto FAIL;

    debug( CCI_DB_FUNC,
           "%8s.%5d In gni_connect()", gdev->nodename, gdev->inst_id );

    conn=calloc( 1, sizeof(*conn) );         // Create a cci__conn_t
    if(!conn) {                              // includes the
                                             // .. cci_connection_t
        cRv=CCI_ENOMEM;
        goto FAIL;
    }

    gconn=calloc( 1, sizeof(*gconn) );       // Now, the GNI part
    if(!gconn) {

        cRv=CCI_ENOMEM;
        goto FAIL;
    }

    hostname=strchr( server_uri, '/' );      // Extracting hostname
    if(!hostname) {

        cRv=CCI_EINVAL;                      // Not found
        goto FAIL;
    }
    hostname+=2;                             // Go to start of hostname
    hostname=strdup(hostname);               // Work with a copy

    port=strchr( hostname, ':' );            // Find delimiter
    if(!port) {

        cRv=CCI_EINVAL;                      // Not found
        goto FAIL;
    }
    *port='\0';                              // .. replace with '\0'
    port++;                                  // Skip to port

    memset( &hint, 0, sizeof(hint) );        // Set hints
    hint.ai_family=AF_INET;                  // .. only IP
    hint.ai_socktype=SOCK_STREAM;            // .. only streams
    hint.ai_protocol=IPPROTO_TCP;            // .. only TCP
    if( (iRv=getaddrinfo( hostname, NULL, &hint, &info )) ) {

        debug( CCI_DB_INFO, "%8s.%5d %s: getaddrinfo(%s): %d",
               gdev->nodename, gdev->inst_id, __func__, hostname,
               gai_strerror(iRv) );
        goto FAIL;
    }

    memcpy( &sin, info->ai_addr, is );       // Save socket address
    sin.sin_port=atoi(port);                 // Set server listen port
    freeaddrinfo(info);

//  Set members of cci__conn_t structure.
    connection=&conn->connection;            // Start w/cci_connection_t
    connection->max_send_size=device->max_send_size;
    connection->endpoint=endpoint;
    connection->attribute=attribute;
    connection->context=context;

    conn->uri=strdup(server_uri);            // continue with others
    conn->tx_timeout=ep->tx_timeout;         // Default to ep timeout
    if(timeout)                              // convert to micro-seconds
        conn->tx_timeout=(timeout->tv_sec*1000000)+timeout->tv_usec;
    conn->priv=gconn;

    gconn->conn=conn;                        // point back to conn
    gconn->data_ptr=data_ptr;                // optional payload
    gconn->data_len=data_len;                // payload length
    gconn->sin=sin;                          // target socket address
    gconn->status=GNI_CONN_PENDING_REPLY;    // Set connection status
    debug( CCI_DB_INFO, "%8s.%5d %s: status=         %-24s",
           gdev->nodename, gdev->inst_id, __func__,
           gni_conn_status_to_str(gconn->status) );

    gni_create_src_box(conn);                //    

//  Add to list of gconn on this endpoint.
    pthread_mutex_lock(&ep->lock);           // Queue gconn
    TAILQ_INSERT_TAIL( &gep->gconns, gconn, entry );
    pthread_mutex_unlock(&ep->lock);

    cRv=CCI_SUCCESS;

    FAIL:
    if(hostname)
        free(hostname);
    if( cRv!=CCI_SUCCESS ) {

        if(gconn)
            free(gconn);
        if(conn)
            free(conn);
    }

    CCI_EXIT;
    return(cRv);
}


static int gni_disconnect(        cci_connection_t *     connection ) {

    cci_device_t const *        device;
    cci__dev_t *                dev;
    gni_dev_t *                 gdev;

    CCI_ENTER;

    if(!gglobals) {

        CCI_EXIT;
        return(CCI_ENODEV);
    }
    device=*(gglobals->devices);
    dev=container_of( device, cci__dev_t, device );
    gdev=dev->priv;
    debug( CCI_DB_WARN,
           "%8s.%5d In gni_disconnect()",
           gdev->nodename, gdev->inst_id );

    CCI_EXIT;
    return(CCI_ERR_NOT_IMPLEMENTED);
}


static int gni_set_opt(           cci_opt_handle_t *     handle,
                                  cci_opt_level_t        level,
                                  cci_opt_name_t         name,
                                  const void *           val,
                                  int32_t                len ) {

    cci_device_t const *        device;
    cci__dev_t *                dev;
    gni_dev_t *                 gdev;

    CCI_ENTER;

    if(!gglobals) {

        CCI_EXIT;
        return(CCI_ENODEV);
    }
    device=*(gglobals->devices);
    dev=container_of( device, cci__dev_t, device );
    gdev=dev->priv;
    debug( CCI_DB_WARN,
           "%8s.%5d In gni_set_opt()", gdev->nodename, gdev->inst_id );

    CCI_EXIT;
    return(CCI_ERR_NOT_IMPLEMENTED);
}


static int gni_get_opt(           cci_opt_handle_t *     handle,
                                  cci_opt_level_t        level,
                                  cci_opt_name_t         name,
                                  void **                val,
                                  int32_t *              len ) {

    cci_device_t const *        device;
    cci__dev_t *                dev;
    gni_dev_t *                 gdev;

    CCI_ENTER;

    if(!gglobals) {

        CCI_EXIT;
        return(CCI_ENODEV);
    }
    device=*(gglobals->devices);
    dev=container_of( device, cci__dev_t, device );
    gdev=dev->priv;
    debug( CCI_DB_WARN,
           "%8s.%5d In gni_get_opt()", gdev->nodename, gdev->inst_id );

    CCI_EXIT;
    return(CCI_ERR_NOT_IMPLEMENTED);
}


static int gni_arm_os_handle(     cci_endpoint_t *       endpoint,
                                  int32_t                flags ) {

    cci_device_t const *        device;
    cci__dev_t *                dev;
    gni_dev_t *                 gdev;

    CCI_ENTER;

    if(!gglobals) {

        CCI_EXIT;
        return(CCI_ENODEV);
    }
    device=*(gglobals->devices);
    dev=container_of( device, cci__dev_t, device );
    gdev=dev->priv;
    debug( CCI_DB_WARN,
           "%8s.%5d In gni_arm_os_handle()",
           gdev->nodename, gdev->inst_id );

    CCI_EXIT;
    return(CCI_ERR_NOT_IMPLEMENTED);
}


static int gni_get_event(         cci_endpoint_t *         endpoint,
                                  cci_event_t ** const     event ) {

//  cci_device_t const *        device;
    cci__dev_t *                dev;
    gni_dev_t *                 gdev;
    cci__ep_t *                 ep;
    cci__evt_t *                evt;
    cci_status_t                cRv;

    CCI_ENTER;

    if(!gglobals) {

        CCI_EXIT;
        return(CCI_ENODEV);
    }

    ep=container_of(endpoint, cci__ep_t, endpoint);
    dev=ep->dev;
    gdev=dev->priv;
    debug( CCI_DB_FUNC,
           "%8s.%5d In gni_get_event()",
           gdev->nodename, gdev->inst_id );

    cRv=gni_reap_recv(dev);
    if( cRv!=CCI_SUCCESS )
        return(cRv);
    gni_reap_send(dev);

    pthread_mutex_lock(&ep->lock);           // Get evt
    if( !TAILQ_EMPTY(&ep->evts) ) {

        evt=TAILQ_FIRST(&ep->evts);
        *event=&evt->event;
        cRv=CCI_SUCCESS;
        debug( CCI_DB_INFO, "%8s.%5d %s: found event: %s",
               gdev->nodename, gdev->inst_id, __func__,
               gni_cci_event_to_str((*event)->type) );
    } else
        cRv=CCI_EAGAIN;
    pthread_mutex_unlock(&ep->lock);

    CCI_EXIT;
    return(cRv);
}


static int gni_return_event(
    cci_event_t *               event ) {

    void *                      buffer;
    cci_device_t const *        device;
    cci__dev_t *                dev;
    gni_dev_t *                 gdev;
    gni_rx_t *                  rx;
    gni_tx_t *                  tx;
    int32_t                     free_evt;

    cci__evt_t *                evt=container_of( event, cci__evt_t,
                                                  event );
    cci__ep_t *                 ep=evt->ep;
    gni_ep_t *                  gep=ep->priv;
    cci_status_t                cRv=CCI_ERR_NOT_IMPLEMENTED;

    CCI_ENTER;

    if(!gglobals) {

        CCI_EXIT;
        return(CCI_ENODEV);
    }

    device=*(gglobals->devices);
    dev=container_of( device, cci__dev_t, device );
    gdev=dev->priv;
    debug( CCI_DB_MSG,
           "%8s.%5d In gni_return_event(): type=%d",
           gdev->nodename, gdev->inst_id, event->type );

    switch(event->type) {

        case CCI_EVENT_RECV:
            rx=container_of( evt, gni_rx_t, evt );
//          Reset rx.
            memset( rx->evt.event.recv.ptr,  // Clear buffer
                    0, ep->buffer_len );
            *((uint32_t *)&rx->evt.event.recv.len)=0;
            rx->evt.event.recv.connection=NULL;
            pthread_mutex_lock(&ep->lock);   // Return rx to list
            TAILQ_INSERT_HEAD( &gep->rxs, rx, entry );
            pthread_mutex_unlock(&ep->lock);
            free_evt=0;
            goto event_next;

        case CCI_EVENT_SEND:
            tx=container_of( evt, gni_tx_t, evt );
//          Reset tx.
            if(!tx->zero_copy)               // If buffer was used,
                memset( tx->ptr, 0,          // .. clear it
                        ep->buffer_len );
            tx->len=0;                       // Reset used length
            tx->zero_copy=1;                 // Default to zero_copy
            tx->evt.event.send.connection=NULL;
            tx->evt.event.send.context=NULL; // Clear context
            pthread_mutex_lock(&ep->lock);   // Return tx to list
            TAILQ_INSERT_HEAD( &gep->txs, tx, entry );
            pthread_mutex_unlock(&ep->lock);
            free_evt=0;
            goto event_next;

        case CCI_EVENT_CONNECT_REQUEST:
            free_evt=1;
            buffer=*((void **)&evt->event.request.data_ptr);
            free(buffer);

        case CCI_EVENT_NONE:
        case CCI_EVENT_CONNECT_TIMEDOUT:
        case CCI_EVENT_CONNECT_REJECTED:
        case CCI_EVENT_CONNECT_ACCEPTED:
        case CCI_EVENT_KEEPALIVE_TIMEDOUT:
        case CCI_EVENT_ENDPOINT_DEVICE_FAILED:
            free_evt=1;

            event_next:
            pthread_mutex_lock(&ep->lock);   // Get evt
            TAILQ_REMOVE( &ep->evts, evt, entry );
            pthread_mutex_unlock(&ep->lock);

            if(free_evt)                     // Do not free embedded evt
                free(evt);
            cRv=CCI_SUCCESS;
            break;
    }

    CCI_EXIT;
    return(cRv);
}


static inline int gni_tx_send(
    gni_tx_t *                  tx ) {

    cci_device_t const *        device;
    cci__dev_t *                dev;
    gni_dev_t *                 gdev;
    cci_connection_t *          connection;
    cci_endpoint_t *            endpoint;
    cci__ep_t *                 ep;
    gni_ep_t *                  gep;
    cci__conn_t *               conn;
    gni_conn_t *                gconn;
    uint64_t                    hdr[2];
    void *                      ptr;
    gni_return_t                gRv;

    CCI_ENTER;

    if(!gglobals) {

        CCI_EXIT;
        return(CCI_ENODEV);
    }
    device=*(gglobals->devices);
    dev=container_of( device, cci__dev_t, device );
    gdev=dev->priv;
    debug( CCI_DB_FUNC,
           "%8s.%5d In gni_tx_send()", gdev->nodename, gdev->inst_id );

    connection=tx->evt.event.send.connection;
    conn=container_of( connection, cci__conn_t, connection );
    gconn=conn->priv;

    endpoint=connection->endpoint;
    ep=container_of( endpoint, cci__ep_t, endpoint );
    gep=ep->priv;

    if(tx->zero_copy)                        // Use user ptr
        ptr=tx->user_ptr;
    else                                     // Use tx buffer
        ptr=tx->ptr;

    gRv=GNI_EpSetEventData( gconn->ep_hndl,
                            0,      
                            gep->id );  
    assert( gRv==GNI_RC_SUCCESS );

    hdr[0]=(uint64_t)gconn;
    hdr[1]=tx->len;
    debug( CCI_DB_INFO,
           "%8s.%5d %s: send_tx %lx %ld %lx %lx %lx",
           gdev->nodename, gdev->inst_id, __func__, hdr[0], hdr[1],
           *((uint64_t *)(ptr+0)), *((uint64_t *)(ptr+8)),
           *((uint64_t *)(ptr+16)) );
    gRv=GNI_SmsgSend(       gconn->ep_hndl,  // Target GNI endpoint
                            hdr,             // header
                            16,              // length of header
                            ptr,             // payload
                            tx->len,         // length of payload
                            tx->id );        // message ID
    gni_log_gni( __func__, "GNI_SmsgSend", gRv );
    if( gRv!=GNI_RC_SUCCESS )
        debug( CCI_DB_WARN, "%8s.%5d %s: %d", gdev->nodename,
               gdev->inst_id, __func__, gconn->credits );
    assert( gRv==GNI_RC_SUCCESS );

    pthread_mutex_lock(&ep->lock);           // Take tx, credit
    TAILQ_REMOVE( &gep->txs_queue, tx, qentry );
    gconn->credits--;                        // Consume credit
    pthread_mutex_unlock(&ep->lock);

    CCI_EXIT;
    return(CCI_SUCCESS);
}


static int gni_send(              cci_connection_t *     connection,
                                  void *                 ptr,
                                  uint32_t               len,
                                  void *                 context,
                                  int32_t                flags ) {

    cci_device_t const *        device;
    cci_endpoint_t *            endpoint=connection->endpoint;
    cci__ep_t *                 ep;
    cci__dev_t *                dev;
    gni_ep_t *                  gep;
    gni_dev_t *                 gdev;
    gni_tx_t *                  tx;
    cci__conn_t *               conn=container_of( connection,
                                                   cci__conn_t,
                                                   connection );
    gni_conn_t *                gconn=conn->priv;
    cci_status_t                cRv=CCI_SUCCESS;

    CCI_ENTER;

    if(!gglobals) {

        CCI_EXIT;
        return(CCI_ENODEV);
    }
    device=*(gglobals->devices);
    dev=container_of( device, cci__dev_t, device );
    gdev=dev->priv;
    debug( CCI_DB_FUNC,
           "%8s.%5d In gni_send()", gdev->nodename, gdev->inst_id );
    ep=container_of( endpoint, cci__ep_t, endpoint );
    gep=ep->priv;

    pthread_mutex_lock(&ep->lock);           // Get tx
    if( TAILQ_EMPTY(&gep->txs) ) {           // Bail if no tx left

        pthread_mutex_unlock(&ep->lock);     // fail tx
        cRv=CCI_ENOBUFS;
        goto FAIL;
    }

    while( !TAILQ_EMPTY(&gep->txs) ) {       // Get available tx

        tx=TAILQ_FIRST(&gep->txs);
        TAILQ_REMOVE( &gep->txs, tx, entry );
        break;
    }
    pthread_mutex_unlock(&ep->lock);         // end get

    tx->len=len;
    tx->evt.event.send.connection=connection;
    tx->evt.event.send.context=context;
    tx->user_ptr=ptr;

    pthread_mutex_lock(&ep->lock);           // Add tx bottom of queue

    if(gconn->credits) {                     // Able to use zero_copy

        TAILQ_INSERT_TAIL( &gep->txs_queue, tx, qentry );
        pthread_mutex_unlock(&ep->lock);     //  ... release "hot path"
        gni_tx_send(tx);                     // Use "hot path"

    } else {                                 // Have run out of credits;

        memcpy( tx->ptr, ptr, len );         // Must queue
        tx->zero_copy=0;                     // Flag tx buffer used
        TAILQ_INSERT_TAIL( &gep->txs_queue, tx, qentry );
        pthread_mutex_unlock(&ep->lock);     // Release for queued path
    }                                        // Note: disable "hot path"

    FAIL:
    CCI_EXIT;
    return(cRv);
}


static int gni_sendv(             cci_connection_t *     connection,
                                  struct iovec *         data,
                                  uint32_t               iovcnt,
                                  void *                 context,
                                  int32_t                flags ) {

    cci_device_t const *        device;
    cci__dev_t *                dev;
    gni_dev_t *                 gdev;

    CCI_ENTER;

    if(!gglobals) {

        CCI_EXIT;
        return(CCI_ENODEV);
    }
    device=*(gglobals->devices);
    dev=container_of( device, cci__dev_t, device );
    gdev=dev->priv;
    debug( CCI_DB_WARN,
           "%8s.%5d In gni_sendv()", gdev->nodename, gdev->inst_id );

    CCI_EXIT;
    return(CCI_ERR_NOT_IMPLEMENTED);
}


static int gni_rma_register(      cci_endpoint_t *       endpoint,
                                  cci_connection_t *     connection,
                                  void *                 start,
                                  uint64_t               length,
                                  uint64_t *             rma_handle ) {

    cci_device_t const *        device;
    cci__dev_t *                dev;
    gni_dev_t *                 gdev;

    CCI_ENTER;

    if(!gglobals) {

        CCI_EXIT;
        return(CCI_ENODEV);
    }
    device=*(gglobals->devices);
    dev=container_of( device, cci__dev_t, device );
    gdev=dev->priv;
    debug( CCI_DB_WARN,
           "%8s.%5d In gni_rma_register()",
           gdev->nodename, gdev->inst_id );

    CCI_EXIT;
    return(CCI_ERR_NOT_IMPLEMENTED);
}


static int gni_rma_deregister(    uint64_t               rma_handle ) {

    cci_device_t const *        device;
    cci__dev_t *                dev;
    gni_dev_t *                 gdev;

    CCI_ENTER;

    if(!gglobals) {

        CCI_EXIT;
        return(CCI_ENODEV);
    }
    device=*(gglobals->devices);
    dev=container_of( device, cci__dev_t, device );
    gdev=dev->priv;
    debug( CCI_DB_WARN,
           "%8s.%5d In gni_rma_deregister()",
           gdev->nodename, gdev->inst_id );

    CCI_EXIT;
    return(CCI_ERR_NOT_IMPLEMENTED);
}


static int gni_rma(               cci_connection_t *     connection,
                                  void *                 msg_ptr,
                                  uint32_t               msg_len,
                                  uint64_t               local_handle,
                                  uint64_t               local_offset,
                                  uint64_t               remote_handle,
                                  uint64_t               remote_offset,
                                  uint64_t               len,
                                  void *                 context,
                                  int32_t                flags ) {

    cci_device_t const *        device;
    cci__dev_t *                dev;
    gni_dev_t *                 gdev;

    CCI_ENTER;

    if(!gglobals) {

        CCI_EXIT;
        return(CCI_ENODEV);
    }
    device=*(gglobals->devices);
    dev=container_of( device, cci__dev_t, device );
    gdev=dev->priv;
    debug( CCI_DB_WARN,
           "%8s.%5d In gni_rma()", gdev->nodename, gdev->inst_id );

    CCI_EXIT;
    return(CCI_ERR_NOT_IMPLEMENTED);
}


static int gni_reap_recv(
    cci__dev_t *                dev ) {

    uint64_t *                  hdrs;
    cci__ep_t *                 ep;
    cci_endpoint_t *            endpoint;
    gni_ep_t *                  gep;
    gni_conn_t *                gconn;
    cci__conn_t *               conn;
    cci_connection_t *          connection;
    gni_cq_entry_t              cqe;
    gni_return_t                gRv;

    cci_status_t                cRv=CCI_SUCCESS;
    gni_dev_t *                 gdev=dev->priv;
    cci__evt_t *                evt=NULL;
    gni_rx_t *                  rx=NULL;

    CCI_ENTER;

    if(!gglobals) {

        CCI_EXIT;
        return(CCI_ENODEV);
    }

    debug( CCI_DB_FUNC, "%8s.%5d In gni_reap_recv()",
           gdev->nodename, gdev->inst_id );

    pthread_mutex_lock(&dev->lock);          // Get CCI endpoint
    ep=TAILQ_FIRST(&dev->eps);               // ### Fix multiple ep's
    pthread_mutex_unlock(&dev->lock);
    gep=ep->priv;

    TAILQ_FOREACH( gconn,                    // Search all connections
                   &gep->gconns,             // .. on this endpoint
                   entry ) {

        if( gconn->status!=GNI_CONN_ACCEPTED )
            continue;                        // Skip if not active

        gRv=GNI_CqGetEvent( gconn->dst_cq_hndl, &cqe );
        if( gRv!=GNI_RC_NOT_DONE ) {         // Something's shaking

            if( gRv==GNI_RC_SUCCESS ) {      // If CQ is OK...

                gRv=GNI_RC_NOT_DONE;
                while( gRv==GNI_RC_NOT_DONE )// Poll for message
                    gRv=GNI_SmsgGetNext( gconn->ep_hndl,
                                         (void **)&hdrs );
                if( gRv==GNI_RC_SUCCESS ) {  // If got message...

//                  Note: inst_id is sender's gep->id and hdrs[0] is
//                  sender's gconn.
                    debug( CCI_DB_INFO, "%8s.%5d %s: INST_ID=%lx",
                           gdev->nodename, gdev->inst_id, __func__,
                           GNI_CQ_GET_INST_ID(cqe) );

                    if(TAILQ_EMPTY(&gep->rxs))
                        return(CCI_ENOBUFS);

//                  Now, we need an rx structure to hold the message and
//                  post an event for get_event().
                    pthread_mutex_lock(&ep->lock);  // Get rx
                    while( !TAILQ_EMPTY(&gep->rxs) ) {

                        rx=TAILQ_FIRST(&gep->rxs);
                        TAILQ_REMOVE( &gep->rxs, rx, entry );
                        break;
                    }
                    pthread_mutex_unlock(&ep->lock);

                    evt=&rx->evt;            // Embedded event
                    conn=gconn->conn;
                    connection=&conn->connection;

                    *((uint32_t *)&evt->event.recv.len)=hdrs[1];
                    evt->event.recv.connection=connection;
                    memcpy( evt->event.recv.ptr, &hdrs[2], hdrs[1] );

                    gRv=GNI_SmsgRelease( gconn->ep_hndl );
                    assert( gRv==GNI_RC_SUCCESS );
                } else
                    gni_log_gni( __func__, "GNI_SmsgGetNext", gRv );

                debug( CCI_DB_INFO, "%8s.%5d %s: %lx %ld %lx %lx %lx",
                       gdev->nodename, gdev->inst_id, __func__, hdrs[0],
                       hdrs[1], hdrs[2], hdrs[3], hdrs[4] );
            } else
                gni_log_gni( __func__, "GNI_CqGetEvent", gRv );

//          If GNI_CqGetEvent or GNI_SmsgGetNext had problems...
//          or message connection did not match, create error event,
//          rather than modifiying recv event.
            if(!rx) {                        // Corrupts ptr address
                                             // if recv event is used
                evt=calloc( 1, sizeof(*evt) );
                endpoint=&ep->endpoint;
                evt->event.dev_failed.type=
                    CCI_EVENT_ENDPOINT_DEVICE_FAILED;
                evt->event.dev_failed.endpoint=endpoint;
                cRv=CCI_ERROR;
            }

            debug( CCI_DB_CONN, "%8s.%5d %s: posting event: %s",
                   gdev->nodename, gdev->inst_id, __func__,
                   gni_cci_event_to_str(evt->event.type) );
            pthread_mutex_lock(&ep->lock);   // Queue evt
            TAILQ_INSERT_TAIL( &ep->evts, evt, entry );
            pthread_mutex_unlock(&ep->lock);
        }
    }

    CCI_EXIT;
    return(cRv);
}


static void gni_reap_send(
    cci__dev_t *                dev ) {

    cci__ep_t *                 ep;
    cci_endpoint_t *            endpoint;
    gni_ep_t *                  gep;
    gni_cq_entry_t              cqe;
    gni_tx_t *                  tx;
    gni_conn_t *                gconn;
    gni_return_t                gRv;

    cci__evt_t *                evt=NULL;
    gni_dev_t *                 gdev=dev->priv;

    CCI_ENTER;

    if(!gglobals) {

        CCI_EXIT;
        return;
    }

    debug( CCI_DB_FUNC, "%8s.%5d In gni_reap_send()",
           gdev->nodename, gdev->inst_id );

    pthread_mutex_lock(&dev->lock);          // Get CCI endpoint
    ep=TAILQ_FIRST(&dev->eps);               // ### Fix multiple ep's
    pthread_mutex_unlock(&dev->lock);
    if(!ep) {

        CCI_EXIT;
        return;
    }
    gep=ep->priv;

//  Check each connection for a pending send request.
    TAILQ_FOREACH( gconn, &gep->gconns, entry ) {
    
        if( gconn->credits==GNI_MBOX_MAX_CREDIT )
            continue;                        // None pending on gconn

        gRv=GNI_CqGetEvent( gconn->src_cq_hndl, &cqe );
        if( gRv!=GNI_RC_NOT_DONE ) {         // Something's shaking

            if( gRv==GNI_RC_SUCCESS ) {      // match event ID to tx->id

                TAILQ_FOREACH( tx, &gep->txs_all, centry ) {

                    if( tx->id==GNI_CQ_GET_INST_ID(cqe) ) {

                        evt=&tx->evt;        // Got tx, evt
                        break;
                    }
                }
                debug( CCI_DB_INFO, "%8s.%5d %s: INST_ID=%lx",
                       gdev->nodename, gdev->inst_id, __func__,
                       GNI_CQ_GET_INST_ID(cqe) );
            }
            pthread_mutex_lock(&ep->lock);   // Restore credit
            gconn->credits++;                // Return credit
            pthread_mutex_unlock(&ep->lock); // 

            if(!(gconn->credits-1)) {        // "hot path" is disabled

//              With "hot path" disabled, there may be queued sends
//              waiting to be progressed.
                if(!TAILQ_EMPTY(&gep->txs_queue)) {

//                  Take entry from top of queue.
                    tx=TAILQ_FIRST(&gep->txs_queue);
                    gni_tx_send(tx);         // Initiate send
                }
//              Note that gni_tx_send() also decrements the credits and
//              removes the tx from the queue.  So, while the above
//              increment to the credits, re-enables the "hot path"; if
//              there is a queued tx, it just got re-disabled.
            }

            if(!evt) {

                evt=calloc( 1, sizeof(*evt) );
                gni_log_gni( __func__, "GNI_CqGetEvent", gRv );
                endpoint=&ep->endpoint;
                evt->event.dev_failed.type=
                    CCI_EVENT_ENDPOINT_DEVICE_FAILED;
                evt->event.dev_failed.endpoint=endpoint;
            }

            debug( CCI_DB_CONN, "%8s.%5d %s: posting event: %s",
                   gdev->nodename, gdev->inst_id, __func__,
                   gni_cci_event_to_str(evt->event.type) );
            pthread_mutex_lock(&ep->lock);   // Queue evt
            TAILQ_INSERT_TAIL( &ep->evts, evt, entry );
            pthread_mutex_unlock(&ep->lock);
        }
    }

    CCI_EXIT;
    return;
}


static void gni_progress_connection_request(
    cci__dev_t *                dev ) {

    int                         iRv;
    uint32_t                    len;
    cci__ep_t *                 ep;
    gni_ep_t *                  gep;
    gni_conn_t *                gconn;
    cci__conn_t *               conn;
    cci_connection_t *          connection;
    cci__evt_t *                evt;

    uint32_t                    sz=sizeof(gni_mailbox_t);
    int                         sd=-1;
    gni_dev_t *                 gdev=dev->priv;
    socklen_t                   is=sizeof(struct sockaddr_in);

    CCI_ENTER;

    if(!gglobals) {

        CCI_EXIT;
        return;
    }

    debug( CCI_DB_FUNC,
           "%8s.%5d In gni_progress_connection_request()",
           gdev->nodename, gdev->inst_id );

    pthread_mutex_lock(&dev->lock);          // Get CCI endpoint
    ep=TAILQ_FIRST(&dev->eps);               // ### Fix multiple ep's
    pthread_mutex_unlock(&dev->lock);

    if(!ep)
        goto FAIL;
    gep=ep->priv;

//  Search all connections on this endpoint.
    TAILQ_FOREACH( gconn, &gep->gconns, entry ) {

        if( gconn->status!=GNI_CONN_PENDING_REPLY )
            continue;                        // Ignore unless pending
                                             // .. reply
        len=gconn->data_len;                 // Optional payload length
        conn=gconn->conn;
        connection=&conn->connection;

        evt=calloc( 1, sizeof(*evt) );       // Create CCI event
        evt->ep=ep;
        evt->event.type=CCI_EVENT_ENDPOINT_DEVICE_FAILED;

        sd=socket( AF_INET, SOCK_STREAM, 0 );// Try to create socket
        if( sd==-1 ) {                       // .. failed

            gni_log_sys( __func__, "socket" );
            goto FAIL;
        }

        iRv=connect( sd,                     // Attempt connection
                     (const struct sockaddr *)&gconn->sin, is );
        if( iRv==-1 ) {

            gni_log_sys( __func__, "connect" );
            goto FAIL;
        }

        debug( CCI_DB_CONN, "%8s.%5d %s: gconn=%zp", 
               gdev->nodename, gdev->inst_id, __func__, gconn );

        if( sz!=send( sd, &(gconn->src_box), sz, 0 ) ) {

            gni_log_sys( __func__, "send" );
            goto FAIL;
        }

        if(len)                              // Optional payload
            if( send( sd, gconn->data_ptr, len, 0 )!=len ) {

                gni_log_sys( __func__, "send" );
                goto FAIL;
            }

        debug( CCI_DB_CONN, "%8s.%5d %s: mailbox=%d  optional=%d", 
               gdev->nodename, gdev->inst_id, __func__, sz, len );

//      Receive remote mailbox structure.
        if( recv( sd, &(gconn->dst_box), sz, MSG_WAITALL )!=sz ) {

            gni_log_sys( __func__, "recv" );
            goto FAIL;
        }
        debug( CCI_DB_CONN,                  // Contents of reply
               "%8s.%5d %s: recv=%d 0x%.8x 0x%.4x %d %zp %d %x %x"
               " %d %d %d %s %zp %s",
               gdev->nodename, gdev->inst_id, __func__, sz,
               gconn->dst_box.nic_addr,
               gconn->dst_box.inst_id,
               gconn->dst_box.attr.msg_type,
               gconn->dst_box.attr.msg_buffer,
               gconn->dst_box.attr.buff_size,
               gconn->dst_box.attr.mem_hndl,
               gconn->dst_box.attr.mbox_offset,
               gconn->dst_box.attr.mbox_maxcredit,
               gconn->dst_box.attr.msg_maxsize,
               gni_cci_attribute_to_str(gconn->dst_box.cci_attr),
               gconn->dst_box.gconn,
               gni_conn_status_to_str(gconn->dst_box.info.reply) );

//      Update status so that we do not retry.
        gconn->status=gconn->dst_box.info.reply;

        if( gconn->status==GNI_CONN_ACCEPTED ) {

            gni_initialize_smsg(conn);
            debug( CCI_DB_CONN, "%8s.%5d %s: created SMSG",
                   gdev->nodename, gdev->inst_id, __func__ );

            evt->event.type=CCI_EVENT_CONNECT_ACCEPTED;
            evt->event.accepted.connection=connection;
        } else
            evt->event.type=CCI_EVENT_CONNECT_REJECTED;

        FAIL:
        if( gconn->status==GNI_CONN_PENDING_REPLY )
            gconn->status=GNI_CONN_FAILED;

        if( sd!=-1 )                         // Finished with socket
            close(sd);

//      Queue event...connection complete.
        debug( CCI_DB_CONN, "%8s.%5d %s: posting event: %s",
               gdev->nodename, gdev->inst_id, __func__,
               gni_cci_event_to_str(evt->event.type) );
        pthread_mutex_lock(&ep->lock);       // Queue evt
        TAILQ_INSERT_TAIL( &ep->evts, evt, entry );
        pthread_mutex_unlock(&ep->lock);
    }

    CCI_EXIT;
    return;
}


static void gni_progress_connection_reply(
    cci__dev_t *                dev ) {

    uint32_t                    len;
//  cci_endpoint_t *            endpoint;
    cci__ep_t *                 ep;
    gni_ep_t *                  gep;
//  cci__conn_t *               conn;
//  gni_return_t                gRv;
//  cci_connection_t *          connection;
//  uint32_t                    Size;         // Size SMSG mailbox

    int                         sd=-1;
    uint32_t                    sz=sizeof(gni_mailbox_t);
    gni_dev_t *                 gdev=dev->priv;
//  void *                      buffer=NULL;
//  gni_conn_t *                gconn=NULL;
    gni_mailbox_t *             dst_box;
    void *                      data_ptr;
    cci__evt_t *                evt=NULL;

    CCI_ENTER;

    if(!gglobals)
        goto FAIL;

    debug( CCI_DB_FUNC,
           "%8s.%5d In gni_progress_connection_reply()",
           gdev->nodename, gdev->inst_id );

//  Check for connection request.
    if( (sd=accept( gdev->sd, NULL, NULL ))==-1 ) {

        if( errno!=EAGAIN )                  // OK to not have a request
            gni_log_sys( __func__, "accept" );
        goto FAIL;
    }

//  Get CCI endpoint.
    pthread_mutex_lock(&dev->lock);          // Get ep
    ep=TAILQ_FIRST(&dev->eps);               // ### Fix multiple ep's
    pthread_mutex_unlock(&dev->lock);
    if(!ep) {

        goto FAIL;
    }

    dst_box=calloc( 1, sizeof(*dst_box) );
    if( sz!=recv( sd, dst_box, sz, MSG_WAITALL ) ) {

        gni_log_sys( __func__, "recv" );
        goto FAIL;
    }

    len=dst_box->info.length;
    data_ptr=calloc( 1, len+1 );
    if( len!=recv( sd, data_ptr, len, MSG_WAITALL ) ) {

        gni_log_sys( __func__, "recv" );
        goto FAIL;
    }

    evt=calloc( 1, sizeof(*evt) );
    evt->ep=ep;
    evt->event.request.type=CCI_EVENT_CONNECT_REQUEST;
    evt->event.request.data_len=dst_box->info.length;
    evt->event.request.data_ptr=data_ptr;
    evt->event.request.attribute=dst_box->cci_attr;
    gep=ep->priv;

    debug( CCI_DB_CONN, "%8s.%5d %s: posting event: %s",
           gdev->nodename, gdev->inst_id, __func__,
           gni_cci_event_to_str(evt->event.type) );
    pthread_mutex_lock(&ep->lock);           // Set dst_box, sd
    gep->dst_box=dst_box;                    // Temporary
    gep->sd=sd;
    TAILQ_INSERT_TAIL( &ep->evts, evt, entry );
    pthread_mutex_unlock(&ep->lock);         //

    CCI_EXIT;
    return;

    FAIL:
    if( sd!=-1 )                             // socket is open
        close(sd);

    CCI_EXIT;
    return;
}


static void gni_progress_dev(
    cci__dev_t *                dev ) {

    CCI_ENTER;

    if(!gglobals)
        goto FAIL;

    gni_progress_connection_request(dev);
    gni_progress_connection_reply(dev);

    FAIL:
    CCI_EXIT;
    return;
}

static void *gni_progress_thread(
    void *                      arg ) {

    CCI_ENTER;

    while(!gni_shut_down) {

        cci__dev_t *            dev;
        cci_device_t const **   device;

        /* for each device, try progressing */
        for( device=gglobals->devices; *device!=NULL; device++ ) {

            dev=container_of( *device, cci__dev_t, device );
            gni_progress_dev(dev);
        }
        usleep(GNI_PROG_TIME_US);
    }
    pthread_exit(NULL);

    CCI_EXIT;
    return(NULL);
}
