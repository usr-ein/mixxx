#include "network/prolink/rpc/xdrbuffer.h"

#include <QtEndian>

namespace {
/// XDR pads every item up to a multiple of four bytes.
int paddedLength(int length) {
    return (length + 3) & ~3;
}
} // namespace

namespace mixxx {
namespace prolink {

// -- writer ------------------------------------------------------------------

XdrWriter& XdrWriter::u32(quint32 value) {
    const quint32 big = qToBigEndian(value);
    m_data.append(reinterpret_cast<const char*>(&big), sizeof(big));
    return *this;
}

XdrWriter& XdrWriter::pad4() {
    const int padding = paddedLength(m_data.size()) - m_data.size();
    if (padding > 0) {
        m_data.append(padding, '\0');
    }
    return *this;
}

XdrWriter& XdrWriter::opaqueFixed(const QByteArray& value) {
    m_data.append(value);
    return pad4();
}

XdrWriter& XdrWriter::opaqueVar(const QByteArray& value) {
    u32(static_cast<quint32>(value.size()));
    m_data.append(value);
    return pad4();
}

XdrWriter& XdrWriter::stringAscii(const QString& text) {
    return opaqueVar(text.toLatin1());
}

XdrWriter& XdrWriter::stringUtf16Le(const QString& text) {
    // Byte-exact, deliberately without QTextCodec or QStringEncoder: this
    // encoding must not acquire a BOM, must not be affected by any locale, and
    // must produce exactly 2 bytes per UTF-16 code unit. Surrogate pairs come
    // out as their two units, which is what the wire wants.
    QByteArray encoded;
    encoded.reserve(text.size() * 2);
    for (const QChar character : text) {
        const ushort unit = character.unicode();
        encoded.append(static_cast<char>(unit & 0xff));
        encoded.append(static_cast<char>((unit >> 8) & 0xff));
    }
    return opaqueVar(encoded);
}

// -- reader ------------------------------------------------------------------

bool XdrReader::fail() {
    m_valid = false;
    return false;
}

bool XdrReader::canRead(int length) const {
    if (!m_valid || length < 0) {
        return false;
    }
    // Check the *padded* length: a field whose data fits but whose padding runs
    // off the end is a malformed datagram, and accepting it would leave the
    // cursor past the end for the next read to trip over.
    const int needed = paddedLength(length);
    return needed <= m_data.size() - m_pos;
}

quint32 XdrReader::u32() {
    if (!canRead(4)) {
        fail();
        return 0;
    }
    quint32 value = 0;
    memcpy(&value, m_data.constData() + m_pos, sizeof(value));
    m_pos += 4;
    return qFromBigEndian(value);
}

QByteArray XdrReader::opaqueFixed(int length) {
    if (!canRead(length)) {
        fail();
        return QByteArray();
    }
    const QByteArray value = m_data.mid(m_pos, length);
    m_pos += paddedLength(length);
    return value;
}

QByteArray XdrReader::opaqueVar(int maxLength) {
    const quint32 length = u32();
    if (!m_valid) {
        return QByteArray();
    }
    // Bound the prefix *before* using it for anything. Both tests matter and
    // for different reasons: the maxLength cap rejects a plausible-but-absurd
    // value, and canRead() rejects one that simply is not there. A prefix of
    // 0xFFFFFFFF must fail here without a single byte being allocated.
    if (length > static_cast<quint32>(maxLength)) {
        fail();
        return QByteArray();
    }
    return opaqueFixed(static_cast<int>(length));
}

QString XdrReader::stringAscii(int maxLength) {
    return QString::fromLatin1(opaqueVar(maxLength));
}

QString XdrReader::stringUtf16Le(int maxLength) {
    const QByteArray raw = opaqueVar(maxLength);
    if (!m_valid) {
        return QString();
    }
    // An odd byte count cannot be UTF-16. Truncating silently would hand back a
    // string that looks fine and is wrong by one character, so refuse instead.
    if (raw.size() % 2 != 0) {
        fail();
        return QString();
    }
    QString text;
    text.reserve(raw.size() / 2);
    for (int i = 0; i + 1 < raw.size(); i += 2) {
        const ushort unit = static_cast<ushort>(static_cast<quint8>(raw.at(i))) |
                (static_cast<ushort>(static_cast<quint8>(raw.at(i + 1))) << 8);
        text.append(QChar(unit));
    }
    return text;
}

void XdrReader::skip(int count) {
    if (!canRead(count)) {
        fail();
        return;
    }
    m_pos += paddedLength(count);
}

} // namespace prolink
} // namespace mixxx
