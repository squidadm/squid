/*
 * Copyright (C) 1996-2025 The Squid Software Foundation and contributors
 *
 * Squid software is distributed under GPLv2+ license and includes
 * contributions from numerous individuals and organizations.
 * Please see the COPYING and CONTRIBUTORS files for details.
 */

/* DEBUG: section 72    Peer Digest Routines */

#include "squid.h"
#if USE_CACHE_DIGESTS
#include "base/IoManip.h"
#include "CacheDigest.h"
#include "CachePeer.h"
#include "event.h"
#include "FwdState.h"
#include "globals.h"
#include "HttpReply.h"
#include "HttpRequest.h"
#include "internal.h"
#include "MemObject.h"
#include "mime_header.h"
#include "neighbors.h"
#include "PeerDigest.h"
#include "Store.h"
#include "store_key_md5.h"
#include "StoreClient.h"
#include "tools.h"
#include "util.h"

/* local types */

/* local prototypes */
static time_t peerDigestIncDelay(const PeerDigest * pd);
static time_t peerDigestNewDelay(const StoreEntry * e);
static void peerDigestSetCheck(PeerDigest * pd, time_t delay);
static EVH peerDigestCheck;
static void peerDigestRequest(PeerDigest * pd);
static STCB peerDigestHandleReply;
static int peerDigestFetchReply(void *, char *, ssize_t);
int peerDigestSwapInCBlock(void *, char *, ssize_t);
int peerDigestSwapInMask(void *, char *, ssize_t);
static int peerDigestFetchedEnough(DigestFetchState * fetch, char *buf, ssize_t size, const char *step_name);
static void finishAndDeleteFetch(DigestFetchState *, const char *reason, bool sawError);
static void peerDigestFetchSetStats(DigestFetchState * fetch);
static int peerDigestSetCBlock(PeerDigest * pd, const char *buf);
static int peerDigestUseful(const PeerDigest * pd);

/* local constants */
Version const CacheDigestVer = { 5, 3 };

#define StoreDigestCBlockSize sizeof(StoreDigestCBlock)

/* min interval for requesting digests from a given peer */
static const time_t PeerDigestReqMinGap = 5 * 60;   /* seconds */
/* min interval for requesting digests (cumulative request stream) */
static const time_t GlobDigestReqMinGap = 1 * 60;   /* seconds */

/* local vars */

static time_t pd_last_req_time = 0; /* last call to Check */

PeerDigest::PeerDigest(CachePeer * const p):
    peer(p),
    host(peer->host) // if peer disappears, we will know its name
{
    times.initialized = squid_curtime;
}

CBDATA_CLASS_INIT(PeerDigest);

CBDATA_CLASS_INIT(DigestFetchState);

DigestFetchState::DigestFetchState(PeerDigest *aPd, HttpRequest *req) :
    pd(aPd),
    entry(nullptr),
    old_entry(nullptr),
    sc(nullptr),
    old_sc(nullptr),
    request(req),
    offset(0),
    mask_offset(0),
    start_time(squid_curtime),
    resp_time(0),
    expires(0),
    bufofs(0),
    state(DIGEST_READ_REPLY)
{
    HTTPMSGLOCK(request);

    sent.msg = 0;
    sent.bytes = 0;

    recv.msg = 0;
    recv.bytes = 0;

    *buf = 0;
}

DigestFetchState::~DigestFetchState()
{
    if (old_entry) {
        debugs(72, 3, "deleting old entry");
        storeUnregister(old_sc, old_entry, this);
        old_entry->releaseRequest();
        old_entry->unlock("DigestFetchState destructed old");
        old_entry = nullptr;
    }

    /* unlock everything */
    storeUnregister(sc, entry, this);

    entry->unlock("DigestFetchState destructed");
    entry = nullptr;

    HTTPMSGUNLOCK(request);
}

