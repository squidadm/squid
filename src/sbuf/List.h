/*
 * Copyright (C) 1996-2025 The Squid Software Foundation and contributors
 *
 * Squid software is distributed under GPLv2+ license and includes
 * contributions from numerous individuals and organizations.
 * Please see the COPYING and CONTRIBUTORS files for details.
 */

#ifndef SQUID_SRC_SBUF_LIST_H
#define SQUID_SRC_SBUF_LIST_H

#include "sbuf/SBuf.h"

#include <list>

typedef std::list<SBuf> SBufList;

/** check for membership
 *
 * \return true if the supplied SBuf is a member of the list
 * \param case_sensitive one of caseSensitive or caseInsensitive
 */
bool IsMember(const SBufList &, const SBuf &, const SBufCaseSensitive isCaseSensitive = caseSensitive);

#endif /* SQUID_SRC_SBUF_LIST_H */

