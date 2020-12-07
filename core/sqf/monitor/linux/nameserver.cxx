///////////////////////////////////////////////////////////////////////////////
//
// @@@ START COPYRIGHT @@@
//
// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
//
// @@@ END COPYRIGHT @@@
//
///////////////////////////////////////////////////////////////////////////////

#include <iostream>

using namespace std;

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/file.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>

#include "trafconf/trafconfig.h"
#include "lnode.h"
#include "pnode.h"
#include "nameserver.h"
#include "monlogging.h"
#include "montrace.h"
#include "nameserverconfig.h"
#include "meas.h"
#include "reqqueue.h"

extern CMonitor *Monitor;
extern CNode *MyNode;
extern CProcess *NameServerProcess;
extern CNodeContainer *Nodes;
extern CReqQueue ReqQueue;
extern bool IsRealCluster;
extern int MyPNID;
extern CNameServerConfigContainer *NameServerConfig;
extern CMeas Meas;

#define NAMESERVER_IO_RETRIES 3

CNameServer::CNameServer( void )
           : nsStartupComplete_(false)
           , shutdown_(false)
           , ioWaitTimeout_(EPOLL_IO_WAIT_TIMEOUT_MSEC)
           , ioRetryCount_(EPOLL_IO_RETRY_COUNT)
           , mon2nsSock_(-1)
           , seqNum_(0)
{
    const char method_name[] = "CNameServer::CNameServer";
    TRACE_ENTRY;

    // Use the EPOLL timeout and retry values
    char *ioWaitTimeoutEnv = getenv( "SQ_MON_EPOLL_WAIT_TIMEOUT" );
    if ( ioWaitTimeoutEnv )
    {
        // Timeout in seconds
        ioWaitTimeout_ = atoi( ioWaitTimeoutEnv );
        char *ioRetryCountEnv = getenv( "SQ_MON_EPOLL_RETRY_COUNT" );
        if ( ioRetryCountEnv )
        {
            ioRetryCount_ = atoi( ioRetryCountEnv );
        }
        if ( ioRetryCount_ > EPOLL_IO_RETRY_COUNT_MAX )
        {
            ioRetryCount_ = EPOLL_IO_RETRY_COUNT_MAX;
        }
    }

    mon2nsHost_[0] = '\0';
    mon2nsPort_[0] = '\0';

    TRACE_EXIT;
}

CNameServer::~CNameServer( void )
{
    const char method_name[] = "CNameServer::~CNameServer";
    TRACE_ENTRY;

    TRACE_EXIT;
}

int CNameServer::ChooseNextNs( void )
{
    const char method_name[] = "CNameServer::ChooseNextNs";
    TRACE_ENTRY;

    static unsigned int seed = 1;
    static bool fix_seed = true;
    if ( fix_seed )
    {
        seed += MyNode->GetPNid();
        fix_seed = false;
    }
    int cnt = NameServerConfig->GetCount();
    int rnd = (int) ((float) (cnt) * (rand_r(&seed) / (RAND_MAX + 1.0)));
    CNameServerConfig *config = NameServerConfig->GetFirstConfig();
    for ( int i = 0; i < rnd; i++)
    {
        config = config->GetNext();
    }
    CNode *node = Nodes->GetNode( (char*) config->GetName() );
    if (node && node->GetState() == State_Up)
    {
        strcpy( mon2nsHost_, config->GetName() );
        if ( trace_settings & TRACE_NS )
        {
            trace_printf( "%s@%d - nameserver=%s, rnd=%d, cnt=%d\n"
                        , method_name, __LINE__
                        , mon2nsHost_
                        , rnd
                        , cnt );
        }
    }
    else
    {
        config = config->GetNext()?config->GetNext():NameServerConfig->GetFirstConfig();
        while (config)
        {
            node = Nodes->GetNode( (char*) config->GetName() );
            if (node && node->GetState() != State_Up)
            {
                config = config->GetNext();
                continue;
            }
            
            strcpy( mon2nsHost_, config->GetName() );
            if ( trace_settings & TRACE_NS )
            {
                trace_printf( "%s@%d - selected alternate nameserver=%s\n"
                            , method_name, __LINE__
                            , mon2nsHost_ );
            }
            break;
        }
    }

    if (strlen(mon2nsHost_) == 0)
    {
        char la_buf[MON_STRING_BUF_SIZE];
        sprintf( la_buf
               , "[%s], No Name Server nodes available.\n"
                 "Scheduling shutdown (abrupt)!\n"
               , method_name );
        mon_log_write(NAMESERVER_CHOOSENEXTNS_1, SQ_LOG_CRIT, la_buf ); 
        ReqQueue.enqueueShutdownReq( ShutdownLevel_Abrupt );

        TRACE_EXIT;
        return( -2 );
    }

    TRACE_EXIT;
    return( 0 );
}

