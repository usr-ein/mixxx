#include "library/deck/deckqueries.h"

#include <QMap>
#include <QPair>
#include <QSqlQuery>
#include <QStringList>
#include <QVariant>

#include <algorithm>
#include <cmath>

#include "library/deck/camelot.h"
#include "library/deck/pdbingest.h"
#include "library/queryutil.h"
#include "track/keyutils.h"
#include "util/logger.h"

namespace {
const mixxx::Logger kLogger("DeckQueries");

using mixxx::deck::MediumId;

/// The medium key as a SQL literal. Everything in here is built by string
/// concatenation because the result goes inside a CREATE TEMPORARY VIEW, which
/// cannot carry placeholders -- so escaping is not optional anywhere.
QString lit(QSqlDatabase& db, const QString& value) {
    return FieldEscaper(db).escapeString(value);
}

QString mediumLit(QSqlDatabase& db, const MediumId& medium) {
    return lit(db, medium.key());
}

/// `col = 'value'`, or "this field is empty" when the value is empty. The two
/// are different tests and the empty one cannot be written as an equality: a
/// pdb leaves a missing genre as either NULL or "" depending on the field.
QString matches(QSqlDatabase& db, const QString& column, const QString& value) {
    if (value.isEmpty()) {
        return QStringLiteral("(%1 IS NULL OR TRIM(%1) = '')").arg(column);
    }
    return QStringLiteral("%1 = %2").arg(column, lit(db, value));
}

/// Rows of one medium, with `id` doubling as the default order. `id` is the
/// insert order, which is the order rekordbox stored them in -- so "Default"
/// (browser-prd.md 9.2) is the medium's own order, not an alphabetical one.
QString selectFrom(QSqlDatabase& db,
        const MediumId& medium,
        const QString& extraWhere = QString()) {
    QString sql = QStringLiteral(
            "SELECT id AS track_id, id AS position FROM %1 WHERE medium = %2")
                          .arg(mixxx::deck::kLibraryTable, mediumLit(db, medium));
    if (!extraWhere.isEmpty()) {
        sql += QStringLiteral(" AND ") + extraWhere;
    }
    return sql;
}

mixxx::track::io::key::ChromaticKey keyFromId(int keyId) {
    return KeyUtils::keyFromNumericValue(keyId);
}
} // namespace

