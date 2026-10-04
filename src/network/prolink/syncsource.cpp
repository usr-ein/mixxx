#include "network/prolink/syncsource.h"

namespace mixxx {
namespace prolink {

namespace {

constexpr int kFirstPlayer = 1;
constexpr int kLastPlayer = 6;

/// How much older than one beat interval a beat packet may be before the next
/// one counts as overdue. The phase stops at the end of the beat rather than
/// run on, so this is how close to that it may get.
constexpr double kBeatOverdueMarginMs = 5.0;
/// A beat older than this many intervals says nothing about where the deck is
/// now: it stopped, or it has only just started again.
constexpr double kBeatStaleIntervals = 1.25;

bool isFresh(const SyncPeer& peer) {
    return peer.hasStatus && peer.statusAgeMs >= 0.0 && peer.statusAgeMs < kStatusFreshMs;
}

bool isCandidate(const SyncPeer& peer, int ours) {
    return peer.number >= kFirstPlayer && peer.number <= kLastPlayer && peer.number != ours &&
            isFresh(peer);
}

/// Fill in tempo and phase for a deck that is playing.
void takeTempoAndPhase(const SyncPeer& peer, SyncSource* pSource) {
    const bool beating = peer.beatBpm > 0.0 && peer.beatAgeMs >= 0.0 &&
            peer.beatAgeMs < kBeatStaleIntervals * 60000.0 / peer.beatBpm;
    // Beats while they are fresh: they carry the tempo the deck is playing at
    // right now. Otherwise status, which a deck that has only just started (no
    // beat yet) or that we have never heard beat still states.
    pSource->bpm = beating ? peer.beatBpm : peer.statusBpm;
    if (beating && peer.barPhase >= 0.0) {
        pSource->barPhase = peer.barPhase;
        pSource->phaseLive = true;
        pSource->beatOverdue = peer.beatAgeMs > 60000.0 / peer.beatBpm - kBeatOverdueMarginMs;
    }
}

} // namespace

SyncSource chooseSyncSource(const std::vector<SyncPeer>& peers, int ours) {
    SyncSource source;

    const SyncPeer* pMaster = nullptr;
    for (const SyncPeer& peer : peers) {
        if (!isCandidate(peer, ours) || !peer.isMaster) {
            continue;
        }
        // Handing over: to us, and we are about to be master; or to another
        // deck, which is. Either way not the deck to follow.
        if (peer.yieldingTo != 0) {
            continue;
        }
        if (pMaster == nullptr || peer.number < pMaster->number) {
            pMaster = &peer;
        }
    }
    if (pMaster != nullptr) {
        source.kind = SyncSource::Kind::Master;
        source.device = pMaster->number;
        if (!pMaster->playing || pMaster->cuePlay) {
            source.masterStopped = true;
            return source;
        }
        takeTempoAndPhase(*pMaster, &source);
        return source;
    }

    // Is anyone handing master over at all? Then there is about to be a
    // master, and nothing should be followed in the meantime.
    for (const SyncPeer& peer : peers) {
        if (isCandidate(peer, ours) && peer.isMaster) {
            return source;
        }
    }

    const SyncPeer* pFallback = nullptr;
    for (const SyncPeer& peer : peers) {
        if (!isCandidate(peer, ours) || !peer.playing || peer.cuePlay) {
            continue;
        }
        if (peer.beatBpm <= 0.0 && peer.statusBpm <= 0.0) {
            continue;
        }
        if (peer.isSynced && (ours <= 0 || peer.number > ours)) {
            continue;
        }
        if (pFallback == nullptr || peer.number < pFallback->number) {
            pFallback = &peer;
        }
    }
    if (pFallback != nullptr) {
        source.kind = SyncSource::Kind::Fallback;
        source.device = pFallback->number;
        takeTempoAndPhase(*pFallback, &source);
    }
    return source;
}

} // namespace prolink
} // namespace mixxx