PeerDigest::~PeerDigest()
{
    if (times.next_check && eventFind(peerDigestCheck, this))
        eventDelete(peerDigestCheck, this);
    delete cd;
    // req_result pointer is not owned by us
}

/* called by peer to indicate that somebody actually needs this digest */
void
peerDigestNeeded(PeerDigest * pd)
{
    assert(pd);
    assert(!pd->flags.needed);
    assert(!pd->cd);

    pd->flags.needed = true;
    pd->times.needed = squid_curtime;
    peerDigestSetCheck(pd, 0);  /* check asap */
}

/* increment retry delay [after an unsuccessful attempt] */
static time_t
peerDigestIncDelay(const PeerDigest * pd)
{
    assert(pd);
    return pd->times.retry_delay > 0 ?
           2 * pd->times.retry_delay :  /* exponential backoff */
           PeerDigestReqMinGap; /* minimal delay */
}

/* artificially increases Expires: setting to avoid race conditions
 * returns the delay till that [increased] expiration time */
static time_t
peerDigestNewDelay(const StoreEntry * e)
{
    assert(e);

    if (e->expires > 0)
        return e->expires + PeerDigestReqMinGap - squid_curtime;

    return PeerDigestReqMinGap;
}

/* registers next digest verification */
static void
peerDigestSetCheck(PeerDigest * pd, time_t delay)
{
    eventAdd("peerDigestCheck", peerDigestCheck, pd, (double) delay, 1);
    pd->times.next_check = squid_curtime + delay;
    debugs(72, 3, "peerDigestSetCheck: will check peer " << pd->host << " in " << delay << " secs");
}

/* callback for eventAdd() (with peer digest locked)
 * request new digest if our copy is too old or if we lack one;
 * schedule next check otherwise */
static void
peerDigestCheck(void *data)
{
    PeerDigest *pd = (PeerDigest *)data;
    time_t req_time;

    assert(!pd->flags.requested);

    pd->times.next_check = 0;   /* unknown */

    Assure(pd->peer.valid());

    debugs(72, 3, "cache_peer " << RawPointer(pd->peer).orNil());
    debugs(72, 3, "peerDigestCheck: time: " << squid_curtime <<
           ", last received: " << (long int) pd->times.received << "  (" <<
           std::showpos << (int) (squid_curtime - pd->times.received) << ")");

    /* decide when we should send the request:
     * request now unless too close to other requests */
    req_time = squid_curtime;

    /* per-peer limit */

    if (req_time - pd->times.received < PeerDigestReqMinGap) {
        debugs(72, 2, "peerDigestCheck: " << pd->host <<
               ", avoiding close peer requests (" <<
               (int) (req_time - pd->times.received) << " < " <<
               (int) PeerDigestReqMinGap << " secs).");

        req_time = pd->times.received + PeerDigestReqMinGap;
    }

    /* global limit */
    if (req_time - pd_last_req_time < GlobDigestReqMinGap) {
        debugs(72, 2, "peerDigestCheck: " << pd->host <<
               ", avoiding close requests (" <<
               (int) (req_time - pd_last_req_time) << " < " <<
               (int) GlobDigestReqMinGap << " secs).");

        req_time = pd_last_req_time + GlobDigestReqMinGap;
    }

    if (req_time <= squid_curtime)
        peerDigestRequest(pd);  /* will set pd->flags.requested */
    else
        peerDigestSetCheck(pd, req_time - squid_curtime);
}