namespace mixxx {
namespace deck {

namespace query {

QString allTracks(QSqlDatabase& db, const MediumId& medium) {
    return selectFrom(db, medium);
}

QString playlist(int playlistId) {
    return QStringLiteral(
            "SELECT track_id, position FROM %1 WHERE playlist_id = %2")
            .arg(kPlaylistTracksTable)
            .arg(playlistId);
}

QString byTextColumn(QSqlDatabase& db,
        const MediumId& medium,
        const QString& column,
        const QString& value) {
    return selectFrom(db, medium, matches(db, column, value));
}

QString byArtistAlbum(QSqlDatabase& db,
        const MediumId& medium,
        const QString& artist,
        const QString& album) {
    const QString where = QStringLiteral("%1 AND %2")
                                  .arg(matches(db, QStringLiteral("artist"), artist),
                                          matches(db, QStringLiteral("album"), album));
    return selectFrom(db, medium, where);
}

QString byKeyId(QSqlDatabase& db, const MediumId& medium, int keyId) {
    return selectFrom(db, medium, QStringLiteral("key_id = %1").arg(keyId));
}

QString byBpmRange(QSqlDatabase& db, const MediumId& medium, double lo, double hi) {
    return selectFrom(db,
            medium,
            QStringLiteral("bpm >= %1 AND bpm < %2")
                    .arg(lo, 0, 'f', 4)
                    .arg(hi, 0, 'f', 4));
}

QString search(QSqlDatabase& db, const MediumId& medium, const QString& needle) {
    if (needle.isEmpty()) {
        // An empty query shows nothing, not everything (browser-prd.md 10).
        return QStringLiteral("SELECT 0 AS track_id, 0 AS position WHERE 0");
    }
    // The on-screen keyboard cannot produce % or _, so there is no wildcard to
    // escape here; a query arriving from anywhere else would need an ESCAPE.
    const QString anywhere = lit(db, QStringLiteral("%%%1%%").arg(needle));
    const QString prefix = lit(db, QStringLiteral("%1%%").arg(needle));

    // Ranked, not merely filtered: a prefix match on the title is what the DJ
    // typed for, and it must not be buried under a substring match on an album.
    return QStringLiteral(
            "SELECT track_id, position FROM ("
            "  SELECT id AS track_id, ROW_NUMBER() OVER (ORDER BY "
            "    CASE WHEN title LIKE %3 THEN 0 "
            "         WHEN artist LIKE %3 THEN 1 "
            "         WHEN album LIKE %3 THEN 2 ELSE 3 END, "
            "    title COLLATE NOCASE) AS position "
            "  FROM %1 WHERE medium = %2 AND ("
            "    title LIKE %4 OR artist LIKE %4 OR album LIKE %4)"
            ") ORDER BY position LIMIT 200")
            .arg(kLibraryTable, mediumLit(db, medium), prefix, anywhere);
}

QString lastPlayed(QSqlDatabase& db, const MediumId& medium) {
    const QString mediumLiteral = mediumLit(db, medium);
    // Two histories, merged. `tier` is what makes ours win: a track played on
    // this deck tonight is more recent than anything a CDJ wrote to the stick,
    // whatever the numbers say, because the stick's history carries no
    // timestamp we can compare against -- only a session and a position.
    return QStringLiteral(
            "SELECT track_id, ROW_NUMBER() OVER (ORDER BY tier DESC, ord DESC) "
            "  AS position FROM ("
            "  SELECT p.track_id AS track_id, 1 AS tier, MAX(p.played_at) AS ord "
            "    FROM deck_play_log p JOIN %1 l ON l.id = p.track_id "
            "   WHERE l.medium = %2 GROUP BY p.track_id "
            "  UNION ALL "
            "  SELECT h.track_id, 0 AS tier, "
            "         MAX(h.session * 100000 + h.position) AS ord "
            "    FROM deck_history h "
            "   WHERE h.medium = %2 "
            "     AND h.track_id NOT IN (SELECT track_id FROM deck_play_log) "
            "   GROUP BY h.track_id)")
            .arg(kLibraryTable, mediumLiteral);
}

} // namespace query

QList<CategoryEntry> textCategory(QSqlDatabase& db,
        const MediumId& medium,
        const QString& column,
        bool withCover) {
    QList<CategoryEntry> entries;
    // COALESCE + NULLIF folds NULL and "" into one group, which is the "—" row:
    // tracks with nothing in this field, kept rather than dropped so the counts
    // down the list add up to the medium's total.
    //
    // coverart_location, not artwork_path: the former is the local file, which
    // is the one that can actually be drawn.
    const QString cover = withCover
            ? QStringLiteral(", MIN(NULLIF(coverart_location, ''))")
            : QStringLiteral(", ''");
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
            "SELECT COALESCE(NULLIF(TRIM(%1), ''), '') AS v, COUNT(*) %2 "
            "FROM %3 WHERE medium = :medium GROUP BY v "
            "ORDER BY (v = ''), v COLLATE NOCASE")
                      .arg(column, cover, kLibraryTable));
    q.bindValue(QStringLiteral(":medium"), medium.key());
    if (!q.exec()) {
        LOG_FAILED_QUERY(q);
        return entries;
    }
    while (q.next()) {
        CategoryEntry entry;
        entry.value = q.value(0).toString();
        entry.display = entry.value.isEmpty() ? QStringLiteral("—") : entry.value;
        entry.count = q.value(1).toInt();
        entry.coverPath = q.value(2).toString();
        entries.append(entry);
    }
    return entries;
}

QList<CategoryEntry> albumsForArtist(QSqlDatabase& db,
        const MediumId& medium,
        const QString& artist) {
    QList<CategoryEntry> entries;
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
            "SELECT COALESCE(NULLIF(TRIM(album), ''), '') AS v, COUNT(*), "
            "       MIN(NULLIF(coverart_location, '')) "
            "FROM %1 WHERE medium = :medium AND %2 GROUP BY v "
            "ORDER BY (v = ''), v COLLATE NOCASE")
                      .arg(kLibraryTable,
                              artist.isEmpty()
                                      ? QStringLiteral("(artist IS NULL OR TRIM(artist) = '')")
                                      : QStringLiteral("artist = :artist")));
    q.bindValue(QStringLiteral(":medium"), medium.key());
    if (!artist.isEmpty()) {
        q.bindValue(QStringLiteral(":artist"), artist);
    }
    if (!q.exec()) {
        LOG_FAILED_QUERY(q);
        return entries;
    }
    while (q.next()) {
        CategoryEntry entry;
        entry.value = q.value(0).toString();
        entry.display = entry.value.isEmpty() ? QStringLiteral("—") : entry.value;
        entry.count = q.value(1).toInt();
        entry.coverPath = q.value(2).toString();
        entries.append(entry);
    }
    return entries;
}

