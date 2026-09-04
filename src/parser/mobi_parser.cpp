#include "loreforge/parser/mobi_parser.h"

#include "loreforge/core/content_hash.h"
#include "loreforge/core/identifier.h"
#include "loreforge/document/document_validation.h"
#include "loreforge/text/chapter_heading_detector.h"
#include "loreforge/text/paragraph_detector.h"
#include "loreforge/text/text_normalizer.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QStringDecoder>
#include <QtEndian>

#include <algorithm>
#include <limits>
#include <optional>
#include <utility>

namespace loreforge::parser {
namespace {

constexpr qsizetype kPalmDatabaseHeaderSize = 78;
constexpr qsizetype kRecordEntrySize = 8;
constexpr qsizetype kMaximumSourceBytes = 512 * 1024 * 1024;
constexpr qsizetype kMaximumDecodedBytes = 512 * 1024 * 1024;
constexpr quint16 kNoCompression = 1;
constexpr quint16 kPalmDocCompression = 2;
constexpr quint16 kHuffCdicCompression = 17'480;
constexpr quint32 kCp1252 = 1'252;
constexpr quint32 kUtf8 = 65'001;
constexpr quint32 kUtf16 = 65'002;

struct RecordRange final {
    qsizetype start = 0;
    qsizetype end = 0;
};

struct MobiHeaders final {
    QList<RecordRange> records;
    quint16 compression = 0;
    quint32 textLength = 0;
    quint16 textRecordCount = 0;
    quint16 textRecordSize = 0;
    quint32 encoding = 0;
    quint32 version = 0;
    quint16 extraFlags = 0;
    QString title;
    QStringList authors;
    QString language;
};

struct ExtractedBlock final {
    document::BlockType type = document::BlockType::Unknown;
    QString text;
    qint64 startByte = 0;
    qint64 endByte = 0;
};

MobiParseError error(MobiParseErrorCode code, QString message) {
    return {code, std::move(message)};
}

bool canRead(QByteArrayView bytes, qsizetype offset, qsizetype size) {
    return offset >= 0 && size >= 0 && offset <= bytes.size() && size <= bytes.size() - offset;
}

std::optional<quint16> read16(QByteArrayView bytes, qsizetype offset) {
    if (!canRead(bytes, offset, 2)) {
        return std::nullopt;
    }
    return qFromBigEndian<quint16>(reinterpret_cast<const uchar*>(bytes.data() + offset));
}

std::optional<quint32> read32(QByteArrayView bytes, qsizetype offset) {
    if (!canRead(bytes, offset, 4)) {
        return std::nullopt;
    }
    return qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(bytes.data() + offset));
}

QString decodeCp1252(QByteArrayView bytes) {
    static constexpr char32_t replacements[32] = {
        0x20AC, 0xFFFD, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
        0x2039, 0x0152, 0xFFFD, 0x017D, 0xFFFD, 0xFFFD, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
        0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0xFFFD, 0x017E, 0x0178,
    };
    QString result;
    result.reserve(bytes.size());
    for (const auto byte : bytes) {
        const auto value = static_cast<uchar>(byte);
        if (value >= 0x80 && value < 0xA0) {
            const auto codePoint = replacements[value - 0x80];
            result += QString::fromUcs4(&codePoint, 1);
        } else {
            result += QChar::fromLatin1(static_cast<char>(value));
        }
    }
    return result;
}

std::optional<QString> decodeString(QByteArrayView bytes, quint32 encoding) {
    if (encoding == kCp1252) {
        return decodeCp1252(bytes);
    }
    if (encoding != kUtf8) {
        return std::nullopt;
    }
    QStringDecoder decoder(QStringDecoder::Utf8, QStringConverter::Flag::Stateless);
    const auto value = decoder.decode(bytes);
    return decoder.hasError() ? std::nullopt : std::optional<QString>(value);
}

std::variant<MobiHeaders, MobiParseError> readHeaders(QByteArrayView source) {
    if (source.size() > kMaximumSourceBytes) {
        return error(MobiParseErrorCode::ResourceLimitExceeded,
                     QStringLiteral("The MOBI file exceeds the 512 MiB import limit."));
    }
    if (!canRead(source, 0, kPalmDatabaseHeaderSize) ||
        source.sliced(60, 8) != QByteArrayView("BOOKMOBI", 8)) {
        return error(MobiParseErrorCode::InvalidContainer,
                     QStringLiteral("The source is not a BOOKMOBI Palm database."));
    }

    const auto recordCountValue = read16(source, 76);
    if (!recordCountValue.has_value() || *recordCountValue < 2) {
        return error(MobiParseErrorCode::InvalidContainer,
                     QStringLiteral("The MOBI record table is empty or truncated."));
    }
    const auto recordCount = static_cast<qsizetype>(*recordCountValue);
    if (recordCount >
        (std::numeric_limits<qsizetype>::max() - kPalmDatabaseHeaderSize) / kRecordEntrySize) {
        return error(MobiParseErrorCode::ResourceLimitExceeded,
                     QStringLiteral("The MOBI record table is too large."));
    }
    const auto tableEnd = kPalmDatabaseHeaderSize + recordCount * kRecordEntrySize;
    if (!canRead(source, kPalmDatabaseHeaderSize, recordCount * kRecordEntrySize)) {
        return error(MobiParseErrorCode::InvalidContainer,
                     QStringLiteral("The MOBI record table is truncated."));
    }

    MobiHeaders headers;
    headers.records.reserve(recordCount);
    QList<qsizetype> offsets;
    offsets.reserve(recordCount);
    for (qsizetype index = 0; index < recordCount; ++index) {
        const auto offset = read32(source, kPalmDatabaseHeaderSize + index * kRecordEntrySize);
        if (!offset.has_value() || *offset > static_cast<quint64>(source.size())) {
            return error(MobiParseErrorCode::InvalidContainer,
                         QStringLiteral("A MOBI record offset lies outside the file."));
        }
        const auto signedOffset = static_cast<qsizetype>(*offset);
        if (signedOffset < tableEnd || (!offsets.isEmpty() && signedOffset <= offsets.last())) {
            return error(MobiParseErrorCode::InvalidContainer,
                         QStringLiteral("MOBI record offsets are not strictly increasing."));
        }
        offsets.append(signedOffset);
    }
    for (qsizetype index = 0; index < offsets.size(); ++index) {
        headers.records.append({offsets.at(index), index + 1 < offsets.size()
                                                       ? offsets.at(index + 1)
                                                       : source.size()});
    }

    const auto record0Range = headers.records.first();
    const auto record0 = source.sliced(record0Range.start, record0Range.end - record0Range.start);
    const auto compression = read16(record0, 0);
    const auto textLength = read32(record0, 4);
    const auto textRecordCount = read16(record0, 8);
    const auto textRecordSize = read16(record0, 10);
    const auto encryption = read16(record0, 12);
    if (!compression.has_value() || !textLength.has_value() || !textRecordCount.has_value() ||
        !textRecordSize.has_value() || !encryption.has_value() || !canRead(record0, 16, 24) ||
        record0.sliced(16, 4) != QByteArrayView("MOBI", 4)) {
        return error(MobiParseErrorCode::InvalidHeader,
                     QStringLiteral("Record 0 has no complete PalmDOC and MOBI headers."));
    }
    if (*encryption != 0) {
        return error(MobiParseErrorCode::EncryptedDocument,
                     QStringLiteral("Encrypted MOBI books are not imported."));
    }
    if (*compression == kHuffCdicCompression) {
        return error(MobiParseErrorCode::UnsupportedCompression,
                     QStringLiteral("HUFF/CDIC-compressed MOBI books are not supported."));
    }
    if (*compression != kNoCompression && *compression != kPalmDocCompression) {
        return error(MobiParseErrorCode::UnsupportedCompression,
                     QStringLiteral("The MOBI book uses an unknown text compression method."));
    }
    if (*textRecordCount == 0 || *textRecordCount >= headers.records.size()) {
        return error(MobiParseErrorCode::InvalidHeader,
                     QStringLiteral("The MOBI text record count is invalid."));
    }

    const auto headerLength = read32(record0, 20);
    const auto encoding = read32(record0, 28);
    const auto version = read32(record0, 36);
    if (!headerLength.has_value() || *headerLength < 24 ||
        *headerLength > static_cast<quint64>(record0.size() - 16) || !encoding.has_value() ||
        !version.has_value()) {
        return error(MobiParseErrorCode::InvalidHeader,
                     QStringLiteral("The MOBI header length or required fields are invalid."));
    }
    if (*version >= 8) {
        return error(MobiParseErrorCode::UnsupportedFormat,
                     QStringLiteral("KF8-only books are not supported by the MOBI7 importer."));
    }
    if (*encoding == kUtf16) {
        return error(MobiParseErrorCode::UnsupportedEncoding,
                     QStringLiteral("UTF-16 MOBI text is not supported."));
    }
    if (*encoding != kCp1252 && *encoding != kUtf8) {
        return error(MobiParseErrorCode::UnsupportedEncoding,
                     QStringLiteral("The MOBI text encoding is not supported."));
    }

    headers.compression = *compression;
    headers.textLength = *textLength;
    headers.textRecordCount = *textRecordCount;
    headers.textRecordSize = *textRecordSize;
    headers.encoding = *encoding;
    headers.version = *version;

    if (*headerLength >= 228 && canRead(record0, 242, 2)) {
        headers.extraFlags = *read16(record0, 242);
    }

    if (*headerLength >= 76) {
        const auto titleOffset = read32(record0, 84);
        const auto titleLength = read32(record0, 88);
        if (!titleOffset.has_value() || !titleLength.has_value()) {
            return error(MobiParseErrorCode::InvalidHeader,
                         QStringLiteral("The MOBI title fields are truncated."));
        }
        if (*titleLength > 0) {
            if (*titleOffset > static_cast<quint64>(record0.size()) ||
                *titleLength > static_cast<quint64>(record0.size()) - *titleOffset) {
                return error(MobiParseErrorCode::InvalidHeader,
                             QStringLiteral("The MOBI full-name field lies outside Record 0."));
            }
            const auto decoded =
                decodeString(record0.sliced(*titleOffset, *titleLength), *encoding);
            if (!decoded.has_value()) {
                return error(MobiParseErrorCode::InvalidHeader,
                             QStringLiteral("The MOBI full-name field is not valid text."));
            }
            headers.title = text::TextNormalizer::normalizeBlockLine(*decoded);
        }
    }

    const auto exthFlags = *headerLength >= 116 ? read32(record0, 128) : std::nullopt;
    if (exthFlags.has_value() && (*exthFlags & 0x40U) != 0) {
        const auto exthOffset = static_cast<qsizetype>(16 + *headerLength);
        if (!canRead(record0, exthOffset, 12) ||
            record0.sliced(exthOffset, 4) != QByteArrayView("EXTH", 4)) {
            return error(MobiParseErrorCode::InvalidHeader,
                         QStringLiteral("The MOBI header advertises a missing EXTH block."));
        }
        const auto exthLength = read32(record0, exthOffset + 4);
        const auto exthCount = read32(record0, exthOffset + 8);
        if (!exthLength.has_value() || !exthCount.has_value() || *exthLength < 12 ||
            *exthCount > 1'024 || *exthLength > static_cast<quint64>(record0.size() - exthOffset)) {
            return error(MobiParseErrorCode::InvalidHeader,
                         QStringLiteral("The EXTH metadata block is malformed."));
        }
        const auto exthEnd = exthOffset + static_cast<qsizetype>(*exthLength);
        auto cursor = exthOffset + 12;
        for (quint32 index = 0; index < *exthCount; ++index) {
            if (cursor > exthEnd || exthEnd - cursor < 8) {
                return error(MobiParseErrorCode::InvalidHeader,
                             QStringLiteral("The EXTH metadata record table is truncated."));
            }
            const auto tag = read32(record0, cursor);
            const auto size = read32(record0, cursor + 4);
            if (!tag.has_value() || !size.has_value() || *size < 8 ||
                *size > static_cast<quint64>(exthEnd - cursor)) {
                return error(MobiParseErrorCode::InvalidHeader,
                             QStringLiteral("An EXTH metadata record is malformed."));
            }
            const auto value = record0.sliced(cursor + 8, static_cast<qsizetype>(*size) - 8);
            if (*tag == 99 || *tag == 100 || *tag == 524) {
                const auto decoded = decodeString(value, *encoding);
                if (!decoded.has_value()) {
                    return error(MobiParseErrorCode::InvalidHeader,
                                 QStringLiteral("An EXTH text record has invalid encoding."));
                }
                const auto normalized = text::TextNormalizer::normalizeBlockLine(*decoded);
                if (*tag == 99 && !normalized.isEmpty()) {
                    headers.title = normalized;
                } else if (*tag == 100 && !normalized.isEmpty()) {
                    headers.authors.append(normalized);
                } else if (*tag == 524 && !normalized.isEmpty()) {
                    headers.language = normalized;
                }
            }
            cursor += static_cast<qsizetype>(*size);
        }
    }
    return headers;
}

std::optional<qsizetype> trailingDataSize(QByteArrayView record, quint16 flags) {
    if (flags == 0) {
        return 0;
    }
    qsizetype cursor = record.size() - 1;
    qsizetype total = 0;
    for (int bit = 15; bit > 0; --bit) {
        if ((flags & (1U << bit)) == 0) {
            continue;
        }
        quint32 value = 0;
        int shift = 0;
        qsizetype encodedLength = 0;
        bool stopped = false;
        while (encodedLength < 4 && cursor - encodedLength >= 0) {
            const auto byte = static_cast<uchar>(record.at(cursor - encodedLength));
            value |= static_cast<quint32>(byte & 0x7FU) << shift;
            shift += 7;
            ++encodedLength;
            if ((byte & 0x80U) != 0) {
                stopped = true;
                break;
            }
        }
        if (!stopped || value < static_cast<quint32>(encodedLength) ||
            value > static_cast<quint64>(cursor + 1)) {
            return std::nullopt;
        }
        cursor -= static_cast<qsizetype>(value);
        total += static_cast<qsizetype>(value);
    }
    if ((flags & 1U) != 0) {
        if (cursor < 0) {
            return std::nullopt;
        }
        const auto size = (static_cast<uchar>(record.at(cursor)) & 0x03U) + 1;
        if (size > cursor + 1) {
            return std::nullopt;
        }
        total += size;
    }
    return total <= record.size() ? std::optional<qsizetype>(total) : std::nullopt;
}

std::variant<QByteArray, MobiParseError> decompressPalmDoc(QByteArrayView compressed,
                                                           qsizetype outputLimit) {
    QByteArray output;
    output.reserve(qMin(outputLimit, compressed.size() * 2));
    for (qsizetype cursor = 0; cursor < compressed.size();) {
        const auto byte = static_cast<uchar>(compressed.at(cursor++));
        if (byte == 0 || (byte >= 0x09 && byte <= 0x7F)) {
            if (output.size() >= outputLimit) {
                return error(MobiParseErrorCode::ResourceLimitExceeded,
                             QStringLiteral("A PalmDOC text record exceeds its declared limit."));
            }
            output.append(static_cast<char>(byte));
        } else if (byte <= 0x08) {
            if (byte > compressed.size() - cursor || byte > outputLimit - output.size()) {
                return error(MobiParseErrorCode::CorruptText,
                             QStringLiteral("A PalmDOC literal run is truncated."));
            }
            output.append(compressed.sliced(cursor, byte));
            cursor += byte;
        } else if (byte <= 0xBF) {
            if (cursor >= compressed.size()) {
                return error(MobiParseErrorCode::CorruptText,
                             QStringLiteral("A PalmDOC back-reference is truncated."));
            }
            const auto next = static_cast<uchar>(compressed.at(cursor++));
            const auto pair = static_cast<quint16>((byte << 8) | next);
            const auto distance = static_cast<qsizetype>((pair >> 3) & 0x07FFU);
            const auto length = static_cast<qsizetype>((pair & 0x07U) + 3);
            if (distance == 0 || distance > output.size() || length > outputLimit - output.size()) {
                return error(MobiParseErrorCode::CorruptText,
                             QStringLiteral("A PalmDOC back-reference is invalid."));
            }
            for (qsizetype index = 0; index < length; ++index) {
                output.append(output.at(output.size() - distance));
            }
        } else {
            if (output.size() > outputLimit - 2) {
                return error(MobiParseErrorCode::ResourceLimitExceeded,
                             QStringLiteral("A PalmDOC text record exceeds its declared limit."));
            }
            output.append(' ');
            output.append(static_cast<char>(byte ^ 0x80U));
        }
    }
    return output;
}

std::variant<QByteArray, MobiParseError> decodedText(QByteArrayView source,
                                                     const MobiHeaders& headers) {
    QByteArray textBytes;
    if (headers.textLength > kMaximumDecodedBytes) {
        return error(MobiParseErrorCode::ResourceLimitExceeded,
                     QStringLiteral("The declared MOBI text exceeds the 512 MiB import limit."));
    }
    textBytes.reserve(static_cast<qsizetype>(headers.textLength));
    const auto recordLimit = qMax<qsizetype>(4'096, headers.textRecordSize);
    for (qsizetype index = 1; index <= headers.textRecordCount; ++index) {
        const auto range = headers.records.at(index);
        auto record = source.sliced(range.start, range.end - range.start);
        const auto trailerSize = trailingDataSize(record, headers.extraFlags);
        if (!trailerSize.has_value() || *trailerSize > record.size()) {
            return error(MobiParseErrorCode::CorruptText,
                         QStringLiteral("A MOBI text-record trailer is malformed."));
        }
        record = record.first(record.size() - *trailerSize);
        QByteArray decodedRecord;
        if (headers.compression == kNoCompression) {
            if (record.size() > recordLimit) {
                return error(MobiParseErrorCode::CorruptText,
                             QStringLiteral("An uncompressed MOBI text record is too large."));
            }
            decodedRecord = QByteArray(record.data(), record.size());
        } else {
            auto result = decompressPalmDoc(record, recordLimit);
            if (std::holds_alternative<MobiParseError>(result)) {
                return std::get<MobiParseError>(result);
            }
            decodedRecord = std::get<QByteArray>(std::move(result));
        }
        if (decodedRecord.size() > kMaximumDecodedBytes - textBytes.size()) {
            return error(MobiParseErrorCode::ResourceLimitExceeded,
                         QStringLiteral("The decoded MOBI text exceeds the import limit."));
        }
        textBytes.append(decodedRecord);
    }
    if (textBytes.size() < headers.textLength) {
        return error(MobiParseErrorCode::CorruptText,
                     QStringLiteral("The decoded MOBI text is shorter than Record 0 declares."));
    }
    textBytes.truncate(static_cast<qsizetype>(headers.textLength));
    if (headers.version <= 3) {
        textBytes.replace(QByteArrayView("\0", 1), QByteArrayView());
    }

    const auto decoded = decodeString(textBytes, headers.encoding);
    if (!decoded.has_value()) {
        return error(MobiParseErrorCode::CorruptText,
                     QStringLiteral("The decoded MOBI text is invalid for its declared encoding."));
    }
    return decoded->toUtf8();
}

qsizetype tagEnd(QByteArrayView bytes, qsizetype start) {
    char quote = 0;
    for (auto cursor = start + 1; cursor < bytes.size(); ++cursor) {
        const auto current = bytes.at(cursor);
        if (quote != 0) {
            if (current == quote) {
                quote = 0;
            }
        } else if (current == '\'' || current == '"') {
            quote = current;
        } else if (current == '>') {
            return cursor + 1;
        }
    }
    return -1;
}

QString tagName(QByteArrayView tag) {
    qsizetype cursor = 1;
    while (cursor < tag.size() &&
           (tag.at(cursor) == '/' || tag.at(cursor) == ' ' || tag.at(cursor) == '\t' ||
            tag.at(cursor) == '\r' || tag.at(cursor) == '\n')) {
        ++cursor;
    }
    const auto start = cursor;
    while (cursor < tag.size()) {
        const auto value = tag.at(cursor);
        if (!(value == ':' || value == '-' || value == '_' || (value >= '0' && value <= '9') ||
              (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z'))) {
            break;
        }
        ++cursor;
    }
    auto name = QString::fromLatin1(tag.sliced(start, cursor - start)).toLower();
    const auto separator = name.lastIndexOf(QLatin1Char(':'));
    return name.mid(separator + 1);
}

QString attributeValue(QByteArrayView tag, QStringView wanted) {
    const auto text = QString::fromUtf8(tag.data(), tag.size());
    const QRegularExpression expression(
        QStringLiteral(R"((?:^|\s)(?:[A-Za-z_][\w.-]*:)?%1\s*=\s*(["'])(.*?)\1)")
            .arg(QRegularExpression::escape(wanted.toString())),
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);
    const auto match = expression.match(text);
    return match.hasMatch() ? match.captured(2) : QString{};
}

QString decodeEntities(QString text) {
    text.replace(QStringLiteral("&nbsp;"), QStringLiteral("&#160;"), Qt::CaseInsensitive);
    QString result;
    qsizetype cursor = 0;
    static const QRegularExpression entity(
        QStringLiteral(R"(&(?:#x[0-9A-Fa-f]+|#[0-9]+|amp|lt|gt|quot|apos);)"),
        QRegularExpression::CaseInsensitiveOption);
    auto matches = entity.globalMatch(text);
    while (matches.hasNext()) {
        const auto match = matches.next();
        result += text.mid(cursor, match.capturedStart() - cursor);
        const auto token = match.captured().toLower();
        if (token == QStringLiteral("&amp;")) {
            result += QLatin1Char('&');
        } else if (token == QStringLiteral("&lt;")) {
            result += QLatin1Char('<');
        } else if (token == QStringLiteral("&gt;")) {
            result += QLatin1Char('>');
        } else if (token == QStringLiteral("&quot;")) {
            result += QLatin1Char('"');
        } else if (token == QStringLiteral("&apos;")) {
            result += QLatin1Char('\'');
        } else {
            bool ok = false;
            const auto hexadecimal = token.startsWith(QStringLiteral("&#x"));
            const auto digits =
                token.mid(hexadecimal ? 3 : 2, token.size() - (hexadecimal ? 4 : 3));
            const auto codePoint = digits.toUInt(&ok, hexadecimal ? 16 : 10);
            if (ok && codePoint <= 0x10FFFF && !(codePoint >= 0xD800 && codePoint <= 0xDFFF)) {
                const auto character = static_cast<char32_t>(codePoint);
                result += QString::fromUcs4(&character, 1);
            } else {
                result += match.captured();
            }
        }
        cursor = match.capturedEnd();
    }
    result += text.mid(cursor);
    return result;
}

QString fragmentText(QByteArrayView fragment) {
    QString output;
    QString ignoredElement;
    int ignoredDepth = 0;
    qsizetype cursor = 0;
    while (cursor < fragment.size()) {
        const auto opening = fragment.indexOf('<', cursor);
        const auto textEnd = opening < 0 ? fragment.size() : opening;
        if (ignoredDepth == 0) {
            output += QString::fromUtf8(fragment.sliced(cursor, textEnd - cursor));
        }
        if (opening < 0) {
            break;
        }
        const auto remainder = fragment.sliced(opening);
        if (remainder.startsWith("<!--")) {
            const auto relativeEnd = remainder.indexOf("-->", 4);
            cursor = relativeEnd < 0 ? fragment.size() : opening + relativeEnd + 3;
            continue;
        }
        const auto end = tagEnd(fragment, opening);
        if (end < 0) {
            break;
        }
        const auto tag = fragment.sliced(opening, end - opening);
        const auto name = tagName(tag);
        const auto closing = tag.size() > 1 && tag.at(1) == '/';
        const auto selfClosing = tag.trimmed().endsWith("/>");
        if (ignoredDepth > 0) {
            if (name == ignoredElement) {
                ignoredDepth += closing ? -1 : (selfClosing ? 0 : 1);
                if (ignoredDepth == 0) {
                    ignoredElement.clear();
                }
            }
        } else if (!closing && (name == QStringLiteral("script") ||
                                name == QStringLiteral("style") || name == QStringLiteral("svg"))) {
            if (!selfClosing) {
                ignoredElement = name;
                ignoredDepth = 1;
            }
        } else if (name == QStringLiteral("br")) {
            output += QLatin1Char(' ');
        } else if (!closing && name == QStringLiteral("img")) {
            output += QLatin1Char(' ');
            output += attributeValue(tag, QStringLiteral("alt"));
            output += QLatin1Char(' ');
        }
        cursor = end;
    }
    return text::TextNormalizer::normalizeBlockLine(decodeEntities(output).simplified());
}

std::optional<document::BlockType> blockType(QStringView name) {
    if (name.size() == 2 && name.at(0) == QLatin1Char('h') && name.at(1) >= QLatin1Char('1') &&
        name.at(1) <= QLatin1Char('6')) {
        return document::BlockType::Heading;
    }
    static const QSet<QString> paragraphs = {QStringLiteral("p"),          QStringLiteral("li"),
                                             QStringLiteral("blockquote"), QStringLiteral("pre"),
                                             QStringLiteral("dt"),         QStringLiteral("dd")};
    return paragraphs.contains(name.toString())
               ? std::optional<document::BlockType>(document::BlockType::Paragraph)
               : std::nullopt;
}

QList<ExtractedBlock> extractBlocks(const QByteArray& bytes) {
    QList<ExtractedBlock> blocks;
    struct ActiveBlock final {
        document::BlockType type;
        QString name;
        qsizetype start = 0;
        qsizetype contentStart = 0;
        int sameNameDepth = 1;
    };
    std::optional<ActiveBlock> active;
    const QRegularExpression bodyExpression(QStringLiteral(R"(<\s*body(?:\s|>))"),
                                            QRegularExpression::CaseInsensitiveOption);
    const auto hasBody = bodyExpression.match(QString::fromUtf8(bytes)).hasMatch();
    bool inBody = !hasBody;
    QString ignoredElement;
    int ignoredDepth = 0;

    const auto finishActive = [&](qsizetype end) {
        if (!active.has_value()) {
            return;
        }
        const auto contentEnd = qMax(active->contentStart, end);
        const auto value = fragmentText(
            QByteArrayView(bytes).sliced(active->contentStart, contentEnd - active->contentStart));
        if (!value.isEmpty()) {
            blocks.append({active->type, value, active->start, end});
        }
        active.reset();
    };

    qsizetype cursor = 0;
    while ((cursor = bytes.indexOf('<', cursor)) >= 0) {
        if (QByteArrayView(bytes).sliced(cursor).startsWith("<!--")) {
            const auto commentEnd = bytes.indexOf("-->", cursor + 4);
            cursor = commentEnd < 0 ? bytes.size() : commentEnd + 3;
            continue;
        }
        const auto end = tagEnd(bytes, cursor);
        if (end < 0) {
            break;
        }
        const auto tag = QByteArrayView(bytes).sliced(cursor, end - cursor);
        const auto name = tagName(tag);
        const auto closing = tag.size() > 1 && tag.at(1) == '/';
        const auto selfClosing = tag.trimmed().endsWith("/>");
        if (ignoredDepth > 0) {
            if (name == ignoredElement) {
                ignoredDepth += closing ? -1 : (selfClosing ? 0 : 1);
                if (ignoredDepth == 0) {
                    ignoredElement.clear();
                }
            }
            cursor = end;
            continue;
        }
        if (!closing && name == QStringLiteral("body")) {
            inBody = true;
            cursor = end;
            continue;
        }
        if (closing && name == QStringLiteral("body")) {
            finishActive(cursor);
            inBody = false;
            cursor = end;
            continue;
        }
        if (!inBody) {
            cursor = end;
            continue;
        }
        if (!closing && (name == QStringLiteral("script") || name == QStringLiteral("style") ||
                         name == QStringLiteral("svg") || name == QStringLiteral("head"))) {
            if (!selfClosing) {
                ignoredElement = name;
                ignoredDepth = 1;
            }
            cursor = end;
            continue;
        }
        if (!closing) {
            if (active.has_value() && name == active->name && !selfClosing) {
                ++active->sameNameDepth;
            } else if (!active.has_value()) {
                if (name == QStringLiteral("hr")) {
                    blocks.append({document::BlockType::SceneBreak, {}, cursor, end});
                } else if (const auto type = blockType(name); type.has_value()) {
                    active = ActiveBlock{*type, name, cursor, end, 1};
                }
            }
        } else if (active.has_value() && name == active->name) {
            --active->sameNameDepth;
            if (active->sameNameDepth == 0) {
                finishActive(end);
            }
        }
        cursor = end;
    }
    finishActive(bytes.size());
    return blocks;
}

void appendChapter(document::Document& document, QString title) {
    const auto index = document.chapters.size();
    const auto chapterKey =
        document.id.toString() + QStringLiteral(":chapter:") + QString::number(index);
    document.chapters.append(
        {core::ChapterId::fromStableKey(chapterKey),
         index,
         title.trimmed().isEmpty() ? QStringLiteral("Untitled chapter") : title.trimmed(),
         {}});
}

} // namespace

MobiParseResult MobiParser::parse(QByteArrayView source, const MobiImportOptions& options) {
    const auto sourceLocator = options.sourceLocator.trimmed();
    if (sourceLocator.isEmpty()) {
        return error(MobiParseErrorCode::MissingSourceLocator,
                     QStringLiteral("A source locator is required for reproducible import."));
    }
    const auto headerResult = readHeaders(source);
    if (std::holds_alternative<MobiParseError>(headerResult)) {
        return std::get<MobiParseError>(headerResult);
    }
    const auto& headers = std::get<MobiHeaders>(headerResult);
    auto textResult = decodedText(source, headers);
    if (std::holds_alternative<MobiParseError>(textResult)) {
        return std::get<MobiParseError>(textResult);
    }
    const auto& utf8Text = std::get<QByteArray>(textResult);
    const auto extracted = extractBlocks(utf8Text);
    if (extracted.isEmpty()) {
        return error(MobiParseErrorCode::EmptyInput,
                     QStringLiteral("The MOBI book contains no importable text blocks."));
    }

    auto title = text::TextNormalizer::normalizeBlockLine(options.title);
    if (title.isEmpty()) {
        title = headers.title;
    }
    if (title.isEmpty()) {
        title =
            text::TextNormalizer::normalizeBlockLine(QFileInfo(sourceLocator).completeBaseName());
    }
    if (title.isEmpty()) {
        title = QStringLiteral("Untitled");
    }
    const auto bookId = core::BookId::fromStableKey(sourceLocator);
    document::Document document{
        bookId,
        {title, options.authors.isEmpty() ? headers.authors : options.authors,
         options.language.trimmed().isEmpty()
             ? (headers.language.isEmpty() ? QStringLiteral("und") : headers.language)
             : options.language.trimmed(),
         QStringLiteral("mobi"), sourceLocator, core::ContentHash::sha256(source)},
        {},
    };
    const auto sourceId = sourceLocator + QStringLiteral("#text&layer=palmdoc-decoded-utf8");
    for (const auto& block : extracted) {
        auto type = block.type;
        auto chapterTitle = type == document::BlockType::Heading
                                ? std::optional<QString>(block.text)
                                : text::ChapterHeadingDetector::detect(block.text);
        if (chapterTitle.has_value()) {
            type = document::BlockType::Heading;
            appendChapter(document, *chapterTitle);
        } else if (document.chapters.isEmpty()) {
            appendChapter(document, title);
        }
        if (type == document::BlockType::Paragraph &&
            text::ParagraphDetector::isSceneBreak(block.text)) {
            type = document::BlockType::SceneBreak;
        }
        document.chapters.last().blocks.append(
            {type,
             type == document::BlockType::SceneBreak ? QString{} : block.text,
             {sourceId, block.startByte, block.endByte}});
    }

    const auto validation = document::validateDocument(document);
    if (!validation.isValid()) {
        return error(MobiParseErrorCode::InvalidDocument,
                     QStringLiteral("The imported document failed domain validation: %1")
                         .arg(validation.errors.first().message));
    }
    return document;
}

MobiParseResult MobiParser::parseFile(QStringView filePath, MobiImportOptions options) {
    const QFileInfo fileInfo(filePath.toString());
    if (!fileInfo.exists() || !fileInfo.isFile()) {
        return error(MobiParseErrorCode::FileNotFound,
                     QStringLiteral("The MOBI file does not exist."));
    }
    if (fileInfo.size() > kMaximumSourceBytes) {
        return error(MobiParseErrorCode::ResourceLimitExceeded,
                     QStringLiteral("The MOBI file exceeds the 512 MiB import limit."));
    }
    QFile file(fileInfo.absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly)) {
        return error(MobiParseErrorCode::ReadFailure,
                     QStringLiteral("The MOBI file could not be read: %1").arg(file.errorString()));
    }
    if (options.sourceLocator.trimmed().isEmpty()) {
        options.sourceLocator = QDir::cleanPath(fileInfo.absoluteFilePath());
    }
    const auto source = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        return error(MobiParseErrorCode::ReadFailure,
                     QStringLiteral("The MOBI file could not be read completely: %1")
                         .arg(file.errorString()));
    }
    return parse(source, options);
}

} // namespace loreforge::parser