/* ask store for a digest */
static void
peerDigestRequest(PeerDigest * pd)
{
    const auto p = pd->peer.get(); // TODO: Replace with a reference.
    StoreEntry *e, *old_e;
    char *url = nullptr;
    HttpRequest *req;
    StoreIOBuffer tempBuffer;

    pd->req_result = nullptr;
    pd->flags.requested = true;

    /* compute future request components */

    if (p->digest_url)
        url = xstrdup(p->digest_url);
    else
        url = xstrdup(internalRemoteUri(p->secure.encryptTransport, p->host, p->http_port, "/squid-internal-periodic/", SBuf(StoreDigestFileName)));
    debugs(72, 2, url);

    const auto mx = MasterXaction::MakePortless<XactionInitiator::initCacheDigest>();
    req = HttpRequest::FromUrlXXX(url, mx);

    assert(req);

    /* add custom headers */
    assert(!req->header.len);

    req->header.putStr(Http::HdrType::ACCEPT, StoreDigestMimeStr);

    req->header.putStr(Http::HdrType::ACCEPT, "text/html");

    if (p->login &&
            p->login[0] != '*' &&
            strcmp(p->login, "PASS") != 0 &&
            strcmp(p->login, "PASSTHRU") != 0 &&
            strncmp(p->login, "NEGOTIATE",9) != 0 &&
            strcmp(p->login, "PROXYPASS") != 0) {
        req->url.userInfo(SBuf(p->login)); // XXX: performance regression make peer login SBuf as well.
    }
    /* create fetch state structure */
    DigestFetchState *fetch = new DigestFetchState(pd, req);

    /* update timestamps */
    pd->times.requested = squid_curtime;
    pd_last_req_time = squid_curtime;
    req->flags.cachable.support(); // prevent RELEASE_REQUEST in storeCreateEntry()

    /* the rest is based on clientReplyContext::processExpired() */
    req->flags.refresh = true;

    old_e = fetch->old_entry = storeGetPublicByRequest(req);

    // XXX: Missing a hittingRequiresCollapsing() && startCollapsingOn() check.
    if (old_e) {
        debugs(72, 5, "found old " << *old_e);

        old_e->lock("peerDigestRequest");
        old_e->ensureMemObject(url, url, req->method);

        fetch->old_sc = storeClientListAdd(old_e, fetch);
    }

    e = fetch->entry = storeCreateEntry(url, url, req->flags, req->method);
    debugs(72, 5, "created " << *e);
    assert(EBIT_TEST(e->flags, KEY_PRIVATE));
    fetch->sc = storeClientListAdd(e, fetch);
    /* set lastmod to trigger IMS request if possible */

    // TODO: Also check for fetch->pd->cd presence as a precondition for sending
    // IMS requests because peerDigestFetchReply() does not accept 304 responses
    // without an in-memory cache digest.
    if (old_e)
        e->lastModified(old_e->lastModified());

    /* push towards peer cache */
    FwdState::fwdStart(Comm::ConnectionPointer(), e, req);

    tempBuffer.offset = 0;

    tempBuffer.length = SM_PAGE_SIZE;

    tempBuffer.data = fetch->buf;

    storeClientCopy(fetch->sc, e, tempBuffer,
                    peerDigestHandleReply, fetch);

    safe_free(url);
}

/* Handle the data copying .. */

/*
 * This routine handles the copy data and then redirects the
 * copy to a bunch of subfunctions depending upon the copy state.
 * It also tracks the buffer offset and "seen", since I'm actually
 * not interested in rewriting everything to suit my little idea.
 */