int CNameServer::ConnectToNs( bool *retry )
{
    const char method_name[] = "CNameServer::ConnectToNs";
    TRACE_ENTRY;

    int err = 0;

reconnect:

    if ( !mon2nsPort_[0] )
    {
        err = GetM2NPort( -1 );
    }
    if ( err == 0 && !mon2nsHost_[0] )
    {
        err = ChooseNextNs();
    }

    int sock = 0;

    if ( shutdown_ )
    {
        err = -1;
    }

    if ( err == 0 )
    {
        char mon2nsPort[MAX_PROCESSOR_NAME+MAX_PROCESSOR_NAME];

        //memset( &mon2nsPort, 0, sizeof(mon2nsPort) );
        mon2nsPort[0] = 0;
        sprintf( mon2nsPort,"%s:%s", mon2nsHost_, mon2nsPort_ );

        if (trace_settings & (TRACE_INIT | TRACE_RECOVERY))
        {
            trace_printf( "%s@%d - Connecting to %s\n"
                        , method_name, __LINE__
                        , mon2nsPort );
        }

        sock = CComm::Connect( mon2nsPort );
        if ( sock < 0 )
        {
            err = sock;
            goto reconnect;
        }
    }
    if ( err == 0 )
    {
        mon2nsSock_ = sock;
        if ( trace_settings & TRACE_NS )
        {
            trace_printf( "%s@%d - connected to nameserver=%s:%s, sock=%d\n"
                        , method_name, __LINE__
                        , mon2nsHost_
                        , mon2nsPort_
                        , mon2nsSock_ );
        }
    }

    if ( err == 0)
    {
        nodeId_t nodeId;
        strcpy( nodeId.nodeName, MyNode->GetName() );
        strcpy( nodeId.commPort, MyNode->GetCommPort() );
        strcpy( nodeId.syncPort, MyNode->GetSyncPort() );
        nodeId.pnid = MyNode->GetPNid();
        nodeId.creatorPNid = -1;
        nodeId.creatorShellPid = -1;
        nodeId.creatorShellVerifier = -1;
        nodeId.creator = false;
        nodeId.ping = false;
        nodeId.nsPid = -1;
        nodeId.nsPNid = -1;
        if ( trace_settings & TRACE_NS )
        {
            trace_printf( "%s@%d - sending node-info to nameserver=%s:%s, sock=%d\n"
                          "        nodeId.nodeName=%s\n"
                          "        nodeId.commPort=%s\n"
                          "        nodeId.syncPort=%s\n"
                          "        nodeId.creatorPNid=%d\n"
                          "        nodeId.creator=%d\n"
                          "        nodeId.creatorShellPid=%d\n"
                          "        nodeId.creatorShellVerifier=%d\n"
                          "        nodeId.ping=%d\n"
                        , method_name, __LINE__
                        , mon2nsHost_
                        , mon2nsPort_
                        , mon2nsSock_
                        , nodeId.nodeName
                        , nodeId.commPort
                        , nodeId.syncPort
                        , nodeId.creatorPNid
                        , nodeId.creator
                        , nodeId.creatorShellPid
                        , nodeId.creatorShellVerifier
                        , nodeId.ping );
        }
        err = CComm::Send( mon2nsSock_
                         , (char *) &nodeId
                         , sizeof(nodeId)
                         , mon2nsHost_
                         , method_name );
        if (err == 0)
        {
            if ( trace_settings & TRACE_NS )
            {
                trace_printf( "%s@%d - OK send to nameserver=%s:%s, sock=%d, error=%d, waiting receive\n"
                            , method_name, __LINE__
                            , mon2nsHost_
                            , mon2nsPort_
                            , mon2nsSock_
                            , err );
            }
            err = CComm::Receive( mon2nsSock_
                                , (char *) &nodeId
                                , sizeof(nodeId)
                                , mon2nsHost_
                                , method_name );
            if ( err )
            {
                if ( trace_settings & TRACE_NS )
                {
                    trace_printf( "%s@%d - error receiving from nameserver=%s:%s, sock=%d, error=%d\n"
                                , method_name, __LINE__
                                , mon2nsHost_
                                , mon2nsPort_
                                , mon2nsSock_
                                , err );
                }
            }
            else
            {
                if ( trace_settings & TRACE_NS )
                {
                    trace_printf( "%s@%d - Received nodeId back\n"
                                  "        nodeId.nodeName=%s\n"
                                  "        nodeId.commPort=%s\n"
                                  "        nodeId.syncPort=%s\n"
                                  "        nodeId.pnid=%d\n"
                                  "        nodeId.nsPid=%d\n"
                                  "        nodeId.nsPNid=%d\n"
                                , method_name, __LINE__
                                , nodeId.nodeName
                                , nodeId.commPort
                                , nodeId.syncPort
                                , nodeId.pnid
                                , nodeId.nsPid
                                , nodeId.nsPNid );
                }
                if ( !nsStartupComplete_ )
                {
                    nsStartupComplete_ = true;
                    if ( NameServerProcess )
                        NameServerProcess->CompleteProcessStartup( (char *) ""
                                                                 , nodeId.nsPid
                                                                 , false
                                                                 , false
                                                                 , false
                                                                 , NULL
                                                                 , -1 );
                }
                if ( nodeId.nsPNid >= 0 )
                {
                    *retry = true;
                    if ( IsRealCluster )
                    {
                        CNode *node = Nodes->GetNode( nodeId.nsPNid );
                        if (node && node->GetState() == State_Up)
                        {
                            strcpy( mon2nsHost_, node->GetName() );
                            GetM2NPort( nodeId.nsPNid );
                        }
                    }
                    else
                    {
                        gethostname( mon2nsHost_, MAX_PROCESSOR_NAME);
                        GetM2NPort( -1 );
                    }
                    CloseNs();
                }
            }
        }
    }

    TRACE_EXIT;
    return err;
}

