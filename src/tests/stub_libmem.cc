/*
 * Copyright (C) 1996-2025 The Squid Software Foundation and contributors
 *
 * Squid software is distributed under GPLv2+ license and includes
 * contributions from numerous individuals and organizations.
 * Please see the COPYING and CONTRIBUTORS files for details.
 */

#include "squid.h"

#define STUB_API "mem/libmem.la"
#include "tests/STUB.h"

#include "mem/Allocator.h"

#include "mem/AllocatorProxy.h"
#include "mem/forward.h"

void *Mem::AllocatorProxy::alloc() {return xmalloc(64*1024);}
void Mem::AllocatorProxy::freeOne(void *address) {xfree(address);}
int Mem::AllocatorProxy::inUseCount() const {return 0;}
size_t Mem::AllocatorProxy::getStats(PoolStats &) STUB_RETVAL(0)

#include "mem/forward.h"
void Mem::Init() STUB_NOP
void Mem::Stats(StoreEntry *) STUB_NOP
void Mem::CleanIdlePools(void *) STUB_NOP
void Mem::Report(std::ostream &) STUB_NOP
void Mem::PoolReport(const PoolStats *, const PoolMeter *, std::ostream &) STUB_NOP
//const size_t squidSystemPageSize = 4096;
void memClean(void) STUB
void memInitModule(void) STUB
void memCleanModule(void) STUB
void memConfigure(void) STUB

void *memAllocate(mem_type)
{
    // let's waste plenty of memory. This should cover any possible need
    return xmalloc(64*1024);
}

void *
memAllocBuf(size_t net_size, size_t * gross_size)
{
    if (gross_size)
        *gross_size = net_size;
    return xmalloc(net_size);
}

/* net_size is the new size, *gross size is the old gross size, to be changed to
 * the new gross size as a side-effect.
 */
void *
memReallocBuf(void *oldbuf, size_t net_size, size_t * gross_size)
{
    void *rv=xrealloc(oldbuf,net_size);
//    if (net_size > *gross_size)
//        memset(rv+net_size,0,net_size-*gross_size);
    *gross_size=net_size;
    return rv;
}

void memFree(void *p, int) {xfree(p);}
void memFreeBuf(size_t, void *buf) {xfree(buf);}
static void cxx_xfree(void * ptr) {xfree(ptr);}
FREE *memFreeBufFunc(size_t) {return cxx_xfree;}
int memInUse(mem_type) STUB_RETVAL(0)

#include "mem/Pool.h"
static MemPools tmpMemPools;
MemPools &MemPools::GetInstance() {return tmpMemPools;}
MemPools::MemPools() STUB_NOP
void MemPools::flushMeters() STUB
Mem::Allocator * MemPools::create(const char *, size_t) STUB_RETVAL(nullptr);
void MemPools::clean(time_t) STUB
void MemPools::setDefaultPoolChunking(bool const &) STUB

#include "mem/Stats.h"
size_t Mem::GlobalStats(PoolStats &) STUB_RETVAL(0)