static void
peerDigestHandleReply(void *data, StoreIOBuffer receivedData)
{
    DigestFetchState *fetch = (DigestFetchState *)data;
    int retsize = -1;
    digest_read_state_t prevstate;
    int newsize;

    if (receivedData.flags.error) {
        finishAndDeleteFetch(fetch, "failure loading digest reply from Store", true);
        return;
    }

    if (!fetch->pd) {
        finishAndDeleteFetch(fetch, "digest disappeared while loading digest reply from Store", true);
        return;
    }

    /* The existing code assumes that the received pointer is
     * where we asked the data to be put
     */
    assert(!receivedData.data || fetch->buf + fetch->bufofs == receivedData.data);

    /* Update the buffer size */
    fetch->bufofs += receivedData.length;

    assert(fetch->bufofs <= SM_PAGE_SIZE);

    /* If we've fetched enough, return */

    if (peerDigestFetchedEnough(fetch, fetch->buf, fetch->bufofs, "peerDigestHandleReply"))
        return;

    /* Call the right function based on the state */
    /* (Those functions will update the state if needed) */

    /* Give us a temporary reference. Some of the calls we make may
     * try to destroy the fetch structure, and we like to know if they
     * do
     */
    CbcPointer<DigestFetchState> tmpLock = fetch;

    /* Repeat this loop until we're out of data OR the state changes */
    /* (So keep going if the state has changed and we still have data */
    do {
        prevstate = fetch->state;

        switch (fetch->state) {

        case DIGEST_READ_REPLY:
            retsize = peerDigestFetchReply(fetch, fetch->buf, fetch->bufofs);
            break;

        case DIGEST_READ_CBLOCK:
            retsize = peerDigestSwapInCBlock(fetch, fetch->buf, fetch->bufofs);
            break;

        case DIGEST_READ_MASK:
            retsize = peerDigestSwapInMask(fetch, fetch->buf, fetch->bufofs);
            break;

        case DIGEST_READ_NONE:
            break;

        default:
            fatal("Bad digest transfer mode!\n");
        }

        if (retsize < 0)
            return;

        /*
         * The returned size indicates how much of the buffer was read -
         * so move the remainder of the buffer to the beginning
         * and update the bufofs / bufsize
         */
        newsize = fetch->bufofs - retsize;

        memmove(fetch->buf, fetch->buf + retsize, fetch->bufofs - newsize);

        fetch->bufofs = newsize;

    } while (cbdataReferenceValid(fetch) && prevstate != fetch->state && fetch->bufofs > 0);

    // Check for EOF here, thus giving the parser one extra run. We could avoid this overhead by
    // checking at the beginning of this function. However, in this case, we would have to require
    // that the parser does not regard EOF as a special condition (it is true now but may change
    // in the future).
    if (fetch->sc->atEof()) {
        finishAndDeleteFetch(fetch, "premature end of digest reply", true);
        return;
    }

    /* Update the copy offset */
    fetch->offset += receivedData.length;

    /* Schedule another copy */
    if (cbdataReferenceValid(fetch)) {
        StoreIOBuffer tempBuffer;
        tempBuffer.offset = fetch->offset;
        tempBuffer.length = SM_PAGE_SIZE - fetch->bufofs;
        tempBuffer.data = fetch->buf + fetch->bufofs;
        storeClientCopy(fetch->sc, fetch->entry, tempBuffer,
                        peerDigestHandleReply, fetch);
    }
}

/// handle HTTP response headers in the initial storeClientCopy() response
static int
peerDigestFetchReply(void *data, char *buf, ssize_t size)
{
    DigestFetchState *fetch = (DigestFetchState *)data;
    const auto pd = fetch->pd.get();
    assert(pd && buf);
    assert(!fetch->offset);

    assert(fetch->state == DIGEST_READ_REPLY);

    if (peerDigestFetchedEnough(fetch, buf, size, "peerDigestFetchReply"))
        return -1;

    {
        const auto &reply = fetch->entry->mem().freshestReply();
        const auto status = reply.sline.status();
        assert(status != Http::scNone);
        debugs(72, 3, "peerDigestFetchReply: " << pd->host << " status: " << status <<
               ", expires: " << (long int) reply.expires << " (" << std::showpos <<
               (int) (reply.expires - squid_curtime) << ")");

        /* this "if" is based on clientHandleIMSReply() */

        if (status == Http::scNotModified) {
            /* our old entry is fine */
            assert(fetch->old_entry);

            if (!fetch->old_entry->mem_obj->request)
                fetch->old_entry->mem_obj->request = fetch->entry->mem_obj->request;

            assert(fetch->old_entry->mem_obj->request);

            if (!Store::Root().updateOnNotModified(fetch->old_entry, *fetch->entry)) {
                finishAndDeleteFetch(fetch, "header update failure after a 304 response", true);
                return -1;
            }

            /* get rid of 304 reply */
            storeUnregister(fetch->sc, fetch->entry, fetch);

            fetch->entry->unlock("peerDigestFetchReply 304");

            fetch->entry = fetch->old_entry;

            fetch->old_entry = nullptr;

            /* preserve request -- we need its size to update counters */
            /* requestUnlink(r); */
            /* fetch->entry->mem_obj->request = nullptr; */

            if (!fetch->pd->cd) {
                finishAndDeleteFetch(fetch, "304 without the old in-memory digest", true);
                return -1;
            }

            // stay with the old in-memory digest
            finishAndDeleteFetch(fetch, "Not modified", false);
            return -1;
        } else if (status == Http::scOkay) {
            /* get rid of old entry if any */

            if (fetch->old_entry) {
                debugs(72, 3, "peerDigestFetchReply: got new digest, releasing old one");
                storeUnregister(fetch->old_sc, fetch->old_entry, fetch);
                fetch->old_entry->releaseRequest();
                fetch->old_entry->unlock("peerDigestFetchReply 200");
                fetch->old_entry = nullptr;
            }

            fetch->state = DIGEST_READ_CBLOCK;
        } else {
            /* some kind of a bug */
            finishAndDeleteFetch(fetch, reply.sline.reason(), true);
            return -1;      /* XXX -1 will abort stuff in ReadReply! */
        }
    }

    return 0; // we consumed/used no buffered bytes
}