int CNameServer::GetM2NPort( int nsPNid )
{
    const char method_name[] = "CNameServer::GetM2NPort";
    TRACE_ENTRY;

    bool done = false;
    int port;
    char *p = getenv( "NS_M2N_COMM_PORT" );
    if ( p )
    {
        port = atoi(p);
    }
    else
    {
        port = 0;
    }
    if ( !IsRealCluster )
    {
        // choose initial port
        int nsMax = NameServerConfig->GetCount();
        int candidatePNid = nsPNid < 0 ? MyPNID : nsPNid;
        int chosenPNid = 
                candidatePNid < nsMax ? candidatePNid : candidatePNid%nsMax;
        int lastChosenPNid = chosenPNid;
        while (!done)
        {
            // check that corresponding node is UP
            // node is up, chosen is good to go
            // not up,
            //   round-robin on other name server nodes and chose 1st up node
            //   no name server nodes available
            //      log event and down my node (MyPNID)
            CNode *node = Nodes->GetNode( chosenPNid );
            if (node && node->GetState() == State_Up)
            {
                port += chosenPNid;
        
                if ( trace_settings & TRACE_NS )
                {
                    trace_printf( "%s@%d - nsMax=%d, nsPNid=%d, MyPNID=%d, "
                                  "candidatePNid=%d, chosenPNid=%d, port=%d\n"
                                , method_name, __LINE__
                                , nsMax
                                , nsPNid
                                , MyPNID
                                , candidatePNid
                                , chosenPNid
                                , port );
                }
                done = true;
            }
            else
            {
                chosenPNid = (chosenPNid+1) < nsMax ? (chosenPNid+1) : 0;
                if (chosenPNid == lastChosenPNid)
                {
                    char la_buf[MON_STRING_BUF_SIZE];
                    sprintf( la_buf
                           , "[%s], No Name Server nodes available, "
                             "chosenPNid=%d, lastChosenPNid=%d.\n"
                             "Scheduling shutdown (abrupt)!\n"
                           , method_name
                           , chosenPNid, lastChosenPNid );
                    mon_log_write(NAMESERVER_GETM2NPORT_1, SQ_LOG_CRIT, la_buf ); 
                    ReqQueue.enqueueShutdownReq( ShutdownLevel_Abrupt );
                    done = true;
                }
                port += chosenPNid;
                TRACE_EXIT;
                return( -2 );
            }
        }
    }
    sprintf( mon2nsPort_, "%d", port );

    TRACE_EXIT;
    return( 0 );
}

bool CNameServer::IsNameServerConfigured( int pnid )
{
    const char method_name[] = "CNameServer::IsNameServerConfigured";
    TRACE_ENTRY;

    bool rs = false;    

    if ( IsRealCluster )
    {
        CNameServerConfig *config;
        CNode *node = Nodes->GetNode( pnid );
        if ( node )
        {
            config = NameServerConfig->GetConfig( node->GetName() );
            if ( config )
            {
                rs = true;
            }
        }
    }
    else
    {
        rs = pnid < NameServerConfig->GetCount() ? true : false;
    }

    if (trace_settings & (TRACE_INIT | TRACE_RECOVERY))
    {
        trace_printf( "%s@%d - pnid=%d, configured=%s\n"
                    , method_name, __LINE__, pnid, rs?"True":"False" );
    }

    TRACE_EXIT;
    return(rs);
}

void CNameServer::NameServerExited( void )
{
    const char method_name[] = "CNameServer::NameServerExited";
    TRACE_ENTRY;

    mon2nsHost_[0] = '\0';
    mon2nsPort_[0] = '\0';
    nsStartupComplete_ = false;
    CloseNs();

    TRACE_EXIT;
}

int CNameServer::NameServerStop( struct message_def* msg )
{
    const char method_name[] = "CNameServer::NameServerStop";
    TRACE_ENTRY;

    if ( trace_settings & TRACE_NS )
    {
        trace_printf( "%s@%d - sending nameserver-stop request to nameserver=%s:%s\n"
                    , method_name, __LINE__
                    , mon2nsHost_, mon2nsPort_ );
    }

    int error = ProcessRequest( msg );

    TRACE_EXIT;
    return error;
}