QList<CategoryEntry> keyCategory(QSqlDatabase& db, const MediumId& medium) {
    QList<CategoryEntry> entries;
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
            "SELECT key_id, MIN(key), COUNT(*) FROM %1 "
            "WHERE medium = :medium GROUP BY key_id")
                      .arg(kLibraryTable));
    q.bindValue(QStringLiteral(":medium"), medium.key());
    if (!q.exec()) {
        LOG_FAILED_QUERY(q);
        return entries;
    }
    while (q.next()) {
        CategoryEntry entry;
        entry.keyId = q.value(0).toInt();
        entry.value = QString::number(entry.keyId);
        const QString stored = q.value(1).toString();
        entry.display = stored.isEmpty() ? QStringLiteral("—") : stored;
        entry.count = q.value(2).toInt();
        entries.append(entry);
    }
    // Sorted here rather than in SQL: the order is the Camelot wheel's, which
    // is neither the text order nor the key_id order.
    std::sort(entries.begin(), entries.end(), [](const CategoryEntry& a, const CategoryEntry& b) {
        return camelot::orderFromKeyId(a.keyId) < camelot::orderFromKeyId(b.keyId);
    });
    return entries;
}

QList<CategoryEntry> dateCategory(QSqlDatabase& db, const MediumId& medium) {
    QList<CategoryEntry> entries;
    QSqlQuery q(db);
    // Newest first, and as stored: rekordbox writes YYYY-MM-DD, which sorts
    // chronologically as text, so nothing has to be parsed.
    q.prepare(QStringLiteral(
            "SELECT COALESCE(NULLIF(TRIM(datetime_added), ''), '') AS d, COUNT(*) "
            "FROM %1 WHERE medium = :medium GROUP BY d ORDER BY (d = ''), d DESC")
                      .arg(kLibraryTable));
    q.bindValue(QStringLiteral(":medium"), medium.key());
    if (!q.exec()) {
        LOG_FAILED_QUERY(q);
        return entries;
    }
    while (q.next()) {
        CategoryEntry entry;
        entry.value = q.value(0).toString();
        entry.display = entry.value.isEmpty() ? QStringLiteral("—") : entry.value;
        entry.count = q.value(1).toInt();
        entries.append(entry);
    }
    return entries;
}

QList<PlaylistEntry> playlistsIn(QSqlDatabase& db,
        const MediumId& medium,
        int parentRbId) {
    QList<PlaylistEntry> entries;
    QSqlQuery q(db);
    // rb_id 0 is the synthetic "All tracks" row the ingest writes for the old
    // view's benefit; the browser reaches all tracks by its own query and must
    // not show it twice.
    q.prepare(QStringLiteral(
            "SELECT p.id, p.rb_id, p.display_name, p.is_folder, "
            "  (SELECT COUNT(*) FROM %2 pt WHERE pt.playlist_id = p.id), "
            "  (SELECT COALESCE(SUM(l.duration), 0) FROM %2 pt "
            "     JOIN %3 l ON l.id = pt.track_id WHERE pt.playlist_id = p.id), "
            "  (SELECT COUNT(*) FROM %1 c WHERE c.medium = p.medium "
            "     AND c.parent_rb_id = p.rb_id) "
            "FROM %1 p WHERE p.medium = :medium AND p.parent_rb_id = :parent "
            "  AND p.rb_id != 0 "
            "ORDER BY p.is_folder DESC, p.sort_order, p.display_name COLLATE NOCASE")
                      .arg(kPlaylistsTable, kPlaylistTracksTable, kLibraryTable));
    q.bindValue(QStringLiteral(":medium"), medium.key());
    q.bindValue(QStringLiteral(":parent"), parentRbId);
    if (!q.exec()) {
        LOG_FAILED_QUERY(q);
        return entries;
    }
    while (q.next()) {
        PlaylistEntry entry;
        entry.id = q.value(0).toInt();
        entry.rbId = q.value(1).toInt();
        entry.name = q.value(2).toString();
        entry.isFolder = q.value(3).toInt() != 0;
        entry.trackCount = q.value(4).toInt();
        entry.durationSeconds = q.value(5).toInt();
        entry.childCount = q.value(6).toInt();
        entries.append(entry);
    }

    // Covers second, one query per playlist. Four rows each off an indexed
    // column, for at most a screenful of playlists at a time.
    for (PlaylistEntry& entry : entries) {
        if (entry.isFolder) {
            continue;
        }
        QSqlQuery covers(db);
        covers.prepare(QStringLiteral(
                "SELECT l.coverart_location FROM %1 pt JOIN %2 l ON l.id = pt.track_id "
                "WHERE pt.playlist_id = :id AND l.coverart_location != '' "
                "ORDER BY pt.position LIMIT 4")
                               .arg(kPlaylistTracksTable, kLibraryTable));
        covers.bindValue(QStringLiteral(":id"), entry.id);
        if (!covers.exec()) {
            LOG_FAILED_QUERY(covers);
            continue;
        }
        while (covers.next()) {
            entry.coverPaths.append(covers.value(0).toString());
        }
    }
    return entries;
}

