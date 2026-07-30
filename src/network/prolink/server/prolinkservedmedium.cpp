#include "network/prolink/server/prolinkservedmedium.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <algorithm>

#include "util/logger.h"

namespace {
const mixxx::Logger kLogger("ProLinkServedMedium");

/// Read a whole file off the medium, or empty. Anything unreadable is normal
/// here — a track analysed by an older rekordbox lacks the newer files, and a
/// yanked stick makes everything unreadable at once.
QByteArray readWhole(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QByteArray();
    }
    return file.readAll();
}

/// Join a medium root to a path as `export.pdb` stores it: relative to the
/// medium with a leading slash.
QString onMedium(const QString& root, const QString& stored) {
    QString relative = stored;
    while (relative.startsWith(QLatin1Char('/'))) {
        relative.remove(0, 1);
    }
    return relative.isEmpty() ? QString() : root + QLatin1Char('/') + relative;
}

quint32 readU32Le(const QByteArray& data, int offset) {
    if (offset + 4 > data.size()) {
        return 0;
    }
    return static_cast<quint32>(static_cast<quint8>(data.at(offset))) |
            (static_cast<quint32>(static_cast<quint8>(data.at(offset + 1))) << 8) |
            (static_cast<quint32>(static_cast<quint8>(data.at(offset + 2))) << 16) |
            (static_cast<quint32>(static_cast<quint8>(data.at(offset + 3))) << 24);
}

} // namespace

namespace mixxx {
namespace prolink {
namespace server {

QByteArray parseMySetting(const QByteArray& fileContents) {
    constexpr int kHeaderLength = 0x60;
    constexpr int kOffsetPayloadLength = 0x64;
    constexpr int kOffsetPayload = 0x68;
    constexpr quint32 kPayloadMagic = 0x12345678;
    /// Settings bytes a type-0x36 reply carries, after the magic and one word.
    constexpr int kSettingsLength = 32;

    if (fileContents.size() < kOffsetPayload) {
        return QByteArray();
    }
    if (readU32Le(fileContents, 0) != kHeaderLength) {
        return QByteArray();
    }
    const quint32 payloadLength = readU32Le(fileContents, kOffsetPayloadLength);
    if (static_cast<qint64>(kOffsetPayload) + payloadLength > fileContents.size()) {
        // A declared payload running past the buffer is refused rather than
        // silently clipped: a truncated settings block handed to a real deck is
        // a worse outcome than not answering.
        return QByteArray();
    }
    const QByteArray payload =
            fileContents.mid(kOffsetPayload, static_cast<int>(payloadLength));
    if (payload.size() < 8 || readU32Le(payload, 0) != kPayloadMagic) {
        return QByteArray();
    }
    return payload.mid(8, kSettingsLength);
}

ProLinkServedMedium ProLinkServedMedium::fromVolume(
        MediaSlot slot, const QString& volumePath) {
    ProLinkServedMedium medium;
    medium.m_slot = slot;
    medium.m_rootPath = QDir(volumePath).absolutePath();
    medium.m_volumeName = QFileInfo(medium.m_rootPath).fileName();

    const QByteArray pdb =
            readWhole(medium.m_rootPath + QLatin1Char('/') + QLatin1String(kPdbPath));
    if (pdb.isEmpty()) {
        kLogger.debug() << volumePath << "has no" << kPdbPath << "- not a rekordbox medium";
        return medium;
    }
    medium.m_library = parsePdb(pdb);
    if (!medium.m_library.ok) {
        kLogger.warning() << volumePath << "has an unusable export.pdb:"
                          << medium.m_library.error;
        return medium;
    }

    medium.m_settings = parseMySetting(readWhole(
            medium.m_rootPath + QLatin1Char('/') + QLatin1String(kMySettingPath)));
    medium.index();
    medium.m_ok = true;
    kLogger.info() << medium.toString();
    return medium;
}

void ProLinkServedMedium::index() {
    const int count = m_library.tracks.size();
    m_byId.reserve(count);
    m_defaultOrder.reserve(count);
    for (int i = 0; i < count; ++i) {
        m_byId.insert(m_library.tracks.at(i).id, i);
        m_defaultOrder.append(i);
    }
    const QList<PdbTrack>& tracks = m_library.tracks;
    std::sort(m_defaultOrder.begin(),
            m_defaultOrder.end(),
            [&tracks](int a, int b) {
                const int byArtist = tracks.at(a).artist.compare(
                        tracks.at(b).artist, Qt::CaseInsensitive);
                if (byArtist != 0) {
                    return byArtist < 0;
                }
                return tracks.at(a).title.compare(
                               tracks.at(b).title, Qt::CaseInsensitive) < 0;
            });
}

const PdbTrack* ProLinkServedMedium::track(quint32 id) const {
    const auto found = m_byId.constFind(id);
    if (found == m_byId.constEnd() || found.value() >= m_library.tracks.size()) {
        return nullptr;
    }
    return &m_library.tracks.at(found.value());
}

QList<const PdbTrack*> ProLinkServedMedium::tracksInDefaultOrder() const {
    QList<const PdbTrack*> out;
    out.reserve(m_defaultOrder.size());
    for (const int index : m_defaultOrder) {
        out.append(&m_library.tracks.at(index));
    }
    return out;
}

QByteArray ProLinkServedMedium::artwork(quint32 artworkId) const {
    const QString stored = m_library.artwork.value(artworkId);
    if (stored.isEmpty()) {
        return QByteArray();
    }
    return readWhole(onMedium(m_rootPath, stored));
}

void ProLinkServedMedium::analysisFiles(
        quint32 trackId, QByteArray* pDat, QByteArray* pExt) const {
    const auto cached = m_analysisCache.constFind(trackId);
    if (cached != m_analysisCache.constEnd()) {
        if (pDat) {
            *pDat = cached->first;
        }
        if (pExt) {
            *pExt = cached->second;
        }
        return;
    }

    QPair<QByteArray, QByteArray> pair;
    const PdbTrack* pTrack = track(trackId);
    if (pTrack && !pTrack->analyzePath.isEmpty()) {
        pair.first = readWhole(onMedium(m_rootPath, pTrack->analyzePath));
        pair.second = readWhole(onMedium(m_rootPath, pTrack->analyzeExtPath()));
    }
    m_analysisCache.insert(trackId, pair);
    if (pDat) {
        *pDat = pair.first;
    }
    if (pExt) {
        *pExt = pair.second;
    }
}

QString ProLinkServedMedium::toString() const {
    return QStringLiteral("%1 %2 %3: %4 tracks, %5 playlists")
            .arg(m_slot == MediaSlot::Usb ? QStringLiteral("USB") : QStringLiteral("SD"),
                    QString::fromLatin1(exportPathForSlot(m_slot)),
                    m_volumeName.isEmpty() ? QStringLiteral("(unnamed)") : m_volumeName)
            .arg(trackCount())
            .arg(playlistCount());
}

} // namespace server
} // namespace prolink
} // namespace mixxx