int CNameServer::ProcessDelete(CProcess* process )
{
    const char method_name[] = "CNameServer::ProcessDelete";
    TRACE_ENTRY;

    struct message_def msg;
    memset(&msg, 0, sizeof(msg) ); // TODO: remove!
    msg.type = MsgType_Service;
    msg.noreply = false;
    msg.reply_tag = seqNum_++;
    msg.u.request.type = ReqType_DelProcessNs;
    struct DelProcessNs_def *msgdel = &msg.u.request.u.del_process_ns;
    msgdel->nid = process->GetNid();
    msgdel->pid = process->GetPid();
    msgdel->verifier = process->GetVerifier();
    strcpy( msgdel->process_name, process->GetName() );
    msgdel->target_nid = msgdel->nid;
    msgdel->target_pid = msgdel->pid;
    msgdel->target_verifier = msgdel->verifier;
    strcpy( msgdel->target_process_name, msgdel->process_name );
    msgdel->target_abended = process->IsAbended();

    if ( trace_settings & TRACE_NS )
    {
        trace_printf( "%s@%d - sending del_process_ns request to nameserver=%s:%s\n"
                      "        msg.del_process_ns.process_name=%s (%d,%d:%d)\n"
                    , method_name, __LINE__
                    , mon2nsHost_, mon2nsPort_
                    , msgdel->process_name
                    , msgdel->nid
                    , msgdel->pid
                    , msgdel->verifier );
    }

    int error = ProcessRequest( &msg );

    TRACE_EXIT;
    return error;
}

int CNameServer::ProcessInfo( struct message_def* msg )
{
    const char method_name[] = "CNameServer::ProcessInfo";
    TRACE_ENTRY;

    if ( trace_settings & TRACE_NS )
    {
        trace_printf( "%s@%d - sending process-info request to nameserver=%s:%s\n"
                    , method_name, __LINE__
                    , mon2nsHost_, mon2nsPort_ );
    }

    int error = ProcessRequest( msg );

    TRACE_EXIT;
    return error;
}

int CNameServer::ProcessInfoCont( struct message_def* msg )
{
    const char method_name[] = "CNameServer::ProcessInfoCont";
    TRACE_ENTRY;

    if ( trace_settings & TRACE_NS )
    {
        trace_printf( "%s@%d - sending process-info-continue request to nameserver=%s:%s\n"
                    , method_name, __LINE__
                    , mon2nsHost_, mon2nsPort_ );
    }

    int error = ProcessRequest( msg );

    TRACE_EXIT;
    return error;
}

int CNameServer::ProcessInfoNs( struct message_def* msg )
{
    const char method_name[] = "CNameServer::ProcessInfoNs";
    TRACE_ENTRY;

    if ( trace_settings & TRACE_NS )
    {
        trace_printf( "%s@%d - sending process-info-ns request to nameserver=%s:%s\n"
                    , method_name, __LINE__
                    , mon2nsHost_, mon2nsPort_ );
    }

    int error = ProcessRequest( msg );

    TRACE_EXIT;
    return error;
}