int categoryCount(QSqlDatabase& db, const MediumId& medium, const QString& column) {
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
            "SELECT COUNT(DISTINCT COALESCE(NULLIF(TRIM(%1), ''), '')) FROM %2 "
            "WHERE medium = :medium")
                      .arg(column, kLibraryTable));
    q.bindValue(QStringLiteral(":medium"), medium.key());
    if (!q.exec() || !q.next()) {
        LOG_FAILED_QUERY(q);
        return 0;
    }
    return q.value(0).toInt();
}

int trackCount(QSqlDatabase& db, const MediumId& medium) {
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT COUNT(*) FROM %1 WHERE medium = :medium")
                      .arg(kLibraryTable));
    q.bindValue(QStringLiteral(":medium"), medium.key());
    if (!q.exec() || !q.next()) {
        LOG_FAILED_QUERY(q);
        return 0;
    }
    return q.value(0).toInt();
}

int playlistCount(QSqlDatabase& db, const MediumId& medium) {
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
            "SELECT COUNT(*) FROM %1 WHERE medium = :medium AND is_folder = 0 "
            "AND rb_id != 0")
                      .arg(kPlaylistsTable));
    q.bindValue(QStringLiteral(":medium"), medium.key());
    if (!q.exec() || !q.next()) {
        LOG_FAILED_QUERY(q);
        return 0;
    }
    return q.value(0).toInt();
}

namespace bpm {

namespace {
/// Half a bucket at this tempo: one fader's reach, capped so WIDE does not
/// swallow the medium.
double halfWidth(double bpm, double rateRange) {
    return std::min(rateRange * bpm, kMaxHalfWidthBpm);
}

/// The bucket above one ending at *lo*, and below one starting at *hi*.
///
/// Solving `hi - lo = 2 * r * centre` for a bucket whose centre is its own
/// midpoint gives `hi = lo * (1 + r) / (1 - r)`, which is why the edges are
/// geometric rather than evenly spaced: a fader's reach is a percentage, so a
/// bucket at 170 BPM is genuinely wider than one at 90.
double edgeAbove(double lo, double rateRange) {
    if (rateRange * lo >= kMaxHalfWidthBpm) {
        return lo + 2 * kMaxHalfWidthBpm;
    }
    return lo * (1.0 + rateRange) / (1.0 - rateRange);
}

double edgeBelow(double hi, double rateRange) {
    if (rateRange * hi >= kMaxHalfWidthBpm) {
        return hi - 2 * kMaxHalfWidthBpm;
    }
    return hi * (1.0 - rateRange) / (1.0 + rateRange);
}
} // namespace

double densityPeak(QSqlDatabase& db, const MediumId& medium) {
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
            "SELECT CAST(bpm + 0.5 AS INTEGER) AS b, COUNT(*) FROM %1 "
            "WHERE medium = :medium AND bpm > 0 GROUP BY b ORDER BY b")
                      .arg(kLibraryTable));
    q.bindValue(QStringLiteral(":medium"), medium.key());
    if (!q.exec()) {
        LOG_FAILED_QUERY(q);
        return 0.0;
    }
    QMap<int, int> histogram;
    while (q.next()) {
        histogram.insert(q.value(0).toInt(), q.value(1).toInt());
    }
    if (histogram.isEmpty()) {
        return 0.0;
    }
    // The 5-BPM window with the most tracks in it. A plain argmax would land on
    // whichever single tempo happened to have one more track than its
    // neighbours, which is noise; a window finds where the music actually sits.
    int bestBpm = histogram.firstKey();
    int bestScore = -1;
    for (auto it = histogram.constBegin(); it != histogram.constEnd(); ++it) {
        int score = 0;
        for (int offset = -2; offset <= 2; ++offset) {
            score += histogram.value(it.key() + offset, 0);
        }
        if (score > bestScore) {
            bestScore = score;
            bestBpm = it.key();
        }
    }
    return bestBpm;
}

