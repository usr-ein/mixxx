#include "library/deck/folderscan.h"

#include <QDir>
#include <QFileInfo>
#include <cmath>
#include <limits>

#include "sources/soundsourceproxy.h"
#include "track/trackmetadata.h"
#include "util/fileaccess.h"
#include "util/logger.h"

namespace {
const mixxx::Logger kLogger("FolderScan");

/// Below this a BPM tag is a typo or a half-time value nobody can mix to; above
/// it, a double-time one. Either way the analysis will say better.
constexpr double kMinTagBpm = 40.0;
constexpr double kMaxTagBpm = 300.0;

void walk(const QString& absoluteDirectory,
        const QString& relativeDirectory,
        int depth,
        const mixxx::deck::FolderScanLimits& limits,
        mixxx::deck::FolderListing* pListing) {
    if (depth > limits.maxDepth) {
        kLogger.info() << "not going deeper than" << limits.maxDepth << "levels at"
                       << relativeDirectory;
        return;
    }
    // Files and directories in one listing, in no particular order: the
    // builder sorts. NoSymLinks is the loop guard; hidden entries are left out
    // because QDir::Hidden is not asked for.
    const QFileInfoList entries = QDir(absoluteDirectory)
                                          .entryInfoList(QDir::Files | QDir::Dirs |
                                                          QDir::NoDotAndDotDot |
                                                          QDir::NoSymLinks,
                                                  QDir::NoSort);
    QString cover;
    int coverRank = std::numeric_limits<int>::max();
    QList<QFileInfo> directories;
    for (const QFileInfo& entry : entries) {
        const QString name = entry.fileName();
        if (entry.isDir()) {
            if (!mixxx::deck::isSkippedDirectoryName(name)) {
                directories.append(entry);
            }
            continue;
        }
        const int rank = mixxx::deck::coverFileRank(name);
        if (rank >= 0) {
            if (rank < coverRank) {
                coverRank = rank;
                cover = relativeDirectory + QLatin1Char('/') + name;
            }
            continue;
        }
        // Zero bytes is a copy that never finished, or a placeholder; it cannot
        // be played and would only be a row that fails to load.
        if (!mixxx::deck::isMusicFileName(name) || entry.size() <= 0) {
            continue;
        }
        if (pListing->files.size() >= limits.maxFiles) {
            pListing->truncated = true;
            return;
        }
        mixxx::deck::FolderFile file;
        file.relativePath = relativeDirectory + QLatin1Char('/') + name;
        file.size = entry.size();
        file.modified = entry.lastModified();
        pListing->files.append(file);
    }
    if (!cover.isEmpty()) {
        pListing->covers.insert(relativeDirectory, cover);
    }
    // Files of this directory before going down, so the cap -- if it bites --
    // keeps the shallow end of the stick rather than one deep corner of it.
    for (const QFileInfo& directory : directories) {
        walk(directory.absoluteFilePath(),
                relativeDirectory + QLatin1Char('/') + directory.fileName(),
                depth + 1,
                limits,
                pListing);
        if (pListing->truncated) {
            return;
        }
    }
}

/// The leading number of a track number tag, which comes as `3`, `03` or
/// `3/12`.
int leadingNumber(const QString& text) {
    int value = 0;
    for (const QChar c : text.trimmed()) {
        if (c < QLatin1Char('0') || c > QLatin1Char('9')) {
            break;
        }
        value = value * 10 + (c.unicode() - '0');
        if (value > 9999) {
            return 0;
        }
    }
    return value;
}
} // namespace

namespace mixxx {
namespace deck {

FolderListing walkFolderMedium(const QString& root, const FolderScanLimits& limits) {
    FolderListing listing;
    walk(root, QString(), 0, limits, &listing);
    if (listing.truncated) {
        kLogger.warning() << "stopped at" << limits.maxFiles << "files on" << root
                          << "-- showing what was found";
    }
    return listing;
}

QList<TrackTagUpdate> readFolderTags(const QList<FolderTagTarget>& targets) {
    QList<TrackTagUpdate> updates;
    updates.reserve(targets.size());
    for (const FolderTagTarget& target : targets) {
        mixxx::TrackMetadata metadata;
        const auto fileAccess = mixxx::FileAccess(mixxx::FileInfo(target.path));
        // resetMissingTagMetadata does not matter here: the metadata starts
        // empty and is thrown away.
        const auto [result, synchronizedAt] =
                SoundSourceProxy::importTrackMetadataAndCoverImageFromFile(
                        fileAccess, &metadata, nullptr, false);
        Q_UNUSED(synchronizedAt);
        // "Unavailable" is a file with no tags at all -- common for WAV and
        // AIFF -- and is not a failure: the importer reads the audio properties
        // before it looks for a tag, so the duration and sample rate are there
        // and worth having. Only a file it could not read at all is skipped.
        if (result == mixxx::MetadataSource::ImportResult::Failed) {
            kLogger.debug() << "could not read" << target.path;
            continue;
        }

        TrackTagUpdate update;
        update.rbId = target.rbId;
        const mixxx::TrackInfo& info = metadata.getTrackInfo();
        update.title = info.getTitle();
        update.artist = info.getArtist();
        update.genre = info.getGenre();
        update.comment = info.getComment();
        update.key = info.getKeyText();
        update.trackNumber = leadingNumber(info.getTrackNumber());
        bool validYear = false;
        const int year = mixxx::TrackMetadata::parseCalendarYear(info.getYear(), &validYear);
        if (validYear && year > 0) {
            update.year = QString::number(year);
        }
        if (info.getBpm().isValid()) {
            const double bpm = info.getBpm().value();
            if (bpm >= kMinTagBpm && bpm <= kMaxTagBpm) {
                update.bpm = bpm;
            }
        }
        update.album = metadata.getAlbumInfo().getTitle();
#if defined(__EXTRA_METADATA__)
        // Only read at all in a build with Mixxx's extra metadata, which the
        // deck's is not -- so a plain stick's Label category stays empty.
        update.label = metadata.getAlbumInfo().getRecordLabel();
#endif

        const mixxx::audio::StreamInfo& stream = metadata.getStreamInfo();
        update.durationSeconds = static_cast<int>(
                std::round(stream.getDuration().toDoubleSeconds()));
        if (stream.getBitrate().isValid()) {
            update.bitrate = static_cast<int>(stream.getBitrate().value());
        }
        if (stream.getSignalInfo().getSampleRate().isValid()) {
            update.sampleRate = static_cast<int>(stream.getSignalInfo().getSampleRate().value());
        }
        updates.append(update);
    }
    return updates;
}

} // namespace deck
} // namespace mixxx
