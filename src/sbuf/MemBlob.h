/*
 * Copyright (C) 1996-2025 The Squid Software Foundation and contributors
 *
 * Squid software is distributed under GPLv2+ license and includes
 * contributions from numerous individuals and organizations.
 * Please see the COPYING and CONTRIBUTORS files for details.
 */

#ifndef SQUID_SRC_SBUF_MEMBLOB_H
#define SQUID_SRC_SBUF_MEMBLOB_H

#define MEMBLOB_DEBUGSECTION 24

#include "base/InstanceId.h"
#include "base/RefCount.h"
#include "mem/forward.h"

/// Various MemBlob class-wide statistics.
class MemBlobStats
{
public:
    /// dumps class-wide statistics
    std::ostream& dump(std::ostream& os) const;

    MemBlobStats& operator += (const MemBlobStats&);

public:
    uint64_t alloc = 0;     ///< number of MemBlob instances created so far
    uint64_t live = 0;      ///< number of MemBlob instances currently alive
    uint64_t append = 0;    ///< number of MemBlob::append() calls
    uint64_t liveBytes = 0; ///< the total size of currently allocated storage
};

/** Refcountable, fixed-size, content-agnostic memory buffer.
 *
 * Allocated memory block is divided into two sequential areas:
 * "used memory" and "available space". The used area can be filled during
 * construction, grows via the append() call, and can be clear()ed.
 *
 * MemBlob users can cooperate to safely share the used area. However, MemBlob
 * provides weak use accounting and no sharing protections besides refcounting.
 */
class MemBlob: public RefCountable
{
    MEMPROXY_CLASS(MemBlob);

public:
    typedef RefCount<MemBlob> Pointer;

    /* emulate std::basic_string API */
    using value_type = char;
    using size_type = uint32_t;
    using const_pointer = const value_type *;

    /// obtain a const view of class-wide statistics
    static const MemBlobStats& GetStats();

    /// create a new MemBlob with at least reserveSize capacity
    explicit MemBlob(const size_type reserveSize);

    /* emulate std::basic_string API */
    MemBlob(const_pointer, const size_type);
    ~MemBlob() override;

    /// the number unused bytes at the end of the allocated blob
    size_type spaceSize() const { return capacity - size; }

    /** check whether the caller can successfully append() n bytes
     *
     * \return true  the caller may append() n bytes to this blob now
     * \param off    the end of the blob area currently used by the caller
     * \param n      the total number of bytes the caller wants to append
     */
    bool canAppend(const size_type off, const size_type n) const {
        // TODO: ignore offset (and adjust size) when the blob is not shared?
        return (isAppendOffset(off) && willFit(n)) || !n;
    }

    /** adjusts internal object state as if exactly n bytes were append()ed
     *
     * \throw TextException if there was not enough space in the blob
     * \param n the number of bytes that were appended
     */
    void appended(const size_type n);

    /** copies exactly n bytes from the source to the available space area,
     *  enlarging the used area by n bytes
     *
     * \throw TextException if there is not enough space in the blob
     * \param source raw buffer to be copied
     * \param n the number of bytes to copy from the source buffer
     */
    void append(const_pointer source, const size_type n);

    /* non-const methods below require exclusive object ownership */

    /// keep the first n bytes and forget the rest of data
    /// cannot be used to increase our size; use append*() methods for that
    void syncSize(const size_type n);

    /// forget the first n bytes, moving the rest of data (if any) to the start
    /// forgets all data (i.e. empties the buffer) if n exceeds size
    void consume(const size_type n);

    /// dump debugging information
    std::ostream & dump(std::ostream &os) const;

    /* emulate std::basic_string API */
    void clear() { size = 0; }

public:
    /* MemBlob users should avoid these and must treat them as read-only */
    value_type *mem = nullptr; ///< raw allocated memory block
    size_type capacity = 0; ///< size of the raw allocated memory block
    size_type size = 0; ///< maximum allocated memory in use by callers
    const InstanceId<MemBlob> id; ///< blob identifier

private:
    void memAlloc(const size_type memSize);

    /// whether the offset points to the end of the used area
    bool isAppendOffset(const size_type off) const { return off == size; }

    /// whether n more bytes can be appended
    bool willFit(const size_type n) const { return n <= spaceSize(); }

    /* copying is not implemented */
    MemBlob(const MemBlob &);
    MemBlob& operator =(const MemBlob &);
};

#endif /* SQUID_SRC_SBUF_MEMBLOB_H */