QList<CategoryEntry> buckets(QSqlDatabase& db,
        const MediumId& medium,
        double rateRange,
        double referenceBpm,
        int* pAnchorIndex) {
    QList<CategoryEntry> entries;
    if (pAnchorIndex) {
        *pAnchorIndex = 0;
    }

    QSqlQuery q(db);
    q.prepare(QStringLiteral(
            "SELECT MIN(bpm), MAX(bpm) FROM %1 WHERE medium = :medium AND bpm > 0")
                      .arg(kLibraryTable));
    q.bindValue(QStringLiteral(":medium"), medium.key());
    if (!q.exec() || !q.next() || q.value(0).isNull()) {
        return entries;
    }
    const double minBpm = q.value(0).toDouble();
    const double maxBpm = q.value(1).toDouble();

    double reference = referenceBpm;
    if (reference <= 0.0) {
        reference = densityPeak(db, medium);
    }
    if (reference <= 0.0) {
        reference = (minBpm + maxBpm) / 2.0;
    }
    // Clamp into the medium's own range, so a deck playing 174 against a stick
    // of house does not open on an empty bucket miles above everything.
    reference = std::clamp(reference, minBpm, maxBpm);

    if (rateRange <= 0.0) {
        rateRange = 0.06;
    }

    // The anchor bucket is centred on the reference exactly, which is the whole
    // point: what is playing sits in the middle of its bucket, so everything in
    // that bucket is within one fader of it. Absolute tiling would leave the
    // playing tempo near an edge half the time, with half the mixable tracks in
    // the bucket next door.
    const double half = halfWidth(reference, rateRange);
    QList<QPair<double, double>> ranges;
    ranges.append({reference - half, reference + half});

    for (double lo = ranges.first().first; lo > minBpm;) {
        const double newLo = edgeBelow(lo, rateRange);
        if (newLo >= lo) {
            break; // Degenerate range; stop rather than loop forever.
        }
        ranges.prepend({newLo, lo});
        lo = newLo;
    }
    for (double hi = ranges.last().second; hi <= maxBpm;) {
        const double newHi = edgeAbove(hi, rateRange);
        if (newHi <= hi) {
            break;
        }
        ranges.append({hi, newHi});
        hi = newHi;
    }

    int anchorIndex = 0;
    for (int i = 0; i < ranges.size(); ++i) {
        const double lo = ranges.at(i).first;
        const double hi = ranges.at(i).second;

        QSqlQuery count(db);
        count.prepare(QStringLiteral(
                "SELECT COUNT(*) FROM %1 WHERE medium = :medium "
                "AND bpm >= :lo AND bpm < :hi")
                              .arg(kLibraryTable));
        count.bindValue(QStringLiteral(":medium"), medium.key());
        count.bindValue(QStringLiteral(":lo"), lo);
        count.bindValue(QStringLiteral(":hi"), hi);
        if (!count.exec() || !count.next()) {
            LOG_FAILED_QUERY(count);
            continue;
        }
        const int n = count.value(0).toInt();
        if (n == 0) {
            // An empty bucket is a row that can only disappoint. The anchor is
            // kept even when empty, so the list still opens where the deck is.
            if (!(reference >= lo && reference < hi)) {
                continue;
            }
        }
        CategoryEntry entry;
        entry.bpmLo = lo;
        entry.bpmHi = hi;
        entry.count = n;
        entry.display = QStringLiteral("%1 – %2")
                                .arg(qRound(lo))
                                .arg(qRound(hi));
        entry.value = QStringLiteral("%1|%2").arg(lo).arg(hi);
        if (reference >= lo && reference < hi) {
            anchorIndex = entries.size();
        }
        entries.append(entry);
    }
    if (pAnchorIndex) {
        *pAnchorIndex = anchorIndex;
    }
    return entries;
}

} // namespace bpm

namespace key {

bool isCompatible(int playingKeyId, int candidateKeyId) {
    return camelot::isCompatible(keyFromId(playingKeyId), keyFromId(candidateKeyId));
}

} // namespace key

} // namespace deck
} // namespace mixxx
