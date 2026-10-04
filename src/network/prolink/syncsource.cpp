#include "network/prolink/syncsource.h"

#include <algorithm>

namespace mixxx {
namespace prolink {

namespace {

/// How much older than one beat interval a beat packet may be before the next
/// one counts as overdue. The phase stops at the end of the beat rather than
/// run on, so this is how close to that it may get.
constexpr double kBeatOverdueMarginMs = 5.0;
/// A beat older than this many intervals says nothing about where the deck is
/// now: it stopped, or it has only just started again.
constexpr double kBeatStaleIntervals = 1.25;

/// Whether *peer*'s beats are still arriving: its last one is younger than a
/// beat and a bit.
bool isBeating(const SyncPeer& peer) {
    return peer.beatBpm > 0.0 && peer.beatAgeMs >= 0.0 &&
            peer.beatAgeMs < kBeatStaleIntervals * 60000.0 / peer.beatBpm;
}

/// Fill in tempo and phase for a deck that is playing.
void takeTempoAndPhase(const SyncPeer& peer, SyncSource* pSource) {
    const bool beating = isBeating(peer);
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
        if (!isHeardMasterClaim(peer, ours)) {
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
        if (!pMaster->playing) {
            source.masterStopped = true;
            return source;
        }
        takeTempoAndPhase(*pMaster, &source);
        return source;
    }

    // Is anyone handing master over at all? Then there is about to be a
    // master, and nothing should be followed in the meantime.
    if (std::any_of(peers.begin(), peers.end(), [ours](const SyncPeer& peer) {
            return isHeardMasterClaim(peer, ours);
        })) {
        return source;
    }

    const SyncPeer* pFallback = nullptr;
    for (const SyncPeer& peer : peers) {
        if (!isHeardPlayer(peer, ours) || !peer.playing) {
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

MeterDeck chooseMeterDeck(const std::vector<SyncPeer>& peers, int ours) {
    MeterDeck shown;
    int shownRank = 0;
    for (const SyncPeer& peer : peers) {
        if (!isPlayerNumber(peer.number) || peer.number == ours) {
            continue;
        }
        MeterDeck candidate;
        candidate.device = peer.number;
        if (!peer.hasStatus) {
            if (!isBeating(peer)) {
                continue;
            }
            candidate.barPhase = peer.barPhase;
            candidate.live = true;
        } else {
            if (!isHeard(peer)) {
                continue;
            }
            candidate.isMaster = peer.isMaster;
            candidate.live = (peer.playing || peer.auditioning) && isBeating(peer) &&
                    peer.barPhase >= 0.0;
            // Not beating yet, too: the first beat after PLAY is up to a beat
            // away, and status places the deck in the meantime.
            candidate.barPhase = candidate.live ? peer.barPhase : peer.barPosition;
        }
        if (candidate.barPhase < 0.0) {
            continue;
        }
        const int rank = candidate.isMaster ? 3 : (candidate.live ? 2 : 1);
        if (rank > shownRank || (rank == shownRank && candidate.device < shown.device)) {
            shown = candidate;
            shownRank = rank;
        }
    }
    return shown;
}

} // namespace prolink
} // namespace mixxx