int
peerDigestSwapInCBlock(void *data, char *buf, ssize_t size)
{
    DigestFetchState *fetch = (DigestFetchState *)data;

    assert(fetch->state == DIGEST_READ_CBLOCK);

    if (peerDigestFetchedEnough(fetch, buf, size, "peerDigestSwapInCBlock"))
        return -1;

    if (size >= (ssize_t)StoreDigestCBlockSize) {
        const auto pd = fetch->pd.get();

        assert(pd);
        assert(fetch->entry->mem_obj);

        if (peerDigestSetCBlock(pd, buf)) {
            /* XXX: soon we will have variable header size */
            /* switch to CD buffer and fetch digest guts */
            buf = nullptr;
            assert(pd->cd->mask);
            fetch->state = DIGEST_READ_MASK;
            return StoreDigestCBlockSize;
        } else {
            finishAndDeleteFetch(fetch, "invalid digest cblock", true);
            return -1;
        }
    }

    /* need more data, do we have space? */
    if (size >= SM_PAGE_SIZE) {
        finishAndDeleteFetch(fetch, "digest cblock too big", true);
        return -1;
    }

    return 0;       /* We need more data */
}

int
peerDigestSwapInMask(void *data, char *buf, ssize_t size)
{
    DigestFetchState *fetch = (DigestFetchState *)data;
    const auto pd = fetch->pd.get();
    assert(pd);
    assert(pd->cd && pd->cd->mask);

    /*
     * NOTENOTENOTENOTENOTE: buf doesn't point to pd->cd->mask anymore!
     * we need to do the copy ourselves!
     */
    memcpy(pd->cd->mask + fetch->mask_offset, buf, size);

    /* NOTE! buf points to the middle of pd->cd->mask! */

    if (peerDigestFetchedEnough(fetch, nullptr, size, "peerDigestSwapInMask"))
        return -1;

    fetch->mask_offset += size;

    if (fetch->mask_offset >= pd->cd->mask_size) {
        debugs(72, 2, "peerDigestSwapInMask: Done! Got " <<
               fetch->mask_offset << ", expected " << pd->cd->mask_size);
        assert(fetch->mask_offset == pd->cd->mask_size);
        assert(peerDigestFetchedEnough(fetch, nullptr, 0, "peerDigestSwapInMask"));
        return -1;      /* XXX! */
    }

    /* We always read everything, so return size */
    return size;
}

