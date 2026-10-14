#include "library/deck/deckrecorder.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStorageInfo>
#include <algorithm>
#include <utility>

#include "control/controlproxy.h"
#include "library/deck/deckhistory.h"
#include "library/deck/historystore.h"
#include "library/deck/mediaregistry.h"
#include "library/deck/playhistory.h"
#include "moc_deckrecorder.cpp"
#include "util/logger.h"

namespace mixxx {
namespace deck {

namespace {

const mixxx::Logger kLogger("DeckRecorder");

const QString kStateFile = QStringLiteral("recording.json");

/// dj-usb mounts the deck's sticks, and is the one thing that writes to one
/// (pi_config/dj-usb/dj-usb, `write`).
const QString kDjUsb = QStringLiteral("/usr/local/bin/dj-usb");
/// The filesystems it writes: its WRITABLE_FS.
const QStringList kWritableFs = {
        QStringLiteral("vfat"),
        QStringLiteral("exfat"),
        QStringLiteral("ext2"),
        QStringLiteral("ext3"),
        QStringLiteral("ext4"),
};
/// `dj-usb write`'s exits.
constexpr int kWriteFull = 3;
constexpr int kWriteFilesystem = 4;
constexpr int kWriteFailed = 5;
constexpr int kWriteTaken = 6;

/// The input, as the kernel lists it: the card ALSA calls CODEC has a
/// capture side. Absent on a card with none (an emulated deck restored from a
/// snapshot older than the emulated input, a DAC that is not the UCA222).
const QString kInputPcm = QStringLiteral("/proc/asound/CODEC/pcm0c");

/// What a track list leaves room for on a stick, on top of the MP3, and the
/// FAT's own: a megabyte, as dj-usb counts it.
constexpr qint64 kStickSpareBytes = 1024 * 1024 + 64 * 1024;

} // namespace

QString DeckRecorder::defaultDirectory() {
    return QStringLiteral("/var/lib/rauc/trimixxx-recording");
}

DeckRecorder::DeckRecorder(DeckHistory* pHistory,
        MediaRegistry* pRegistry,
        const QString& directory,
        QObject* pParent)
        : QObject(pParent),
          m_pHistory(pHistory),
          m_pRegistry(pRegistry),
          m_directory(directory) {
    m_thread.setObjectName(QStringLiteral("DeckRecorder"));
    m_pCapture = new InputCapture();
    m_pCapture->moveToThread(&m_thread);
    connect(m_pCapture, &InputCapture::levels, this, &DeckRecorder::onLevels);
    connect(m_pCapture, &InputCapture::finished, this, &DeckRecorder::onCaptureFinished);
    m_thread.start();

    m_tick.setInterval(1000);
    connect(&m_tick, &QTimer::timeout, this, &DeckRecorder::tick);
    m_roomCheck.setInterval(5000);
    connect(&m_roomCheck, &QTimer::timeout, this, &DeckRecorder::checkRoom);
    m_writeProgress.setInterval(250);
    connect(&m_writeProgress, &QTimer::timeout, this, &DeckRecorder::tick);

    // The silence prompt is the skin's overlay (TriMixxx_skin/skin.xml): its
    // controls are the skin's attributes, made before any deck node, and so
    // before this.
    m_pSilencePrompt = std::make_unique<ControlProxy>(QStringLiteral("[TriMixxx]"),
            QStringLiteral("record_silence"),
            this,
            ControlFlag::NoAssertIfMissing);
    m_pSilenceKeep = std::make_unique<ControlProxy>(QStringLiteral("[TriMixxx]"),
            QStringLiteral("record_silence_keep"),
            this,
            ControlFlag::NoAssertIfMissing);
    m_pSilenceEnd = std::make_unique<ControlProxy>(QStringLiteral("[TriMixxx]"),
            QStringLiteral("record_silence_end"),
            this,
            ControlFlag::NoAssertIfMissing);
    m_pSilenceKeep->connectValueChanged(this, [this](double value) {
        if (value > 0.0) {
            m_silence.keep();
            updateSilencePrompt();
        }
    });
    m_pSilenceEnd->connectValueChanged(this, [this](double value) {
        if (value > 0.0 && m_state == State::Recording) {
            stop();
            emit endAsked();
        }
    });
    m_pSilencePrompt->set(0.0);

    load();
}

DeckRecorder::~DeckRecorder() {
    // A recording ends with Mixxx: the file closed whole, and offered again
    // if Mixxx comes back within this power-up.
    const bool recording = m_state == State::Recording;
    m_pCapture->requestStop();
    m_thread.quit();
    m_thread.wait();
    if (recording && m_pCapture->frames() > 0) {
        m_endMs = m_startMs + m_pCapture->frames() * 1000 / InputCapture::kRate;
        m_state = State::Recorded;
        save();
    }
    delete m_pCapture;
    if (m_pWriter) {
        // dj-usb runs as root and cannot be stopped from here, nor should it
        // be: it ends with the stick read-only again. Waited for, a while.
        kLogger.info() << "waiting for the write to" << m_writeMountPoint << "to end";
        m_pWriter->waitForFinished(60000);
    }
}

QString DeckRecorder::cannotStart() const {
    if (m_state != State::Idle) {
        return QString();
    }
    // The directory is made on the card, after it is mounted, and never by
    // Mixxx: none means no recording, rather than one in RAM (Sam: "This
    // needs to get written to the SD (not RAM)").
    const QFileInfo directory(m_directory);
    const QStorageInfo storage(m_directory);
    const QByteArray type = storage.fileSystemType();
    if (!directory.isDir() || !directory.isWritable() || !storage.isValid() || type == "tmpfs" ||
            type == "ramfs" || type == "overlay" || type == "squashfs") {
        return tr("no room on the card");
    }
#ifdef __linux__
    if (!QFileInfo::exists(kInputPcm)) {
        return tr("no input on the sound card");
    }
#endif
    if (storage.bytesAvailable() <
            setrecording::bytesFor(setrecording::kMaxLengthMs) + kStartSpareBytes) {
        return tr("not enough room on the card");
    }
    return QString();
}

void DeckRecorder::start() {
    if (m_state != State::Idle) {
        return;
    }
    const QString why = cannotStart();
    if (!why.isEmpty()) {
        emit notice(tr("Can't record: %1").arg(why));
        return;
    }
    // What an earlier recording left, written and done with.
    clearDirectory();
    m_digits = setrecording::digitsOf(QRandomGenerator::system()->generate());
    m_bootId = DeckHistory::bootId();
    m_startMs = DeckHistory::clockMs();
    m_endMs = -1;
    m_frames = 0;
    m_written.clear();
    m_peakLeft = 0.0f;
    m_peakRight = 0.0f;
    m_silence = setrecording::SilenceWatch();
    m_stopReason.clear();
    m_state = State::Recording;
    save();

    const QString path = audioPath();
    const QString title = tr("Recorded set %1").arg(m_digits);
    const qint64 maxFrames = setrecording::kMaxLengthMs * InputCapture::kRate / 1000;
    m_pCapture->arm();
    QMetaObject::invokeMethod(
            m_pCapture,
            [pCapture = m_pCapture, path, title, maxFrames]() {
                pCapture->run(path, title, maxFrames);
            },
            Qt::QueuedConnection);
    m_tick.start();
    m_roomCheck.start();
    kLogger.info() << "recording the input into" << path;
    emit stateChanged();
}

void DeckRecorder::stop() {
    if (m_state == State::Recording) {
        m_pCapture->requestStop();
    }
}

qint64 DeckRecorder::lengthMs() const {
    switch (m_state) {
    case State::Recording:
        return m_frames * 1000 / InputCapture::kRate;
    case State::Recorded:
        return std::max<qint64>(m_endMs - m_startMs, 0);
    case State::Idle:
        break;
    }
    return 0;
}

QPair<double, double> DeckRecorder::takePeaks() {
    const QPair<double, double> peaks(
            setrecording::dbfs(m_peakLeft), setrecording::dbfs(m_peakRight));
    m_peakLeft = 0.0f;
    m_peakRight = 0.0f;
    return peaks;
}

void DeckRecorder::onLevels(float left, float right, qint64 frames) {
    if (m_state != State::Recording) {
        return;
    }
    m_frames = frames;
    m_peakLeft = std::max(m_peakLeft, left);
    m_peakRight = std::max(m_peakRight, right);
    m_silence.hear(DeckHistory::clockMs(),
            std::max(setrecording::dbfs(left), setrecording::dbfs(right)));
    updateSilencePrompt();
    emit levelsChanged();
}

void DeckRecorder::onCaptureFinished(
        qint64 frames, InputCapture::End end, const QString& error) {
    m_tick.stop();
    m_roomCheck.stop();
    m_frames = frames;
    m_peakLeft = 0.0f;
    m_peakRight = 0.0f;
    const QString stopReason = std::exchange(m_stopReason, QString());
    if (frames == 0) {
        // Nothing came in: the input would not open, or the card would not
        // take the file. Nothing to keep.
        clearDirectory();
        m_digits.clear();
        m_state = State::Idle;
        updateSilencePrompt();
        emit notice(tr("Can't record: %1").arg(error.isEmpty() ? tr("nothing came in") : error));
        emit stateChanged();
        return;
    }
    m_endMs = m_startMs + frames * 1000 / InputCapture::kRate;
    m_state = State::Recorded;
    save();
    updateSilencePrompt();
    const QString length = setrecording::lengthText(lengthMs());
    if (end == InputCapture::End::Limit) {
        emit notice(tr("Recording stopped at 3 hours"));
    } else if (end == InputCapture::End::Failed) {
        emit notice(tr("Recording stopped at %1: %2").arg(length, error));
    } else if (!stopReason.isEmpty()) {
        emit notice(tr("Recording stopped at %1: %2").arg(length, stopReason));
    }
    kLogger.info() << "recorded" << length << "into" << audioPath();
    emit stateChanged();
}

void DeckRecorder::checkRoom() {
    if (m_state != State::Recording) {
        return;
    }
    const QStorageInfo storage(m_directory);
    if (storage.isValid() && storage.bytesAvailable() < kStopBelowBytes) {
        m_stopReason = tr("the card is full");
        stop();
    }
}

void DeckRecorder::updateSilencePrompt() {
    const bool asking = m_state == State::Recording && m_silence.asking(DeckHistory::clockMs());
    if ((m_pSilencePrompt->get() > 0.0) != asking) {
        m_pSilencePrompt->set(asking ? 1.0 : 0.0);
    }
}

QString DeckRecorder::audioPath() const {
    return QDir(m_directory).filePath(setrecording::audioFileName(m_digits));
}

QString DeckRecorder::trackListPath() const {
    return QDir(m_directory).filePath(setrecording::trackListFileName(m_digits));
}

QString DeckRecorder::stickId(const QString& mountPoint) {
    // dj-usb wrote it down at the mount: /run/dj-usb/DJ_USB_N.uuid.
    QFile uuid(QStringLiteral("/run/dj-usb/%1.uuid").arg(QFileInfo(mountPoint).fileName()));
    if (uuid.open(QIODevice::ReadOnly)) {
        const QString id = QString::fromUtf8(uuid.readAll()).trimmed();
        if (!id.isEmpty()) {
            return id;
        }
    }
    return mountPoint;
}

QString DeckRecorder::stickName(const QString& mountPoint, int slot) const {
    // As the home list calls it: what the medium calls itself.
    if (m_pRegistry) {
        for (const MediumInfo& medium : m_pRegistry->media()) {
            if (medium.id.isLocal() && medium.id.mountPoint() == mountPoint &&
                    !medium.name.isEmpty()) {
                return medium.name;
            }
        }
    }
    QFile label(QStringLiteral("/run/dj-usb/%1.label").arg(QFileInfo(mountPoint).fileName()));
    if (label.open(QIODevice::ReadOnly)) {
        const QString name = QString::fromUtf8(label.readAll()).trimmed();
        if (!name.isEmpty()) {
            return name;
        }
    }
    return tr("USB %1").arg(slot);
}

qint64 DeckRecorder::bytesNeeded() const {
    return QFileInfo(audioPath()).size() + kStickSpareBytes;
}

QList<DeckRecorder::Stick> DeckRecorder::sticks() const {
    // What dj-usb has mounted in the deck's ports, from the kernel's own
    // list: a stick the registry is still reading is a stick all the same.
    QList<Stick> sticks;
    QFile mounts(QStringLiteral("/proc/self/mounts"));
    if (!mounts.open(QIODevice::ReadOnly)) {
        return sticks;
    }
    static const QRegularExpression kSlot(QStringLiteral("^/media/DJ_USB_(\\d+)$"));
    const qint64 needed = bytesNeeded();
    for (const QByteArray& line : mounts.readAll().split('\n')) {
        const QList<QByteArray> fields = line.split(' ');
        if (fields.size() < 3) {
            continue;
        }
        const QString mountPoint = QString::fromUtf8(fields.at(1));
        const QRegularExpressionMatch match = kSlot.match(mountPoint);
        if (!match.hasMatch()) {
            continue;
        }
        Stick stick;
        stick.mountPoint = mountPoint;
        stick.slot = match.captured(1).toInt();
        stick.name = stickName(mountPoint, stick.slot);
        stick.freeBytes = QStorageInfo(mountPoint).bytesAvailable();
        stick.written = m_written.contains(stickId(mountPoint));
        if (!kWritableFs.contains(QString::fromUtf8(fields.at(2)))) {
            stick.refusal = tr("can't write to this stick");
        } else if (!stick.written && stick.freeBytes < needed) {
            stick.refusal = tr("full: needs %1").arg(setrecording::sizeText(needed));
        }
        sticks.append(stick);
    }
    std::sort(sticks.begin(), sticks.end(), [](const Stick& a, const Stick& b) {
        return a.slot < b.slot;
    });
    return sticks;
}

double DeckRecorder::writeProgress() const {
    if (!m_pWriter || m_writeTotalBytes <= 0) {
        return 0.0;
    }
    // What has reached the stick: each file under its own name once whole,
    // under dj-usb's hidden one until then.
    qint64 done = 0;
    const QDir stick(m_writeMountPoint);
    for (const QString& name : {setrecording::audioFileName(m_digits),
                 setrecording::trackListFileName(m_digits)}) {
        const QFileInfo whole(stick.filePath(name));
        const QFileInfo part(stick.filePath(QStringLiteral(".%1.part").arg(name)));
        done += whole.exists() ? whole.size() : (part.exists() ? part.size() : 0);
    }
    return std::clamp(static_cast<double>(done) / static_cast<double>(m_writeTotalBytes), 0.0, 1.0);
}

void DeckRecorder::writeTo(const QString& mountPoint) {
    if (m_state != State::Recorded || m_pWriter) {
        return;
    }
    const QList<Stick> all = sticks();
    const auto it = std::find_if(all.begin(), all.end(), [&mountPoint](const Stick& stick) {
        return stick.mountPoint == mountPoint;
    });
    if (it == all.end()) {
        emit notice(tr("That stick is not in the deck any more"));
        return;
    }
    if (it->written) {
        return;
    }
    if (!it->refusal.isEmpty()) {
        emit notice(tr("%1: %2").arg(it->name, it->refusal));
        return;
    }
    // The track list as the History has it now, names that came late and all.
    QSaveFile list(trackListPath());
    if (!list.open(QIODevice::WriteOnly) || list.write(trackListText().toUtf8()) < 0 ||
            !list.commit()) {
        emit notice(tr("The set was not written: the card would not take its track list"));
        return;
    }
    m_writeMountPoint = mountPoint;
    m_writeName = it->name;
    m_writeTotalBytes = QFileInfo(audioPath()).size() + QFileInfo(trackListPath()).size();
    m_pWriter = new QProcess(this);
    connect(m_pWriter,
            &QProcess::finished,
            this,
            [this](int exitCode, QProcess::ExitStatus status) {
                onWriteFinished(exitCode, status == QProcess::CrashExit);
            });
    connect(m_pWriter, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        // The only error that is not followed by finished().
        if (error == QProcess::FailedToStart) {
            onWriteFinished(-1, true);
        }
    });
    // sudo -n: a password asked for would wait for ever.
    m_pWriter->start(QStringLiteral("sudo"),
            {QStringLiteral("-n"),
                    kDjUsb,
                    QStringLiteral("write"),
                    mountPoint,
                    audioPath(),
                    trackListPath()});
    m_writeProgress.start();
    kLogger.info() << "writing" << audioPath() << "to" << mountPoint;
    emit stateChanged();
}

void DeckRecorder::onWriteFinished(int exitCode, bool crashed) {
    if (!m_pWriter) {
        return;
    }
    const QString said = QString::fromUtf8(m_pWriter->readAllStandardError()).trimmed();
    m_pWriter->deleteLater();
    m_pWriter = nullptr;
    m_writeProgress.stop();
    const QString name = std::exchange(m_writeName, QString());
    const QString mountPoint = std::exchange(m_writeMountPoint, QString());
    kLogger.info() << "dj-usb write to" << mountPoint << "ended:" << (crashed ? -1 : exitCode)
                   << said;
    if (!crashed && exitCode == 0) {
        m_written.insert(stickId(mountPoint));
        save();
        emit notice(tr("Set written to %1").arg(name));
        emit stateChanged();
        return;
    }
    switch (crashed ? -1 : exitCode) {
    case kWriteFull:
        emit notice(tr("The set was not written: %1 is full").arg(name));
        break;
    case kWriteFilesystem:
        emit notice(tr("The set was not written: the deck can't write to %1").arg(name));
        break;
    case kWriteTaken:
        emit notice(tr("The set was not written: %1 has a set of that name").arg(name));
        break;
    case kWriteFailed:
        emit notice(tr("The set was not written to %1: was it pulled out?").arg(name));
        break;
    default:
        emit notice(tr("The set was not written to %1").arg(name));
        break;
    }
    emit stateChanged();
}

void DeckRecorder::discard() {
    if (m_state != State::Recorded || m_pWriter) {
        return;
    }
    clearDirectory();
    m_digits.clear();
    m_written.clear();
    m_state = State::Idle;
    kLogger.info() << "the recording was discarded";
    emit notice(tr("Recording discarded"));
    emit stateChanged();
}

void DeckRecorder::finish() {
    if (m_state != State::Recorded || m_pWriter || m_written.isEmpty()) {
        return;
    }
    m_state = State::Idle;
    // Said on the card too, so a Mixxx started again does not offer it.
    save();
    emit stateChanged();
}

QString DeckRecorder::trackListText() const {
    const qint64 endMs = m_state == State::Recording ? DeckHistory::clockMs() : m_endMs;
    QList<setrecording::TrackListLine> lines;
    if (m_pHistory && !m_pHistory->sessions().isEmpty()) {
        // This power-up's: the recording is, whatever ended it.
        const HistorySession& session = m_pHistory->sessions().first();
        for (const history::ListedTrack& listed :
                history::tracksBetween(m_pHistory->playsNow(), m_startMs, endMs)) {
            const HistoryTrack* pTrack = session.track(listed.track);
            setrecording::TrackListLine line;
            line.atMs = listed.atMs;
            if (pTrack) {
                line.artist = pTrack->artist;
                line.title = pTrack->title;
            }
            lines.append(line);
        }
    }
    return setrecording::trackListText(m_digits, lengthMs(), lines);
}

void DeckRecorder::save() const {
    QJsonArray written;
    for (const QString& id : m_written) {
        written.append(id);
    }
    const QJsonObject state{
            {QStringLiteral("digits"), m_digits},
            {QStringLiteral("boot"), QString::fromLatin1(m_bootId)},
            {QStringLiteral("startMs"), m_startMs},
            {QStringLiteral("endMs"), m_endMs},
            {QStringLiteral("written"), written},
            {QStringLiteral("done"), m_state == State::Idle},
    };
    QSaveFile file(QDir(m_directory).filePath(kStateFile));
    if (!file.open(QIODevice::WriteOnly) ||
            file.write(QJsonDocument(state).toJson(QJsonDocument::Compact)) < 0 ||
            !file.commit()) {
        kLogger.warning() << "could not keep the recording's state in" << file.fileName();
    }
}

void DeckRecorder::load() {
    QFile file(QDir(m_directory).filePath(kStateFile));
    if (!file.open(QIODevice::ReadOnly)) {
        return;
    }
    const QJsonObject state = QJsonDocument::fromJson(file.readAll()).object();
    // Another power-up's is not offered: the boot empties the directory, and
    // a Mixxx that started before it did must not offer last night's set.
    // Nor one already written: whatever the DJ was doing with it went with
    // the Mixxx that quit, and "Record set" is what they come back to.
    if (state.value(QStringLiteral("boot")).toString().toLatin1() != DeckHistory::bootId() ||
            state.value(QStringLiteral("done")).toBool() ||
            !state.value(QStringLiteral("written")).toArray().isEmpty()) {
        return;
    }
    m_digits = state.value(QStringLiteral("digits")).toString();
    if (m_digits.isEmpty() || !QFileInfo::exists(audioPath())) {
        m_digits.clear();
        return;
    }
    m_bootId = DeckHistory::bootId();
    m_startMs = state.value(QStringLiteral("startMs")).toInteger();
    m_endMs = state.value(QStringLiteral("endMs")).toInteger(-1);
    if (m_endMs < m_startMs) {
        // Mixxx went while it recorded: as long as what reached the card.
        m_endMs = m_startMs +
                QFileInfo(audioPath()).size() * 8 / setrecording::kBitrateKbps;
    }
    m_state = State::Recorded;
    kLogger.info() << "this power-up's recording is still on the card:" << audioPath();
}

void DeckRecorder::clearDirectory() {
    QDir directory(m_directory);
    const QStringList names = directory.entryList(
            {QStringLiteral("recorded-set-*"), kStateFile}, QDir::Files | QDir::Hidden);
    for (const QString& name : names) {
        directory.remove(name);
    }
}

} // namespace deck
} // namespace mixxx