int CNameServer::ProcessNew(CProcess* process )
{
    const char method_name[] = "CNameServer::ProcessNew";
    TRACE_ENTRY;

    struct message_def msg;
    memset(&msg, 0, sizeof(msg) ); // TODO: remove!
    msg.type = MsgType_Service;
    msg.noreply = false;
    msg.reply_tag = seqNum_++;
    msg.u.request.type = ReqType_NewProcessNs;
    struct NewProcessNs_def *msgnew = &msg.u.request.u.new_process_ns;
    CProcess* parent = process->GetParent();
    if ( parent )
    {
        msgnew->parent_nid = parent->GetNid();
        msgnew->parent_pid = parent->GetPid();
        msgnew->parent_verifier = parent->GetVerifier();
    }
    else
    {
        msgnew->parent_nid = -1;
        msgnew->parent_pid = -1;
        msgnew->parent_verifier = -1;
    }
    msgnew->pair_parent_nid = process-> GetPairParentNid();
    msgnew->pair_parent_pid = process-> GetPairParentPid();
    msgnew->pair_parent_verifier = process-> GetPairParentVerifier();
    msgnew->nid = process->GetNid();
    msgnew->pid = process->GetPid();
    msgnew->verifier = process->GetVerifier();
    msgnew->type = process->GetType();
    msgnew->priority = process->GetPriority();
    msgnew->backup = process->IsBackup();
    msgnew->unhooked = process->IsUnhooked();
    msgnew->event_messages = process->IsEventMessages();
    msgnew->system_messages = process->IsSystemMessages();
    strcpy( msgnew->path, process->path() );
    strcpy( msgnew->ldpath, process->ldpath() );
    strcpy( msgnew->program, process->program() );
    strcpy( msgnew->process_name, process->GetName() );
    strcpy( msgnew->port_name, process->GetPort() );
    msgnew->argc = process->argc();
    process->getUserArgs(msgnew->argv);
    strcpy( msgnew->infile, process->infile() );
    strcpy( msgnew->outfile, process->outfile() );
    msgnew->creation_time = process->GetCreationTime();

    if ( trace_settings & ( TRACE_NS | TRACE_REQUEST) )
    {
        trace_printf( "%s@%d - sending new_process_ns request to nameserver=%s:%s\n"
                      "        msg.new_process_ns.nid=%d\n"
                      "        msg.new_process_ns.pid=%d\n"
                      "        msg.new_process_ns.verifier=%d\n"
                      "        msg.new_process_ns.process_name=%s\n"
                      "        msg.new_process_ns.type=%d\n"
                      "        msg.new_process_ns.parent_nid=%d\n"
                      "        msg.new_process_ns.parent_pid=%d\n"
                      "        msg.new_process_ns.parent_verifier=%d\n"
                      "        msg.new_process_ns.pair_parent_nid=%d\n"
                      "        msg.new_process_ns.pair_parent_pid=%d\n"
                      "        msg.new_process_ns.pair_parent_verifier=%d\n"
                      "        msg.new_process_ns.priority=%d\n"
                      "        msg.new_process_ns.backup=%d\n"
                      "        msg.new_process_ns.unhooked=%d\n"
                      "        msg.new_process_ns.event_messages=%d\n"
                      "        msg.new_process_ns.system_messages=%d\n"
                      "        msg.new_process_ns.path=%s\n"
                      "        msg.new_process_ns.ldpath=%s\n"
                      "        msg.new_process_ns.program=%s\n"
                      "        msg.new_process_ns.port=%s\n"
                      "        msg.new_process_ns.infile=%s\n"
                      "        msg.new_process_ns.outfile=%s\n"
                      "        msg.new_process_ns.creation_time=%ld(secs):%ld(nsecs)\n"
                    , method_name, __LINE__
                    , mon2nsHost_, mon2nsPort_ 
                    , msgnew->nid
                    , msgnew->pid
                    , msgnew->verifier
                    , msgnew->process_name
                    , msgnew->type
                    , msgnew->parent_nid
                    , msgnew->parent_pid
                    , msgnew->parent_verifier
                    , msgnew->pair_parent_nid
                    , msgnew->pair_parent_pid
                    , msgnew->pair_parent_verifier
                    , msgnew->priority
                    , msgnew->backup
                    , msgnew->unhooked
                    , msgnew->event_messages
                    , msgnew->system_messages
                    , msgnew->path
                    , msgnew->ldpath
                    , msgnew->program
                    , msgnew->port_name
                    , msgnew->infile
                    , msgnew->outfile
                    , msgnew->creation_time.tv_sec
                    , msgnew->creation_time.tv_nsec
                    );
        trace_printf("%s@%d - msg.new_process_ns.argc=%d\n"
                    , method_name, __LINE__
                    , msgnew->argc );
        for (int i=0; i < msgnew->argc; i++)
        {
            trace_printf("%s@%d - msg.new_process_ns.argv[%d]=%s\n"
                        , method_name, __LINE__
                        , i, msgnew->argv[i]);
        }
    }

    int error = ProcessRequest( &msg );

    TRACE_EXIT;
    return error;
}

int CNameServer::ProcessNodeDown( int nid, char *nodeName )
{
    const char method_name[] = "CNameServer::ProcessNodeDown";
    TRACE_ENTRY;

    int error = 0;
    CProcess *process = MyNode->GetProcessByType( ProcessType_NameServer );
    if (process)
    {
        struct message_def msg;
        memset(&msg, 0, sizeof(msg) ); // TODO: remove!
        msg.type = MsgType_Service;
        msg.noreply = false;
        msg.reply_tag = seqNum_++;
        msg.u.request.type = ReqType_NodeDown;
        struct NodeDown_def *msgdown = &msg.u.request.u.down;
        msgdown->nid = nid;
        strcpy( msgdown->node_name, nodeName );
        msgdown->takeover = 0;
        msgdown->reason[0] = 0;
    
        if ( trace_settings & TRACE_NS )
        {
            trace_printf( "%s@%d - sending node-down request to nameserver=%s:%s\n"
                          "        msg.down.nid=%d\n"
                          "        msg.down.node_name=%s\n"
                        , method_name, __LINE__
                        , mon2nsHost_, mon2nsPort_ 
                        , msgdown->nid
                        , msgdown->node_name );
        }

        error = ProcessRequest( &msg );
    }

    TRACE_EXIT;
    return error;
}

int CNameServer::ProcessShutdown( void )
{
    const char method_name[] = "CNameServer::ProcessShutdown";
    TRACE_ENTRY;

    struct message_def msg;
    memset(&msg, 0, sizeof(msg) ); // TODO: remove!
    msg.type = MsgType_Service;
    msg.noreply = false;
    msg.reply_tag = seqNum_++;
    msg.u.request.type = ReqType_ShutdownNs;
    struct ShutdownNs_def *msgshutdown = &msg.u.request.u.shutdown_ns;
    msgshutdown->nid = -1;
    msgshutdown->pid = -1;
    msgshutdown->level = ShutdownLevel_Normal;

    if ( trace_settings & TRACE_NS )
    {
        trace_printf( "%s@%d - sending shutdown_ns request to nameserver=%s:%s\n"
                      "        msg.shutdown_ns.level=%d\n"
                    , method_name, __LINE__
                    , mon2nsHost_, mon2nsPort_
                    , msgshutdown->level );
    }

    int error = ProcessRequest( &msg );

    if ( error == 0 )
    {
        SetShutdown( true );
    }

    TRACE_EXIT;
    return error;
}

