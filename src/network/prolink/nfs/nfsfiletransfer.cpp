#include "network/prolink/nfs/nfsfiletransfer.h"

#include "moc_nfsfiletransfer.cpp"
#include "util/logger.h"

namespace {
const mixxx::Logger kLogger("ProLinkFetch");
} // namespace

namespace mixxx {
namespace prolink {
namespace nfs {

NfsFileTransfer::NfsFileTransfer(NfsV2Client* pClient, QObject* parent)
        : QObject(parent),
          m_pClient(pClient) {
}

void NfsFileTransfer::fetch(const QByteArray& handle,
        quint32 expectedSize,
        Callback callback) {
    m_handle = handle;
    m_expectedSize = expectedSize;
    m_callback = std::move(callback);
    m_chunks.clear();
    m_nextOffset = 0;
    m_inFlight = 0;
    m_failed = false;
    m_done = false;
    m_error.clear();
    m_reads = 0;
    m_shortReads = 0;
    m_timer.start();

    if (expectedSize == 0) {
        // A zero-length file is legal and reading it would hang waiting for a
        // reply that never comes, since there is nothing to ask for.
        finish();
        return;
    }
    pump();
}

void NfsFileTransfer::pump() {
    while (!m_failed && m_inFlight < m_window && m_nextOffset < m_expectedSize) {
        const quint32 offset = m_nextOffset;
        const quint32 remaining = m_expectedSize - offset;
        const quint32 count = qMin(remaining, m_chunkSize);
        m_nextOffset += count;
        m_inFlight++;
        m_reads++;

        m_pClient->read(m_handle,
                offset,
                count,
                [this, offset, count](const NfsV2Client::Outcome<NfsV2Client::ReadResult>&
                                result) {
                    m_inFlight--;
                    if (!result.ok) {
                        // First error wins: later ones are usually the rest of
                        // the window failing for the same reason, and reporting
                        // the last would name a symptom rather than the cause.
                        if (!m_failed) {
                            m_failed = true;
                            m_error = result.error;
                        }
                        if (m_inFlight == 0) {
                            finish();
                        }
                        return;
                    }
                    const QByteArray& data = result.value.data;
                    m_chunks.insert(offset, data);
                    if (static_cast<quint32>(data.size()) < count &&
                            offset + static_cast<quint32>(data.size()) < m_expectedSize) {
                        m_shortReads++;
                    }
                    pump();
                });
    }

    if (m_inFlight == 0 && (m_failed || m_nextOffset >= m_expectedSize)) {
        finish();
    }
}

void NfsFileTransfer::finish() {
    if (m_done) {
        return;
    }
    m_done = true;

    Result result;
    result.elapsedMs = m_timer.elapsed();
    result.reads = m_reads;
    result.shortReads = m_shortReads;

    if (m_failed) {
        result.error = m_error;
        if (m_callback) {
            m_callback(result);
        }
        return;
    }

    // Reassemble in offset order. QMap iterates sorted by key, which is exactly
    // what out-of-order UDP replies need.
    result.data.reserve(static_cast<int>(m_expectedSize));
    for (auto it = m_chunks.constBegin(); it != m_chunks.constEnd(); ++it) {
        result.data.append(it.value());
    }

    if (static_cast<quint32>(result.data.size()) != m_expectedSize) {
        // Refuse a partial file rather than hand back something that looks
        // complete. A truncated export.pdb parses far enough to be plausible and
        // then produces a library missing its last few hundred tracks, which is
        // a much harder bug to notice than a failed fetch.
        result.error = QStringLiteral("expected %1 bytes, assembled %2")
                               .arg(m_expectedSize)
                               .arg(result.data.size());
        if (m_callback) {
            m_callback(result);
        }
        return;
    }

    result.ok = true;
    if (m_callback) {
        m_callback(result);
    }
}

} // namespace nfs
} // namespace prolink
} // namespace mixxx
