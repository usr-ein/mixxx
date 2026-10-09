#include "library/deck/autoplay.h"

#include <QRandomGenerator>
#include <QSqlQuery>
#include <QVariant>

#include <cmath>
#include <limits>

#include "library/deck/deckqueries.h"
#include "library/deck/pdbingest.h"
#include "library/queryutil.h"

namespace mixxx {
namespace deck {
namespace autoplay {

bool PlayedMemory::contains(const QString& scope, const QString& key) const {
    const auto it = m_played.constFind(scope);
    return it != m_played.constEnd() && it->contains(key);
}

void PlayedMemory::insert(const QString& scope, const QString& key) {
    m_played[scope].insert(key);
}

void PlayedMemory::clear(const QString& scope) {
    m_played.remove(scope);
}

int nearestUnplayed(const QList<Candidate>& candidates,
        const QSet<QString>& excluded,
        double referenceBpm,
        QRandomGenerator* pRandom) {
    // Apart, because a track with no tempo cannot be near anything: it is not
    // the farthest, it is not on the scale at all. So those wait until every
    // track that is on it has played.
    QList<int> withBpm;
    QList<int> withoutBpm;
    for (int i = 0; i < candidates.size(); ++i) {
        const Candidate& candidate = candidates.at(i);
        if (excluded.contains(candidate.key)) {
            continue;
        }
        (candidate.bpm > 0.0 ? withBpm : withoutBpm).append(i);
    }
    const auto anyOf = [pRandom](const QList<int>& indices) {
        return indices.at(pRandom->bounded(static_cast<int>(indices.size())));
    };

    if (!withBpm.isEmpty()) {
        if (!(referenceBpm > 0.0)) {
            // Nothing to be near: the track playing has no tempo, which only a
            // starting track nobody has analysed can be. Any of them, then;
            // the one after it has a tempo to be near.
            return anyOf(withBpm);
        }
        double nearestDistance = std::numeric_limits<double>::infinity();
        QList<int> nearest;
        for (const int i : withBpm) {
            const double distance = std::abs(candidates.at(i).bpm - referenceBpm);
            if (distance < nearestDistance - kSameBpm) {
                nearestDistance = distance;
                nearest = {i};
            } else if (distance <= nearestDistance + kSameBpm) {
                nearest.append(i);
            }
        }
        return anyOf(nearest);
    }
    if (!withoutBpm.isEmpty()) {
        return anyOf(withoutBpm);
    }
    return -1;
}

Pick next(const QList<Candidate>& candidates,
        PlayedMemory* pMemory,
        const QString& scope,
        const QString& currentKey,
        double referenceBpm,
        QRandomGenerator* pRandom) {
    Pick pick;
    const QSet<QString> played = pMemory->played(scope);
    for (const Candidate& candidate : candidates) {
        if (!played.contains(candidate.key)) {
            ++pick.unplayed;
        }
    }
    pick.index = nearestUnplayed(candidates, played, referenceBpm, pRandom);
    if (pick.index < 0 && !candidates.isEmpty()) {
        // The whole genre has played. A new round -- opened on anything but
        // the track that has just ended: the nearest tempo to a track is its
        // own, so without this every round would start by playing it twice.
        pMemory->clear(scope);
        pick.newRound = true;
        pick.unplayed = static_cast<int>(candidates.size());
        pick.index = nearestUnplayed(candidates, {currentKey}, referenceBpm, pRandom);
        if (pick.index < 0) {
            // A genre of one track, which can only follow itself.
            pick.index = nearestUnplayed(candidates, {}, referenceBpm, pRandom);
        }
    }
    if (pick.index >= 0) {
        pMemory->insert(scope, candidates.at(pick.index).key);
    }
    return pick;
}

QString scopeKey(const MediumId& medium, const QString& genre) {
    const QString volumeId = medium.volumeId();
    const QString drive = volumeId.isEmpty() ? medium.key() : QStringLiteral("uuid:") + volumeId;
    // A newline cannot be in a medium key, so no drive and genre can spell
    // another's.
    return drive + QLatin1Char('\n') + genre;
}

QString trackKey(const QString& location, const QString& root, quint32 rekordboxId) {
    if (location.isEmpty()) {
        return QStringLiteral("#%1").arg(rekordboxId);
    }
    if (!root.isEmpty() && location.startsWith(root)) {
        return location.mid(root.size());
    }
    return location;
}

QList<Candidate> candidates(QSqlDatabase& db,
        const MediumId& medium,
        const QString& genre,
        const QString& root) {
    QList<Candidate> result;
    // Through the genre list's own query rather than a WHERE of its own, so a
    // round is exactly the list the DJ started it from: the two cannot
    // disagree about what "House" matches, an empty genre included.
    const QString genreTracks = query::byTextColumn(db, medium, QStringLiteral("genre"), genre);
    QSqlQuery query(db);
    query.prepare(QStringLiteral(
            "SELECT id, rb_id, location, bpm, title FROM %1 "
            "WHERE id IN (SELECT track_id FROM (%2)) ORDER BY id")
                          .arg(kLibraryTable, genreTracks));
    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
        return result;
    }
    while (query.next()) {
        Candidate candidate;
        candidate.rowId = query.value(0).toInt();
        candidate.key = trackKey(query.value(2).toString(),
                root,
                static_cast<quint32>(query.value(1).toUInt()));
        candidate.bpm = query.value(3).toDouble();
        candidate.title = query.value(4).toString();
        result.append(candidate);
    }
    return result;
}

} // namespace autoplay
} // namespace deck
} // namespace mixxx
