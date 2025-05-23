/*
 * Copyright (C) 1996-2025 The Squid Software Foundation and contributors
 *
 * Squid software is distributed under GPLv2+ license and includes
 * contributions from numerous individuals and organizations.
 * Please see the COPYING and CONTRIBUTORS files for details.
 */

/* DEBUG: section 54    Interprocess Communication */

#include "squid.h"
#include "base/AsyncCallbacks.h"
#include "base/TextException.h"
#include "comm.h"
#include "comm/Connection.h"
#include "globals.h"
#include "ipc/Kids.h"
#include "ipc/Messages.h"
#include "ipc/Port.h"
#include "ipc/SharedListen.h"
#include "ipc/StartListening.h"
#include "ipc/TypedMsgHdr.h"
#include "tools.h"

#include <list>
#include <map>

/// holds information necessary to handle JoinListen response
class PendingOpenRequest
{
public:
    Ipc::OpenListenerParams params; ///< actual comm_open_sharedListen() parameters
    Ipc::StartListeningCallback callback; // who to notify
};

/// maps ID assigned at request time to the response callback
typedef std::map<Ipc::RequestId::Index, PendingOpenRequest> SharedListenRequestMap;
static SharedListenRequestMap TheSharedListenRequestMap;

/// accumulates delayed requests until they are ready to be sent, in FIFO order
typedef std::list<PendingOpenRequest> DelayedSharedListenRequests;
static DelayedSharedListenRequests TheDelayedRequests;

// TODO: Encapsulate "Pending Request Map" logic shared by all RequestId users.
/// registers the given request in the collection of pending requests
/// \returns the registration key
static Ipc::RequestId::Index
AddToMap(const PendingOpenRequest &por)
{
    static Ipc::RequestId::Index LastIndex = 0;
    // TODO: Switch Ipc::RequestId::Index to uint64_t and drop these 0 checks.
    if (++LastIndex == 0) // don't use zero value as an ID
        ++LastIndex;
    assert(TheSharedListenRequestMap.find(LastIndex) == TheSharedListenRequestMap.end());
    TheSharedListenRequestMap[LastIndex] = por;
    return LastIndex;
}

bool
Ipc::OpenListenerParams::operator <(const OpenListenerParams &p) const
{
    if (sock_type != p.sock_type)
        return sock_type < p.sock_type;

    if (proto != p.proto)
        return proto < p.proto;

    // ignore flags and fdNote differences because they do not affect binding

    return addr.compareWhole(p.addr) < 0;
}

Ipc::SharedListenRequest::SharedListenRequest(const OpenListenerParams &aParams, const RequestId aMapId):
    requestorId(KidIdentifier),
    params(aParams),
    mapId(aMapId)
{
    // caller will then set public data members
}

Ipc::SharedListenRequest::SharedListenRequest(const TypedMsgHdr &hdrMsg)
{
    hdrMsg.checkType(mtSharedListenRequest);
    hdrMsg.getPod(*this);
}

void Ipc::SharedListenRequest::pack(TypedMsgHdr &hdrMsg) const
{
    hdrMsg.setType(mtSharedListenRequest);
    hdrMsg.putPod(*this);
}

Ipc::SharedListenResponse::SharedListenResponse(const int aFd, const int anErrNo, const RequestId aMapId):
    fd(aFd), errNo(anErrNo), mapId(aMapId)
{
}

Ipc::SharedListenResponse::SharedListenResponse(const TypedMsgHdr &hdrMsg):
    fd(-1),
    errNo(0)
{
    hdrMsg.checkType(mtSharedListenResponse);
    hdrMsg.getPod(*this);
    fd = hdrMsg.getFd();
    // other conn details are passed in OpenListenerParams and filled out by SharedListenJoin()
}

void Ipc::SharedListenResponse::pack(TypedMsgHdr &hdrMsg) const
{
    hdrMsg.setType(mtSharedListenResponse);
    hdrMsg.putPod(*this);
    // XXX: When we respond with an error, putFd() throws due to the negative fd
    hdrMsg.putFd(fd);
}

static void
SendSharedListenRequest(const PendingOpenRequest &por)
{
    const Ipc::SharedListenRequest request(por.params, Ipc::RequestId(AddToMap(por)));

    debugs(54, 3, "getting listening FD for " << request.params.addr <<
           " mapId=" << request.mapId);

    Ipc::TypedMsgHdr message;
    request.pack(message);
    SendMessage(Ipc::Port::CoordinatorAddr(), message);
}

static void
kickDelayedRequest()
{
    if (TheDelayedRequests.empty())
        return; // no pending requests to resume

    debugs(54, 3, "resuming with " << TheSharedListenRequestMap.size() <<
           " active + " << TheDelayedRequests.size() << " delayed requests");

    SendSharedListenRequest(*TheDelayedRequests.begin());
    TheDelayedRequests.pop_front();
}

void
Ipc::JoinSharedListen(const OpenListenerParams &params, StartListeningCallback &cb)
{
    PendingOpenRequest por;
    por.params = params;
    por.callback = cb;

    const DelayedSharedListenRequests::size_type concurrencyLimit = 1;
    if (TheSharedListenRequestMap.size() >= concurrencyLimit) {
        debugs(54, 3, "waiting for " << TheSharedListenRequestMap.size() <<
               " active + " << TheDelayedRequests.size() << " delayed requests");
        TheDelayedRequests.push_back(por);
    } else {
        SendSharedListenRequest(por);
    }
}

void Ipc::SharedListenJoined(const SharedListenResponse &response)
{
    // Dont debugs c fully since only FD is filled right now.
    debugs(54, 3, "got listening FD " << response.fd << " errNo=" <<
           response.errNo << " mapId=" << response.mapId << " with " <<
           TheSharedListenRequestMap.size() << " active + " <<
           TheDelayedRequests.size() << " delayed requests");

    Must(response.mapId);
    const auto pori = TheSharedListenRequestMap.find(response.mapId.index());
    Must(pori != TheSharedListenRequestMap.end());
    auto por = pori->second;
    Must(por.callback);
    TheSharedListenRequestMap.erase(pori);

    auto &answer = por.callback.answer();
    Assure(answer.conn);
    auto &conn = answer.conn;
    conn->fd = response.fd;

    if (Comm::IsConnOpen(conn)) {
        OpenListenerParams &p = por.params;
        conn->local = p.addr;
        conn->flags = p.flags;
        // XXX: leave the comm AI stuff to comm_import_opened()?
        struct addrinfo *AI = nullptr;
        p.addr.getAddrInfo(AI);
        AI->ai_socktype = p.sock_type;
        AI->ai_protocol = p.proto;
        comm_import_opened(conn, FdNote(p.fdNote), AI);
        Ip::Address::FreeAddr(AI);
    }

    answer.errNo = response.errNo;
    ScheduleCallHere(por.callback.release());

    kickDelayedRequest();
}