static int
peerDigestFetchedEnough(DigestFetchState * fetch, char *, ssize_t size, const char *step_name)
{
    static const SBuf hostUnknown("<unknown>"); // peer host (if any)
    SBuf host = hostUnknown;

    const auto pd = fetch->pd.get();
    Assure(pd);
    const char *reason = nullptr;  /* reason for completion */
    const char *no_bug = nullptr;  /* successful completion if set */

    /* test possible exiting conditions (the same for most steps!)
     * cases marked with '?!' should not happen */

    if (!reason) {
        host = pd->host;
    }

    debugs(72, 6, step_name << ": peer " << host << ", offset: " <<
           fetch->offset << " size: " << size << ".");

    /* continue checking (with pd and host known and valid) */

    if (!reason) {
        if (size < 0)
            reason = "swap failure";
        else if (!fetch->entry)
            reason = "swap aborted?!";
        else if (EBIT_TEST(fetch->entry->flags, ENTRY_ABORTED))
            reason = "swap aborted";
    }

    /* continue checking (maybe-successful eof case) */
    if (!reason && !size && fetch->state != DIGEST_READ_REPLY) {
        if (!pd->cd)
            reason = "null digest?!";
        else if (fetch->mask_offset != pd->cd->mask_size)
            reason = "premature end of digest?!";
        else if (!peerDigestUseful(pd))
            reason = "useless digest";
        else
            reason = no_bug = "success";
    }

    /* finish if we have a reason */
    if (reason) {
        const int level = strstr(reason, "?!") ? 1 : 3;
        debugs(72, level, "" << step_name << ": peer " << host << ", exiting after '" << reason << "'");
        finishAndDeleteFetch(fetch, reason, !no_bug);
    }

    return reason != nullptr;
}

/* complete the digest transfer, update stats, unlock/release everything */
static void
finishAndDeleteFetch(DigestFetchState * const fetch, const char * const reason, const bool err)
{
    assert(reason);

    debugs(72, 2, "peer: " << RawPointer(fetch->pd.valid() ? fetch->pd->peer : nullptr).orNil() << ", reason: " << reason << ", err: " << err);

    /* note: order is significant */
    peerDigestFetchSetStats(fetch);
    if (const auto pd = fetch->pd.get())
        pd->noteFetchFinished(*fetch, reason, err);

    delete fetch;
}

void
PeerDigest::noteFetchFinished(const DigestFetchState &finishedFetch, const char * const outcomeDescription, const bool sawError)
{
    const auto pd = this; // TODO: remove this diff reducer
    const auto fetch = &finishedFetch; // TODO: remove this diff reducer

    pd->flags.requested = false;
    pd->req_result = outcomeDescription;

    pd->times.received = squid_curtime;
    pd->times.req_delay = fetch->resp_time;
    pd->stats.sent.kbytes += fetch->sent.bytes;
    pd->stats.recv.kbytes += fetch->recv.bytes;
    pd->stats.sent.msgs += fetch->sent.msg;
    pd->stats.recv.msgs += fetch->recv.msg;

    if (sawError) {
        debugs(72, DBG_IMPORTANT, "disabling (" << outcomeDescription << ") digest from " << host);

        pd->times.retry_delay = peerDigestIncDelay(pd);
        peerDigestSetCheck(pd, pd->times.retry_delay);
        delete pd->cd;
        pd->cd = nullptr;

        pd->flags.usable = false;
    } else {
        pd->flags.usable = true;
        pd->times.retry_delay = 0;
        peerDigestSetCheck(pd, peerDigestNewDelay(fetch->entry));

        /* XXX: ugly condition, but how? */

        if (fetch->entry->store_status == STORE_OK)
            debugs(72, 2, "re-used old digest from " << host);
        else
            debugs(72, 2, "received valid digest from " << host);
    }
}