int CNameServer::ProcessRequest( struct message_def* msg )
{
    const char method_name[] = "CNameServer::ProcessRequest";
    TRACE_ENTRY;

    int retryCount = 0;
    char desc[256];
    char* descp;
    struct DelProcessNs_def* msgdel;
    struct NameServerStart_def* msgstart;
    struct NameServerStop_def* msgstop;
    struct NewProcessNs_def* msgnew;
    struct NodeDown_def* msgdown;
    struct ProcessInfo_def* msginfo;
    struct ShutdownNs_def* msgshutdown;
    struct message_def msg_reply;
    struct message_def* pmsg_reply = &msg_reply;

    descp = desc;
    int size = offsetof(struct message_def, u.request.u);
    switch ( msg->u.request.type )
    {
    case ReqType_DelProcessNs:
        msgdel = &msg->u.request.u.del_process_ns;
        sprintf( desc, "delete-process (nid=%d, pid=%d, verifier=%d, name=%s)",
                 msgdel->nid, msgdel->pid, msgdel->verifier, msgdel->process_name );
        size += sizeof(msg->u.request.u.del_process_ns);
        break;
    case ReqType_NameServerStart:
        msgstart = &msg->u.request.u.nameserver_start;
        sprintf( desc, "start-nameserver (nid=%d, pid=%d, node=%s)",
                 msgstart->nid, msgstart->pid, msgstart->node_name );
        size += sizeof(msg->u.request.u.nameserver_start);
        break;
    case ReqType_NameServerStop:
        msgstop = &msg->u.request.u.nameserver_stop;
        sprintf( desc, "stop-nameserver (nid=%d, pid=%d, node=%s)",
                 msgstop->nid, msgstop->pid, msgstop->node_name );
        size += sizeof(msg->u.request.u.nameserver_stop);
        break;
    case ReqType_NewProcessNs:
        msgnew = &msg->u.request.u.new_process_ns;
        sprintf( desc, "new-process (nid=%d, pid=%d, verifier=%d, name=%s)",
                msgnew->nid, msgnew->pid, msgnew->verifier, msgnew->process_name );
        size += sizeof(msg->u.request.u.new_process_ns);
        break;
    case ReqType_NodeDown:
        msgdown = &msg->u.request.u.down;
        sprintf( desc, "node-down (nid=%d, node-name=%s, takeover=%d, reason=%s)",
                msgdown->nid, msgdown->node_name,
                msgdown->takeover, msgdown->reason );
        size += sizeof(msg->u.request.u.down);
        break;
    case ReqType_ProcessInfo:
        msginfo = &msg->u.request.u.process_info;
        sprintf( desc, "process-info (nid=%d, pid=%d, verifier=%d, name=%s)\n"
                       "\ttarget (nid=%d, pid=%d, verifier=%d, name=%s, type=%d)\n",
                msginfo->nid, msginfo->pid, msginfo->verifier, msginfo->process_name,
                msginfo->target_nid, msginfo->target_pid, msginfo->target_verifier, 
                msginfo->target_process_name, msginfo->type );
        size += sizeof(msg->u.request.u.process_info);
        break;
    case ReqType_ProcessInfoCont:
        descp = (char *) "process-info-cont";
        size += sizeof(msg->u.request.u.process_info_cont);
        break;
    case ReqType_ProcessInfoNs:
        msginfo = &msg->u.request.u.process_info;
        sprintf( desc, "process-info-ns (nid=%d, pid=%d, verifier=%d, name=%s)\n"
                       "\ttarget (nid=%d, pid=%d, verifier=%d, name=%s, type=%d)\n",
                msginfo->nid, msginfo->pid, msginfo->verifier, msginfo->process_name,
                msginfo->target_nid, msginfo->target_pid, msginfo->target_verifier, 
                msginfo->target_process_name, msginfo->type );
        size += sizeof(msg->u.request.u.process_info);
        break;
    case ReqType_ShutdownNs:
        msgshutdown = &msg->u.request.u.shutdown_ns;
        sprintf( desc, "shutdown-ns (nid=%d, pid=%d, level=%d)",
                msgshutdown->nid, msgshutdown->pid, msgshutdown->level );
        size += sizeof(msg->u.request.u.shutdown_ns);
        break;
    default:
        abort(); // TODO change
        break;
    }

retryIO:

    int error = SendToNs( descp, msg, size );
    if ( error == 0 )
    {
        error = CComm::Receive( mon2nsSock_
                              , (char *) &size
                              , sizeof(size)
                              , mon2nsHost_
                              , method_name );
    }
    if ( error == 0 )
    {
        error = CComm::Receive( mon2nsSock_
                              , (char *) pmsg_reply
                              , size
                              , mon2nsHost_
                              , method_name );
    }
    if ( error != 0 )
    {
        // Choose another name server on IO retry
        if (IsRealCluster)
        {
            mon2nsHost_[0] = 0;
        }
        else
        {
            mon2nsPort_[0] = 0;
        }
    }

    if ( error == 0 )
    {
        memcpy( msg, pmsg_reply, size );
        if ( trace_settings & ( TRACE_NS | TRACE_PROCESS ) )
        {
            char desc[2048];
            char* descp = desc;
            switch ( msg->u.reply.type )
            {
            case ReplyType_DelProcessNs:
                sprintf( desc, "DelProcessNs, nid=%d, pid=%d, verifier=%d, name=%s, rc=%d",
                         msg->u.reply.u.del_process_ns.nid,
                         msg->u.reply.u.del_process_ns.pid,
                         msg->u.reply.u.del_process_ns.verifier,
                         msg->u.reply.u.del_process_ns.process_name,
                         msg->u.reply.u.del_process_ns.return_code );
                break;
            case ReplyType_Generic:
                sprintf( desc, "Generic, nid=%d, pid=%d, verifier=%d, name=%s, rc=%d",
                         msg->u.reply.u.generic.nid,
                         msg->u.reply.u.generic.pid,
                         msg->u.reply.u.generic.verifier,
                         msg->u.reply.u.generic.process_name,
                         msg->u.reply.u.generic.return_code );
                break;
            case ReplyType_NewProcessNs:
                sprintf( desc, "NewProcessNs, nid=%d, pid=%d, verifier=%d, name=%s, rc=%d",
                         msg->u.reply.u.new_process_ns.nid,
                         msg->u.reply.u.new_process_ns.pid,
                         msg->u.reply.u.new_process_ns.verifier,
                         msg->u.reply.u.new_process_ns.process_name,
                         msg->u.reply.u.new_process_ns.return_code );
                break;
            case ReplyType_ProcessInfo:
                sprintf( desc, "ProcessInfo, num_processes=%d, rc=%d, more_data=%d",
                         msg->u.reply.u.process_info.num_processes,
                         msg->u.reply.u.process_info.return_code,
                         msg->u.reply.u.process_info.more_data );
                break;
            case ReplyType_ProcessInfoNs:
                sprintf( desc, 
                         "process-info-ns reply:\n"
                         "        process_info_ns.nid=%d\n"
                         "        process_info_ns.pid=%d\n"
                         "        process_info_ns.verifier=%d\n"
                         "        process_info_ns.process_name=%s\n"
                         "        process_info_ns.type=%d\n"
                         "        process_info_ns.parent_nid=%d\n"
                         "        process_info_ns.parent_pid=%d\n"
                         "        process_info_ns.parent_verifier=%d\n"
                         "        process_info_ns.priority=%d\n"
                         "        process_info_ns.backup=%d\n"
                         "        process_info_ns.state=%d\n"
                         "        process_info_ns.unhooked=%d\n"
                         "        process_info_ns.event_messages=%d\n"
                         "        process_info_ns.system_messages=%d\n"
                         "        process_info_ns.path=%s\n"
                         "        process_info_ns.ldpath=%s\n"
                         "        process_info_ns.program=%s\n"
                         "        process_info_ns.port_name=%s\n"
                         "        process_info_ns.argc=%d\n"
                         "        process_info_ns.infile=%s\n"
                         "        process_info_ns.outfile=%s\n"
#if 0
                         "        process_info_ns.creation_time=%ld(secs)\n",
                         "        process_info_ns.creation_time=%ld(secs):%ld(nsecs)\n",
#endif
                         "        process_info_ns.return_code=%d"
                         , msg->u.reply.u.process_info_ns.nid
                         , msg->u.reply.u.process_info_ns.pid
                         , msg->u.reply.u.process_info_ns.verifier
                         , msg->u.reply.u.process_info_ns.process_name
                         , msg->u.reply.u.process_info_ns.type
                         , msg->u.reply.u.process_info_ns.parent_nid
                         , msg->u.reply.u.process_info_ns.parent_pid
                         , msg->u.reply.u.process_info_ns.parent_verifier
                         , msg->u.reply.u.process_info_ns.priority
                         , msg->u.reply.u.process_info_ns.backup
                         , msg->u.reply.u.process_info_ns.state
                         , msg->u.reply.u.process_info_ns.unhooked
                         , msg->u.reply.u.process_info_ns.event_messages
                         , msg->u.reply.u.process_info_ns.system_messages
                         , msg->u.reply.u.process_info_ns.path
                         , msg->u.reply.u.process_info_ns.ldpath
                         , msg->u.reply.u.process_info_ns.program
                         , msg->u.reply.u.process_info_ns.port_name
                         , msg->u.reply.u.process_info_ns.argc
                         , msg->u.reply.u.process_info_ns.infile
                         , msg->u.reply.u.process_info_ns.outfile
#if 0
                         , msg->u.reply.u.process_info_ns.creation_time.tv_sec
                         , msg->u.reply.u.process_info_ns.creation_time.tv_nsec
#endif
                         , msg->u.reply.u.process_info_ns.return_code );
                break;
            default:
                descp = (char *) "UNKNOWN";
                break;
            }
            trace_printf( "%s@%d - msgType=%d, replyType=%d, ReplyType=%s\n"
                        , method_name, __LINE__
                        , msg->type, msg->u.reply.type
                        , descp
                        );
        }
    }
    else if ( error != -2 && retryCount < NAMESERVER_IO_RETRIES )
    {
        retryCount++;
        if ( trace_settings & TRACE_NS )
        {
            trace_printf( "%s@%d - retrying IO (%d) to nameserver=%s:%s\n"
                        , method_name, __LINE__
                        , retryCount
                        , mon2nsHost_, mon2nsPort_ );
        }
        goto retryIO;
    }

    if ( error )
    {
        // create a synthetic reply
        msg->u.reply.u.generic.nid = -1;
        msg->u.reply.u.generic.pid = -1;
        msg->u.reply.u.generic.verifier = -1;
        msg->u.reply.u.generic.process_name[0] = '\0';
        msg->u.reply.u.generic.return_code = MPI_ERR_IO;
        error = 0;

        if ( trace_settings & ( TRACE_NS | TRACE_PROCESS ) )
        {
            char desc[200];
            sprintf( desc, "Generic, nid=%d, pid=%d, verifier=%d, name=%s, rc=%d",
                     msg->u.reply.u.generic.nid,
                     msg->u.reply.u.generic.pid,
                     msg->u.reply.u.generic.verifier,
                     msg->u.reply.u.generic.process_name,
                     msg->u.reply.u.generic.return_code );
            trace_printf( "%s@%d - msgType=%d, replyType=%d, ReplyType=%s\n"
                        , method_name, __LINE__
                        , msg->type, msg->u.reply.type
                        , desc
                        );
        }
    }

    TRACE_EXIT;
    return error;
}

