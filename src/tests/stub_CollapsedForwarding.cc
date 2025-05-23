/*
 * Copyright (C) 1996-2025 The Squid Software Foundation and contributors
 *
 * Squid software is distributed under GPLv2+ license and includes
 * contributions from numerous individuals and organizations.
 * Please see the COPYING and CONTRIBUTORS files for details.
 */

#include "squid.h"
#include "CollapsedForwarding.h"

#define STUB_API "CollapsedForwarding.cc"
#include "tests/STUB.h"

void CollapsedForwarding::Broadcast(StoreEntry const&, const bool) STUB
void CollapsedForwarding::Broadcast(const sfileno, const bool) STUB
void CollapsedForwarding::StatQueue(std::ostream &) STUB