/* calculate fetch stats after completion */
static void
peerDigestFetchSetStats(DigestFetchState * fetch)
{
    MemObject *mem;
    assert(fetch->entry && fetch->request);

    mem = fetch->entry->mem_obj;
    assert(mem);

    /* XXX: outgoing numbers are not precise */
    /* XXX: we must distinguish between 304 hits and misses here */
    fetch->sent.bytes = fetch->request->prefixLen();
    /* XXX: this is slightly wrong: we don't KNOW that the entire memobject
     * was fetched. We only know how big it is
     */
    fetch->recv.bytes = mem->size();
    fetch->sent.msg = fetch->recv.msg = 1;
    fetch->expires = fetch->entry->expires;
    fetch->resp_time = squid_curtime - fetch->start_time;

    debugs(72, 3, "peerDigestFetchFinish: recv " << fetch->recv.bytes <<
           " bytes in " << (int) fetch->resp_time << " secs");

    debugs(72, 3, "peerDigestFetchFinish: expires: " <<
           (long int) fetch->expires << " (" << std::showpos <<
           (int) (fetch->expires - squid_curtime) << "), lmt: " <<
           std::noshowpos << (long int) fetch->entry->lastModified() << " (" <<
           std::showpos << (int) (fetch->entry->lastModified() - squid_curtime) <<
           ")");

    statCounter.cd.kbytes_sent += fetch->sent.bytes;
    statCounter.cd.kbytes_recv += fetch->recv.bytes;
    statCounter.cd.msgs_sent += fetch->sent.msg;
    statCounter.cd.msgs_recv += fetch->recv.msg;
}

static int
peerDigestSetCBlock(PeerDigest * pd, const char *buf)
{
    StoreDigestCBlock cblock;
    int freed_size = 0;
    const auto host = pd->host;

    memcpy(&cblock, buf, sizeof(cblock));
    /* network -> host conversions */
    cblock.ver.current = ntohs(cblock.ver.current);
    cblock.ver.required = ntohs(cblock.ver.required);
    cblock.capacity = ntohl(cblock.capacity);
    cblock.count = ntohl(cblock.count);
    cblock.del_count = ntohl(cblock.del_count);
    cblock.mask_size = ntohl(cblock.mask_size);
    debugs(72, 2, "got digest cblock from " << host << "; ver: " <<
           (int) cblock.ver.current << " (req: " << (int) cblock.ver.required <<
           ")");

    debugs(72, 2, "\t size: " <<
           cblock.mask_size << " bytes, e-cnt: " <<
           cblock.count << ", e-util: " <<
           xpercentInt(cblock.count, cblock.capacity) << "%" );
    /* check version requirements (both ways) */

    if (cblock.ver.required > CacheDigestVer.current) {
        debugs(72, DBG_IMPORTANT, "" << host << " digest requires version " <<
               cblock.ver.required << "; have: " << CacheDigestVer.current);

        return 0;
    }

    if (cblock.ver.current < CacheDigestVer.required) {
        debugs(72, DBG_IMPORTANT, "" << host << " digest is version " <<
               cblock.ver.current << "; we require: " <<
               CacheDigestVer.required);

        return 0;
    }

    /* check consistency */
    if (cblock.ver.required > cblock.ver.current ||
            cblock.mask_size <= 0 || cblock.capacity <= 0 ||
            cblock.bits_per_entry <= 0 || cblock.hash_func_count <= 0) {
        debugs(72, DBG_CRITICAL, "" << host << " digest cblock is corrupted.");
        return 0;
    }

    /* check consistency further */
    if ((size_t)cblock.mask_size != CacheDigest::CalcMaskSize(cblock.capacity, cblock.bits_per_entry)) {
        debugs(72, DBG_CRITICAL, host << " digest cblock is corrupted " <<
               "(mask size mismatch: " << cblock.mask_size << " ? " <<
               CacheDigest::CalcMaskSize(cblock.capacity, cblock.bits_per_entry)
               << ").");
        return 0;
    }

    /* there are some things we cannot do yet */
    if (cblock.hash_func_count != CacheDigestHashFuncCount) {
        debugs(72, DBG_CRITICAL, "ERROR: " << host << " digest: unsupported #hash functions: " <<
               cblock.hash_func_count << " ? " << CacheDigestHashFuncCount << ".");
        return 0;
    }

    /*
     * no cblock bugs below this point
     */
    /* check size changes */
    if (pd->cd && cblock.mask_size != (ssize_t)pd->cd->mask_size) {
        debugs(72, 2, host << " digest changed size: " << cblock.mask_size <<
               " -> " << pd->cd->mask_size);
        freed_size = pd->cd->mask_size;
        delete pd->cd;
        pd->cd = nullptr;
    }

    if (!pd->cd) {
        debugs(72, 2, "creating " << host << " digest; size: " << cblock.mask_size << " (" <<
               std::showpos <<  (int) (cblock.mask_size - freed_size) << ") bytes");
        pd->cd = new CacheDigest(cblock.capacity, cblock.bits_per_entry);

        if (cblock.mask_size >= freed_size)
            statCounter.cd.memory += (cblock.mask_size - freed_size);
    }

    assert(pd->cd);
    /* these assignments leave us in an inconsistent state until we finish reading the digest */
    pd->cd->count = cblock.count;
    pd->cd->del_count = cblock.del_count;
    return 1;
}