int CNameServer::SendToNs( const char *reqType, struct message_def *msg, int size )
{
    const char method_name[] = "CNameServer::SendToNs";
    TRACE_ENTRY;

    if ( trace_settings & TRACE_NS )
    {
        trace_printf( "%s@%d - sending %s\tREQ (size=%d) to nameserver=%s:%s, sock=%d, shutdown=%d\n"
                    , method_name, __LINE__
                    , reqType
                    , size
                    , mon2nsHost_
                    , mon2nsPort_
                    , mon2nsSock_ 
                    , shutdown_ );
    }

    int error = 0;
    if ( shutdown_ )
    {
        error = -1;
    }
    else if ( mon2nsSock_ < 0 )
    {
        bool retry = false;
        error = ConnectToNs( &retry );
        if ( retry )
        {
            // only retry once
            error = ConnectToNs( &retry );
        }
    }

    if ( error == 0 )
    {
        error = CComm::Send( mon2nsSock_
                           , (char *) &size
                           , sizeof(size)
                           , mon2nsHost_
                           , method_name );
        if (error)
        {
            int err = error;
            char buf[MON_STRING_BUF_SIZE];
            snprintf( buf, sizeof(buf)
                    , "[%s], unable to send %s request size %d to "
                      "nameserver=%s:%s, error: %d(%s)\n"
                    , method_name, reqType, size, mon2nsHost_, mon2nsPort_, err, strerror(err) );
            mon_log_write(NAMESERVER_SENDTONS_1, SQ_LOG_ERR, buf);    
        }
        else
        {
            error = CComm::Send( mon2nsSock_
                               , (char *) msg
                               , size
                               , mon2nsHost_
                               , method_name );
            if (error)
            {
                int err = error;
                char buf[MON_STRING_BUF_SIZE];
                snprintf( buf, sizeof(buf)
                        , "[%s], unable to send %s request to "
                          "nameserver=%s:%s, error: %d(%s)\n"
                        , method_name, reqType,  mon2nsHost_, mon2nsPort_, err, strerror(err) );
                mon_log_write(NAMESERVER_SENDTONS_2, SQ_LOG_ERR, buf);    
            }
        }
    }

    TRACE_EXIT;
    return error;
}

void CNameServer::SetLocalHost( void )
{
    gethostname( mon2nsHost_, MAX_PROCESSOR_NAME );
}

void CNameServer::SetShutdown( bool shutdown )
{
    const char method_name[] = "CNameServer::SetShutdown";
    TRACE_ENTRY;

    if ( trace_settings & TRACE_NS )
        trace_printf( "%s@%d - set shutdown_=%d\n"
                    , method_name, __LINE__, shutdown );
    shutdown_ = shutdown;

    TRACE_EXIT;
}

void CNameServer::CloseNs( void )
{
    const char method_name[] = "CNameServer::CloseNs";
    TRACE_ENTRY;

    if (mon2nsSock_ != -1)
    {
        CComm::Close( mon2nsSock_ );
        mon2nsSock_ = -1;
    }

    TRACE_EXIT;
}

