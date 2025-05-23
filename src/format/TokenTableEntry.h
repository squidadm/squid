/*
 * Copyright (C) 1996-2025 The Squid Software Foundation and contributors
 *
 * Squid software is distributed under GPLv2+ license and includes
 * contributions from numerous individuals and organizations.
 * Please see the COPYING and CONTRIBUTORS files for details.
 */

#ifndef SQUID_SRC_FORMAT_TOKENTABLEENTRY_H
#define SQUID_SRC_FORMAT_TOKENTABLEENTRY_H

#include "format/ByteCode.h"

/*
 * Squid configuration allows users to define custom formats in
 * several components.
 * - logging
 * - external ACL input
 * - deny page URL
 *
 * These enumerations and classes define the API for parsing of
 * format directives to define these patterns. Along with output
 * functionality to produce formatted buffers.
 */

namespace Format
{

/// One entry in a table of format tokens.
class TokenTableEntry
{
public:
    TokenTableEntry() : configTag(nullptr), tokenType(LFT_NONE), options(0) {}
    TokenTableEntry(const char *aTag, const ByteCode_t &aType) : configTag(aTag), tokenType(aType), options(0) {}
    // nothing to destruct configTag is pointer to global const string
    ~TokenTableEntry() {}
    TokenTableEntry(const TokenTableEntry& t) : configTag(t.configTag), tokenType(t.tokenType), options(t.options) {}

    /// the config file ASCII representation for this token
    /// just the base tag bytes, excluding any option syntax bytes
    const char *configTag;

    /// the internal byte code representatio of this token
    ByteCode_t tokenType;

    /// 32-bit mask? of options affecting the output display of this token
    uint32_t options;

private:
    TokenTableEntry &operator =(const TokenTableEntry&); // not implemented
};

} // namespace Format

#endif /* SQUID_SRC_FORMAT_TOKENTABLEENTRY_H */

