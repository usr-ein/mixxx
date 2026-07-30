#include "network/prolink/dbserver/dbservermessage.h"

#include <kaitai/exceptions.h>
#include <kaitai/kaitaistream.h>

#include <QStringList>
#include <exception>
#include <sstream>

#include "prolink_dbserver.h"

namespace mixxx {
namespace prolink {
namespace dbserver {

namespace {

/// The fixed size of the argument-type blob. The matching cap on the argument
/// count is enforced by the schema, on the read side, where it also protects the
/// generated code's indexing into this blob.
constexpr int kArgTagBlobSize = 12;

ArgType argTagFor(FieldType type) {
    switch (type) {
    case FieldType::UInt8:
        return ArgType::UInt8;
    case FieldType::UInt16:
        return ArgType::UInt16;
    case FieldType::UInt32:
        return ArgType::UInt32;
    case FieldType::Binary:
        return ArgType::Binary;
    case FieldType::String:
        return ArgType::String;
    }
    return ArgType::UInt32;
}

void appendU16(QByteArray* pOut, quint16 value) {
    pOut->append(static_cast<char>((value >> 8) & 0xFF));
    pOut->append(static_cast<char>(value & 0xFF));
}

void appendU32(QByteArray* pOut, quint32 value) {
    pOut->append(static_cast<char>((value >> 24) & 0xFF));
    pOut->append(static_cast<char>((value >> 16) & 0xFF));
    pOut->append(static_cast<char>((value >> 8) & 0xFF));
    pOut->append(static_cast<char>(value & 0xFF));
}

/// UTF-16 **big-endian**, counted in characters and including the terminating
/// NUL, so a three-character string announces 4 and carries 8 bytes.
///
/// Note the endianness: the NFS side of this same protocol uses UTF-16
/// *little*-endian counted in *bytes*. Getting the two the wrong way round
/// produces strings that look like CJK noise rather than an obvious failure,
/// which is why neither is left to a generic helper.
void appendUtf16Be(QByteArray* pOut, const QString& text) {
    appendU32(pOut, static_cast<quint32>(text.size() + 1));
    for (const QChar character : text) {
        appendU16(pOut, character.unicode());
    }
    appendU16(pOut, 0);
}

/// The read direction of appendUtf16Be.
///
/// Hand-written rather than left to the schema: Mixxx compiles the Kaitai
/// runtime with KS_STR_ENCODING_NONE, under which `bytes_to_str` returns its
/// input unchanged. An `encoding: UTF-16BE` in the .ksy would therefore be
/// silently ignored and produce UTF-16 bytes in a std::string that claimed to be
/// decoded, so the schema deliberately hands back raw bytes.
QString decodeUtf16Be(const QByteArray& bytes) {
    QString text;
    text.reserve(bytes.size() / 2);
    for (int i = 0; i + 1 < bytes.size(); i += 2) {
        const char16_t unit = static_cast<char16_t>(
                (static_cast<quint8>(bytes.at(i)) << 8) |
                static_cast<quint8>(bytes.at(i + 1)));
        if (unit == 0) {
            break; // the counted trailing NUL
        }
        text.append(QChar(unit));
    }
    return text;
}

void appendField(QByteArray* pOut, const Field& field) {
    pOut->append(static_cast<char>(field.type));
    switch (field.type) {
    case FieldType::UInt8:
        pOut->append(static_cast<char>(field.number & 0xFF));
        return;
    case FieldType::UInt16:
        appendU16(pOut, static_cast<quint16>(field.number));
        return;
    case FieldType::UInt32:
        appendU32(pOut, field.number);
        return;
    case FieldType::Binary:
        appendU32(pOut, static_cast<quint32>(field.blob.size()));
        pOut->append(field.blob);
        return;
    case FieldType::String:
        appendUtf16Be(pOut, field.text);
        return;
    }
}

} // namespace

Field Field::u8(quint8 value) {
    Field field;
    field.type = FieldType::UInt8;
    field.number = value;
    return field;
}

Field Field::u16(quint16 value) {
    Field field;
    field.type = FieldType::UInt16;
    field.number = value;
    return field;
}

Field Field::u32(quint32 value) {
    Field field;
    field.type = FieldType::UInt32;
    field.number = value;
    return field;
}

Field Field::binary(const QByteArray& value) {
    Field field;
    field.type = FieldType::Binary;
    field.blob = value;
    return field;
}

Field Field::string(const QString& value) {
    Field field;
    field.type = FieldType::String;
    field.text = value;
    return field;
}

quint32 Message::number(int index, quint32 fallback) const {
    if (index < 0 || index >= m_args.size()) {
        return fallback;
    }
    const Field& field = m_args.at(index);
    switch (field.type) {
    case FieldType::UInt8:
    case FieldType::UInt16:
    case FieldType::UInt32:
        return field.number;
    default:
        return fallback;
    }
}

QByteArray Message::blob(int index) const {
    if (index < 0 || index >= m_args.size()) {
        return QByteArray();
    }
    const Field& field = m_args.at(index);
    return field.type == FieldType::Binary ? field.blob : QByteArray();
}

QByteArray Message::encode() const {
    QByteArray out;
    appendField(&out, Field::u32(kMagic));
    appendField(&out, Field::u32(m_transactionId));
    appendField(&out, Field::u16(m_type));
    appendField(&out, Field::u8(static_cast<quint8>(m_args.size())));

    QByteArray tags(kArgTagBlobSize, '\0');
    for (int i = 0; i < m_args.size() && i < kArgTagBlobSize; ++i) {
        tags[i] = static_cast<char>(argTagFor(m_args.at(i).type));
    }
    appendField(&out, Field::binary(tags));

    for (const Field& field : m_args) {
        // A zero-length binary argument is omitted from the wire entirely; the
        // preceding UInt32 length argument is what tells the reader it is
        // absent. Sending an empty one anyway is not a harmless variation --
        // every following argument then decodes one position out.
        if (field.type == FieldType::Binary && field.blob.isEmpty()) {
            continue;
        }
        appendField(&out, field);
    }
    return out;
}

Message::DecodeStatus Message::decode(
        const QByteArray& buffer, int* pOffset, Message* pMessage) {
    if (*pOffset >= buffer.size()) {
        return DecodeStatus::Incomplete;
    }

    // Parsed by the generated Kaitai reader, from `prolinks-compat/ksy/
    // prolink_dbserver.ksy`. The schema owns the two tag numberings, the
    // omitted-empty-blob rule and the length sanity limits; this function only
    // lifts the result into Qt types.
    //
    // Kaitai parses from a fully resident buffer, which is exactly right here:
    // a whole message is in memory before it is offered, and the schema's job is
    // to say whether it is a whole one.
    std::string window(buffer.constData() + *pOffset,
            static_cast<size_t>(buffer.size() - *pOffset));
    std::istringstream stream(window);
    kaitai::kstream ks(&stream);

    Message message;
    int consumed = 0;
    try {
        prolink_dbserver_t parsed(&ks);
        message.m_transactionId = parsed.transaction_id();
        message.m_type = parsed.message_type();
        for (const auto& pArgument : *parsed.args()) {
            if (!pArgument->is_present()) {
                // The omitted empty blob. Materialised so callers index by
                // argument position rather than by wire position, which is the
                // whole reason the rule is dangerous in the first place.
                message.m_args.append(Field::binary(QByteArray()));
                continue;
            }
            const auto* pField = pArgument->field();
            switch (static_cast<FieldType>(pField->field_type())) {
            case FieldType::UInt8:
                message.m_args.append(Field::u8(pField->value_u8()));
                break;
            case FieldType::UInt16:
                message.m_args.append(Field::u16(pField->value_u16()));
                break;
            case FieldType::UInt32:
                message.m_args.append(Field::u32(pField->value_u32()));
                break;
            case FieldType::Binary:
                message.m_args.append(Field::binary(
                        QByteArray(pField->blob().data(),
                                static_cast<int>(pField->blob().size()))));
                break;
            case FieldType::String:
                message.m_args.append(Field::string(decodeUtf16Be(
                        QByteArray(pField->text_raw().data(),
                                static_cast<int>(pField->text_raw().size())))));
                break;
            }
        }
        consumed = static_cast<int>(ks.pos());
    } catch (const kaitai::kstruct_error&) {
        // A `valid:` constraint failed: a wrong tag byte, a magic that is not
        // ours, an implausible length. The stream is desynchronised, and since
        // messages are framed by nothing but their own contents there is no
        // resynchronisation point to skip to.
        return DecodeStatus::Malformed;
    } catch (const std::exception&) {
        // Ran off the end. The expected outcome of parsing a TCP buffer too
        // early, and not an error: wait for more bytes and try again.
        return DecodeStatus::Incomplete;
    }

    *pMessage = message;
    *pOffset += consumed;
    return DecodeStatus::Ok;
}

QString Message::toString() const {
    QStringList rendered;
    for (const Field& field : m_args) {
        switch (field.type) {
        case FieldType::Binary:
            rendered.append(QStringLiteral("<%1B>").arg(field.blob.size()));
            break;
        case FieldType::String:
            rendered.append(QStringLiteral("\"%1\"").arg(field.text));
            break;
        default:
            rendered.append(QStringLiteral("0x%1").arg(field.number, 0, 16));
            break;
        }
    }
    return QStringLiteral("0x%1(tx=0x%2) [%3]")
            .arg(m_type, 4, 16, QChar('0'))
            .arg(m_transactionId, 0, 16)
            .arg(rendered.join(QStringLiteral(", ")));
}

QByteArray preamble() {
    QByteArray out;
    appendField(&out, Field::u32(1));
    return out;
}

QByteArray portQuery() {
    const QByteArray name = QByteArray(kPortQueryName);
    QByteArray out;
    appendU32(&out, static_cast<quint32>(name.size() + 1));
    out.append(name);
    out.append('\0');
    return out;
}

Message makeIntroduce(int requesterNumber) {
    return Message(kSetupTransactionId,
            MessageType::Introduce,
            {Field::u32(static_cast<quint32>(requesterNumber))});
}

Message makeDisconnect() {
    return Message(kSetupTransactionId, MessageType::Disconnect, {});
}

Message makeGetArtwork(quint32 transactionId, quint32 descriptor, quint32 artworkId) {
    return Message(transactionId,
            MessageType::GetArtwork,
            {Field::u32(descriptor), Field::u32(artworkId)});
}

} // namespace dbserver
} // namespace prolink
} // namespace mixxx
