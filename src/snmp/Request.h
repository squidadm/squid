/*
 * Copyright (C) 1996-2025 The Squid Software Foundation and contributors
 *
 * Squid software is distributed under GPLv2+ license and includes
 * contributions from numerous individuals and organizations.
 * Please see the COPYING and CONTRIBUTORS files for details.
 */

/* DEBUG: section 49    SNMP Interface */

#ifndef SQUID_SRC_SNMP_REQUEST_H
#define SQUID_SRC_SNMP_REQUEST_H

#include "ip/Address.h"
#include "ipc/forward.h"
#include "ipc/Request.h"
#include "snmp/Pdu.h"
#include "snmp/Session.h"

namespace Snmp
{

/// SNMP request
class Request: public Ipc::Request
{
public:
    Request(int aRequestorId, Ipc::RequestId aRequestId, const Pdu& aPdu,
            const Session& aSession, int aFd, const Ip::Address& anAddress);

    explicit Request(const Ipc::TypedMsgHdr& msg); ///< from recvmsg()
    /* Ipc::Request API */
    void pack(Ipc::TypedMsgHdr& msg) const override;
    Pointer clone() const override;

public:
    Pdu pdu; ///< SNMP protocol data unit
    Session session; ///< SNMP session
    int fd; ///< client connection descriptor
    Ip::Address address; ///< client address
};

} // namespace Snmp

#endif /* SQUID_SRC_SNMP_REQUEST_H */