static int
peerDigestUseful(const PeerDigest * pd)
{
    /* TODO: we should calculate the prob of a false hit instead of bit util */
    const auto bit_util = pd->cd->usedMaskPercent();

    if (bit_util > 65.0) {
        debugs(72, DBG_CRITICAL, "WARNING: " << pd->host <<
               " peer digest has too many bits on (" << bit_util << "%).");
        return 0;
    }

    return 1;
}

static int
saneDiff(time_t diff)
{
    return abs((int) diff) > squid_curtime / 2 ? 0 : diff;
}

void
peerDigestStatsReport(const PeerDigest * pd, StoreEntry * e)
{
#define f2s(flag) (pd->flags.flag ? "yes" : "no")
#define appendTime(tm) storeAppendPrintf(e, "%s\t %10ld\t %+d\t %+d\n", \
    ""#tm, (long int)pd->times.tm, \
    saneDiff(pd->times.tm - squid_curtime), \
    saneDiff(pd->times.tm - pd->times.initialized))

    assert(pd);

    auto host = pd->host;
    storeAppendPrintf(e, "\npeer digest from " SQUIDSBUFPH "\n", SQUIDSBUFPRINT(host));

    cacheDigestGuessStatsReport(&pd->stats.guess, e, host);

    storeAppendPrintf(e, "\nevent\t timestamp\t secs from now\t secs from init\n");
    appendTime(initialized);
    appendTime(needed);
    appendTime(requested);
    appendTime(received);
    appendTime(next_check);

    storeAppendPrintf(e, "peer digest state:\n");
    storeAppendPrintf(e, "\tneeded: %3s, usable: %3s, requested: %3s\n",
                      f2s(needed), f2s(usable), f2s(requested));
    storeAppendPrintf(e, "\n\tlast retry delay: %d secs\n",
                      (int) pd->times.retry_delay);
    storeAppendPrintf(e, "\tlast request response time: %d secs\n",
                      (int) pd->times.req_delay);
    storeAppendPrintf(e, "\tlast request result: %s\n",
                      pd->req_result ? pd->req_result : "(none)");

    storeAppendPrintf(e, "\npeer digest traffic:\n");
    storeAppendPrintf(e, "\trequests sent: %d, volume: %d KB\n",
                      pd->stats.sent.msgs, (int) pd->stats.sent.kbytes.kb);
    storeAppendPrintf(e, "\treplies recv:  %d, volume: %d KB\n",
                      pd->stats.recv.msgs, (int) pd->stats.recv.kbytes.kb);

    storeAppendPrintf(e, "\npeer digest structure:\n");

    if (pd->cd)
        cacheDigestReport(pd->cd, host, e);
    else
        storeAppendPrintf(e, "\tno in-memory copy\n");
}

#endif

