#pragma once

#include <QHash>
#include <QList>
#include <QSet>
#include <QSqlDatabase>
#include <QString>

#include "library/deck/mediumid.h"

class QRandomGenerator;

namespace mixxx {
namespace deck {

/// Autoplay's rule and its memory: which track of a genre plays next, and which
/// have played.
///
/// The DJ picks a drive, one of its genres and a track to start from. From then
/// on each next track is the nearest in BPM to the one playing, among the
/// genre's tracks not yet played; once every one of them has played, a new
/// round starts.
///
/// **Pure**: no widgets, no controls, no deck. DeckAutoplay drives it, and
/// keeping the two apart is what lets the rule be tested with nothing loaded.
namespace autoplay {

/// One track autoplay could play next.
struct Candidate {
    /// The `deck_library` row, which is what gets loaded. Only good for as long
    /// as the medium stays read: a re-read inserts every row again under a new
    /// id, which is why nothing is remembered by it.
    int rowId = -1;
    /// What the track is remembered by: see trackKey().
    QString key;
    /// 0 when nobody knows: a stick's file with no BPM tag, not analysed yet.
    double bpm = 0.0;
    /// For the log.
    QString title;
};

/// The tracks that have played, per drive and genre, since the deck started.
///
/// In RAM and nowhere else (Sam, 2026-10-09): nothing is written to the card or
/// to the stick, and a restart forgets it.
class PlayedMemory {
  public:
    QSet<QString> played(const QString& scope) const {
        return m_played.value(scope);
    }
    bool contains(const QString& scope, const QString& key) const;
    void insert(const QString& scope, const QString& key);
    /// A new round: everything in *scope* is unplayed again.
    void clear(const QString& scope);

  private:
    QHash<QString, QSet<QString>> m_played;
};

/// Two tempos closer than this are the same tempo, and a tie. The deck shows
/// two decimals; nothing anyone can see or hear tells these apart.
constexpr double kSameBpm = 0.001;

/// The index in *candidates* of the next track, or -1 when every one is in
/// *excluded*:
///
///  - among the tracks with a BPM, the nearest to *referenceBpm*, ties at
///    random; any of them, at random, when there is no reference to be near;
///  - the tracks with no BPM only once no track with one is left, at random.
int nearestUnplayed(const QList<Candidate>& candidates,
        const QSet<QString>& excluded,
        double referenceBpm,
        QRandomGenerator* pRandom);

struct Pick {
    /// Into the candidates, or -1 when there are none at all.
    int index = -1;
    /// Every track had played, so this one opens a new round.
    bool newRound = false;
    /// How many were unplayed when it was picked, itself included.
    int unplayed = 0;
};

/// One step of autoplay: the next track after *currentKey*, which is playing at
/// *referenceBpm*, recorded as played under *scope*.
///
/// When every candidate has played, the scope is cleared and a new round
/// starts -- but never with the track that has just played, whose nearest tempo
/// is its own, unless it is the only one.
Pick next(const QList<Candidate>& candidates,
        PlayedMemory* pMemory,
        const QString& scope,
        const QString& currentKey,
        double referenceBpm,
        QRandomGenerator* pRandom);

/// What one drive's genre is remembered under.
///
/// The drive is its filesystem UUID where it has one, so a stick pulled and put
/// back in the other port -- a new mount point, and so a new medium id -- is
/// still the same stick with the same played tracks.
QString scopeKey(const MediumId& medium, const QString& genre);

/// What a track is remembered by: its path on the drive, *location* less the
/// drive's *root*. Stable across a re-read, and across a stick edited on a
/// laptop between two insertions, which renumbers a folder stick's rows; the
/// rekordbox id stands in only for a row with no location.
QString trackKey(const QString& location, const QString& root, quint32 rekordboxId);

/// The tracks of *genre* on *medium* -- exactly the rows of the genre's track
/// list, query::byTextColumn() -- keyed against *root*, the drive's root.
QList<Candidate> candidates(QSqlDatabase& db,
        const MediumId& medium,
        const QString& genre,
        const QString& root);

} // namespace autoplay
} // namespace deck
} // namespace mixxx
