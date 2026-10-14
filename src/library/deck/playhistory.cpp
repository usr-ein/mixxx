#include "library/deck/playhistory.h"

#include <QSet>
#include <algorithm>
#include <set>
#include <tuple>
#include <utility>

namespace mixxx {
namespace deck {
namespace history {

namespace {

/// The tracks of a segment as a set that counts twice what plays twice, so two
/// segments compare equal whatever order their tracks started in.
QList<int> sortedTracks(QList<int> tracks) {
    std::sort(tracks.begin(), tracks.end());
    return tracks;
}

/// Join *rows* side by side with the same tracks into one.
QList<Row> mergeNeighbours(const QList<Row>& rows) {
    QList<Row> merged;
    for (const Row& row : rows) {
        if (!merged.isEmpty() &&
                sortedTracks(merged.last().tracks) == sortedTracks(row.tracks)) {
            merged.last().durationMs += row.durationMs;
            continue;
        }
        merged.append(row);
    }
    return merged;
}

} // namespace

QList<Segment> segmentsOf(QList<Play> plays) {
    // A play that never lasted, or one whose end came before its start (a
    // clock that went back, a row damaged on the card), is no time at all.
    plays.erase(std::remove_if(plays.begin(),
                        plays.end(),
                        [](const Play& play) {
                            return play.endMs <= play.startMs;
                        }),
            plays.end());

    // Every moment anything starts or stops, in order, walked once with the
    // plays under way kept as it goes: a long set holds thousands of plays,
    // and the list of this power-up is worked out again every second while
    // anything plays, on the GUI thread.
    struct Event {
        qint64 atMs;
        bool starts;
        int play;
    };
    QList<Event> events;
    events.reserve(plays.size() * 2);
    for (int i = 0; i < plays.size(); ++i) {
        events.append({plays.at(i).startMs, true, i});
        events.append({plays.at(i).endMs, false, i});
    }
    std::sort(events.begin(), events.end(), [](const Event& lhs, const Event& rhs) {
        return lhs.atMs < rhs.atMs;
    });
    // The plays under way, in the order they started, and by player for two
    // that started at the same moment, so the order never depends on how the
    // plays were stored. The play's index makes each its own.
    std::set<std::tuple<qint64, int, int>> under;

    QList<Segment> segments;
    int next = 0;
    while (next < events.size()) {
        // Everything that happens at this moment, before looking at what
        // plays from it on: between two moments nothing changes.
        const qint64 from = events.at(next).atMs;
        for (; next < events.size() && events.at(next).atMs == from; ++next) {
            const Play& play = plays.at(events.at(next).play);
            const auto key = std::make_tuple(play.startMs, play.player, events.at(next).play);
            if (events.at(next).starts) {
                under.insert(key);
            } else {
                under.erase(key);
            }
        }
        if (next >= events.size() || under.empty()) {
            // The last moment, or silence until the next.
            continue;
        }
        const qint64 to = events.at(next).atMs;
        QList<int> tracks;
        tracks.reserve(static_cast<qsizetype>(under.size()));
        for (const auto& entry : under) {
            tracks.append(plays.at(std::get<2>(entry)).track);
        }
        // The same tracks straight on: the same segment, in the order it
        // started with. Only straight on -- after a silence it is a segment of
        // its own, which rowsOf() may then list as the same row.
        if (!segments.isEmpty() && segments.last().endMs == from &&
                sortedTracks(segments.last().tracks) == sortedTracks(tracks)) {
            segments.last().endMs = to;
            continue;
        }
        Segment segment;
        segment.startMs = from;
        segment.endMs = to;
        segment.tracks = tracks;
        segments.append(segment);
    }
    return segments;
}

QList<Row> rowsOf(const QList<Segment>& segments, const RowRules& rules) {
    QList<Row> rows;
    rows.reserve(segments.size());
    for (const Segment& segment : segments) {
        Row row;
        row.tracks = segment.tracks;
        row.durationMs = segment.durationMs();
        row.startMs = segment.startMs;
        rows.append(row);
    }
    // Merged before the short ones go, so a track paused twice for a second
    // is one row of its whole length and not three rows, two of them dropped.
    rows = mergeNeighbours(rows);
    rows.erase(std::remove_if(rows.begin(),
                       rows.end(),
                       [&rules](const Row& row) {
                           return row.durationMs < rules.minimumMs;
                       }),
            rows.end());
    // And after: the overlap of a cut gone, what is left either side of it may
    // be the same tracks -- a CUE tapped on another deck in the middle of a
    // track is no reason to list that track twice.
    return mergeNeighbours(rows);
}

int trackCount(const QList<Row>& rows) {
    QSet<int> tracks;
    for (const Row& row : rows) {
        for (const int track : row.tracks) {
            tracks.insert(track);
        }
    }
    return static_cast<int>(tracks.size());
}

QString durationText(qint64 ms) {
    // To the nearest second: 9.6 s played reads as 10s.
    const qint64 seconds = (std::max<qint64>(ms, 0) + 500) / 1000;
    if (seconds < 60) {
        return QStringLiteral("%1s").arg(seconds);
    }
    if (seconds < 3600) {
        const qint64 minutes = seconds / 60;
        const qint64 rest = seconds % 60;
        // Sam's own: `2m` for whole minutes, `1m20s` otherwise.
        return rest == 0
                ? QStringLiteral("%1m").arg(minutes)
                : QStringLiteral("%1m%2s").arg(minutes).arg(rest, 2, 10, QChar('0'));
    }
    // An hour of one row is an hour of nothing changing; the seconds no longer
    // say anything.
    const qint64 minutes = (seconds + 30) / 60;
    return QStringLiteral("%1h%2m").arg(minutes / 60).arg(minutes % 60, 2, 10, QChar('0'));
}

QList<Recorder::Change> Recorder::update(qint64 nowMs, const QList<Sample>& playing) {
    QHash<int, QString> now;
    for (const Sample& sample : playing) {
        if (!sample.trackKey.isEmpty()) {
            now.insert(sample.player, sample.trackKey);
        }
    }
    QList<Change> stops;
    QList<Change> starts;
    // Stopped: not playing any more, or playing something else -- a player
    // that went from one track to the next without stopping in between stops
    // the first as the second starts.
    for (auto it = m_playing.constBegin(); it != m_playing.constEnd(); ++it) {
        if (now.value(it.key()) != it.value()) {
            stops.append({Change::Kind::Stopped, it.key(), it.value(), nowMs});
        }
    }
    for (auto it = now.constBegin(); it != now.constEnd(); ++it) {
        if (m_playing.value(it.key()) != it.value()) {
            starts.append({Change::Kind::Started, it.key(), it.value(), nowMs});
        }
    }
    // By player, so the plays two samples start or stop come out in an order
    // that does not depend on how a QHash happened to lay them out.
    const auto byPlayer = [](const Change& lhs, const Change& rhs) {
        return lhs.player < rhs.player;
    };
    std::sort(stops.begin(), stops.end(), byPlayer);
    std::sort(starts.begin(), starts.end(), byPlayer);
    m_playing = now;
    return stops + starts;
}

QList<Recorder::Change> Recorder::stopAll(qint64 nowMs) {
    return update(nowMs, {});
}

} // namespace history
} // namespace deck
} // namespace mixxx
