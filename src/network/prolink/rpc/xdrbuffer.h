#pragma once

#include <QByteArray>
#include <QString>

namespace mixxx {
namespace prolink {

/// XDR (RFC 4506) encoding, plus Pioneer's one deviation from it.
///
/// Standard XDR: everything big-endian, everything padded up to a 4-byte
/// boundary, variable-length data prefixed with a 32-bit byte count.
///
/// **The deviation.** Standard NFS and MOUNT encode path and file names as
/// ASCII. Pioneer encodes the mount path and *every* `LOOKUP` filename as
/// **UTF-16LE**, still length-prefixed — and the prefix counts **bytes**, not
/// characters, so an n-character ASCII path produces a prefix of 2n.
///
/// Two reasons that detail is worth this much comment. It is the single most
/// important non-standard fact about the file-access path, and getting it wrong
/// produces `NFSERR_NOENT` for files that are plainly there. And it is why we
/// cannot simply link libnfs: its wire encoder emits ASCII, so using it would
/// mean patching the library rather than writing these two hundred lines.
///
/// Note it is *not* the same convention as the dbserver layer, which uses
/// UTF-16 **big**-endian counted in **characters including a trailing NUL**.
/// Two encodings, two endiannesses, two units, in one protocol. Do not let a
/// helper be shared between them.
class XdrWriter {
  public:
    XdrWriter() = default;

    const QByteArray& data() const {
        return m_data;
    }
    int size() const {
        return m_data.size();
    }

    XdrWriter& u32(quint32 value);
    XdrWriter& i32(qint32 value) {
        return u32(static_cast<quint32>(value));
    }
    XdrWriter& boolean(bool value) {
        return u32(value ? 1 : 0);
    }

    /// Fixed-length opaque: raw bytes, padded to 4, no length prefix.
    ///
    /// This is how the 32-byte NFS filehandle travels. A handle is an
    /// *uninterpreted token* — echo back exactly what the server gave us, never
    /// parse or normalise it. (A CDJ does not return the favour: it keeps only
    /// our leading 12 bytes and overwrites the rest, which is why our own server
    /// keys its handle table on those 12. Irrelevant as a client, but it is the
    /// same field.)
    XdrWriter& opaqueFixed(const QByteArray& value);

    /// Variable-length opaque: u32 byte count, bytes, padding.
    XdrWriter& opaqueVar(const QByteArray& value);

    XdrWriter& stringAscii(const QString& text);

    /// Pioneer's UTF-16LE length-prefixed string. See the class comment.
    XdrWriter& stringUtf16Le(const QString& text);

  private:
    XdrWriter& pad4();

    QByteArray m_data;
};

/// The reading counterpart.
///
/// **Every length prefix is validated against the bytes actually remaining
/// before anything is allocated.** A corrupt or hostile datagram claiming a
/// 4 GiB payload must cost nothing but a `false` return — this is a network
/// input path, and the whole class is written so that no reachable input can
/// make it allocate more than the datagram's own size. Any change here should
/// keep that property, and the unit tests pin it.
///
/// Failure is sticky: once a read fails, `isValid()` stays false and every
/// subsequent read is a no-op returning a default. That means a caller can
/// decode a whole structure and check once at the end, rather than testing
/// every field — which is what stops half-checked decoders from existing.
class XdrReader {
  public:
    explicit XdrReader(const QByteArray& data)
            : m_data(data) {
    }

    bool isValid() const {
        return m_valid;
    }
    int remaining() const {
        return m_data.size() - m_pos;
    }
    bool atEnd() const {
        return remaining() <= 0;
    }
    int position() const {
        return m_pos;
    }

    quint32 u32();
    qint32 i32() {
        return static_cast<qint32>(u32());
    }
    bool boolean() {
        return u32() != 0;
    }

    QByteArray opaqueFixed(int length);
    /// *maxLength* is a sanity cap; a prefix above it fails rather than
    /// allocating. Defaults to NFSv2's own ceiling on a single READ payload.
    QByteArray opaqueVar(int maxLength = 8192);

    QString stringAscii(int maxLength = 1024);
    QString stringUtf16Le(int maxLength = 1024);

    /// Skip *count* bytes plus XDR padding. For fields we must step over
    /// without interpreting.
    void skip(int count);

  private:
    /// Mark the read failed and return false, so callers can `return fail();`.
    bool fail();
    /// Can *length* bytes be read, and does the padded length fit too?
    bool canRead(int length) const;

    const QByteArray m_data;
    int m_pos = 0;
    bool m_valid = true;
};

} // namespace prolink
} // namespace mixxx
