#include "loreforge/parser/epub_parser.h"

#include "loreforge/core/content_hash.h"
#include "loreforge/core/identifier.h"
#include "loreforge/document/document_validation.h"
#include "loreforge/text/text_normalizer.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>
#include <QXmlStreamReader>
#include <QtCore/private/qzipreader_p.h>
#include <QtEndian>

#include <algorithm>
#include <optional>
#include <utility>

namespace loreforge::parser {
namespace {

constexpr qsizetype kMaximumEntries = 10'000;
constexpr qint64 kMaximumEntryBytes = 64 * 1024 * 1024;
constexpr qint64 kMaximumTotalBytes = 512 * 1024 * 1024;

struct Archive final {
    QHash<QString, QByteArray> files;
    QStringList order;
};

struct ManifestItem final {
    QString id;
    QString path;
    QString mediaType;
    QString properties;
};

struct Package final {
    QString title;
    QStringList authors;
    QString language;
    QString packagePath;
    QHash<QString, ManifestItem> manifest;
    QStringList spine;
    QString tocId;
};

struct NavigationEntry final {
    QString path;
    QString fragment;
    QString title;
};

struct ExtractedBlock final {
    document::Block block;
    QString anchor;
};

struct ExtractedResource final {
    QString path;
    QList<ExtractedBlock> blocks;
    QHash<QString, qint64> anchors;
};

EpubParseError error(EpubParseErrorCode code, QString message) {
    return {code, std::move(message)};
}

bool hasValidMimetypeHeader(QByteArrayView source) {
    constexpr qsizetype headerSize = 30;
    constexpr QByteArrayView signature("PK\x03\x04", 4);
    constexpr QByteArrayView fileName("mimetype", 8);
    constexpr QByteArrayView mediaType("application/epub+zip", 20);
    if (source.size() < headerSize + fileName.size() + mediaType.size() ||
        source.first(4) != signature) {
        return false;
    }
    const auto* bytes = reinterpret_cast<const uchar*>(source.data());
    const auto flags = qFromLittleEndian<quint16>(bytes + 6);
    const auto compression = qFromLittleEndian<quint16>(bytes + 8);
    const auto compressedSize = qFromLittleEndian<quint32>(bytes + 18);
    const auto uncompressedSize = qFromLittleEndian<quint32>(bytes + 22);
    const auto fileNameSize = qFromLittleEndian<quint16>(bytes + 26);
    const auto extraSize = qFromLittleEndian<quint16>(bytes + 28);
    return (flags & 0x0001U) == 0 && compression == 0 && compressedSize == mediaType.size() &&
           uncompressedSize == mediaType.size() && fileNameSize == fileName.size() &&
           extraSize == 0 && source.sliced(headerSize, fileName.size()) == fileName &&
           source.sliced(headerSize + fileName.size(), mediaType.size()) == mediaType;
}

QString localName(QStringView qualifiedName) {
    const auto separator = qualifiedName.lastIndexOf(QLatin1Char(':'));
    return qualifiedName.mid(separator + 1).toString().toLower();
}

bool isSafeArchivePath(QStringView path) {
    if (path.isEmpty() || path.startsWith(QLatin1Char('/')) || path.startsWith(QLatin1Char('\\')) ||
        path.contains(QLatin1Char('\\')) ||
        QRegularExpression(QStringLiteral("^[A-Za-z]:")).matchView(path).hasMatch()) {
        return false;
    }
    const auto segments = path.split(QLatin1Char('/'));
    return std::ranges::none_of(segments, [](QStringView segment) {
        return segment.isEmpty() || segment == QStringView(u".") || segment == QStringView(u"..");
    });
}

std::variant<Archive, EpubParseError> readArchive(QByteArrayView source) {
    if (!hasValidMimetypeHeader(source)) {
        return error(
            EpubParseErrorCode::InvalidContainer,
            QStringLiteral("The first EPUB entry must be the exact, uncompressed mimetype."));
    }
    QByteArray bytes(source.data(), source.size());
    QBuffer buffer(&bytes);
    if (!buffer.open(QIODevice::ReadOnly)) {
        return error(EpubParseErrorCode::ReadFailure,
                     QStringLiteral("The EPUB bytes could not be opened."));
    }

    QZipReader reader(&buffer);
    if (!reader.exists() || !reader.isReadable() || reader.status() != QZipReader::NoError) {
        return error(EpubParseErrorCode::InvalidContainer,
                     QStringLiteral("The source is not a readable ZIP container."));
    }

    const auto entries = reader.fileInfoList();
    if (entries.size() > kMaximumEntries) {
        return error(EpubParseErrorCode::ResourceLimitExceeded,
                     QStringLiteral("The EPUB contains too many archive entries."));
    }

    Archive archive;
    qint64 totalBytes = 0;
    for (const auto& entry : entries) {
        auto pathForValidation = entry.filePath;
        if (entry.isDir && pathForValidation.endsWith(QLatin1Char('/'))) {
            pathForValidation.chop(1);
        }
        if (!isSafeArchivePath(pathForValidation) || entry.isSymLink) {
            return error(
                EpubParseErrorCode::UnsafeArchive,
                QStringLiteral("The EPUB contains an unsafe archive path: %1").arg(entry.filePath));
        }
        if (archive.files.contains(entry.filePath)) {
            return error(EpubParseErrorCode::UnsafeArchive,
                         QStringLiteral("The EPUB contains a duplicate archive path: %1")
                             .arg(entry.filePath));
        }
        if (!entry.isFile) {
            continue;
        }
        if (entry.size < 0 || entry.size > kMaximumEntryBytes ||
            totalBytes > kMaximumTotalBytes - entry.size) {
            return error(EpubParseErrorCode::ResourceLimitExceeded,
                         QStringLiteral("The EPUB exceeds the import size limits."));
        }
        const auto data = reader.fileData(entry.filePath);
        if (reader.status() != QZipReader::NoError || data.size() != entry.size) {
            return error(
                EpubParseErrorCode::ReadFailure,
                QStringLiteral("An EPUB archive entry could not be read: %1").arg(entry.filePath));
        }
        totalBytes += entry.size;
        archive.files.insert(entry.filePath, data);
        archive.order.append(entry.filePath);
    }

    if (archive.order.isEmpty() || archive.order.first() != QStringLiteral("mimetype") ||
        archive.files.value(QStringLiteral("mimetype")) !=
            QByteArrayLiteral("application/epub+zip")) {
        return error(EpubParseErrorCode::InvalidContainer,
                     QStringLiteral("The EPUB mimetype entry is missing, misplaced, or invalid."));
    }
    return archive;
}

std::optional<QString> resolvedPath(QStringView basePath, QStringView reference) {
    const QUrl url(reference.toString());
    if (!url.isRelative()) {
        return std::nullopt;
    }
    const auto decodedPath = url.path(QUrl::FullyDecoded);
    if (decodedPath.isEmpty() || decodedPath.contains(QLatin1Char('\\'))) {
        return std::nullopt;
    }
    const auto baseDirectory = QFileInfo(basePath.toString()).path();
    const auto joined = QDir::cleanPath(QDir(baseDirectory).filePath(decodedPath));
    if (!isSafeArchivePath(joined)) {
        return std::nullopt;
    }
    return joined;
}

std::variant<QString, EpubParseError> packagePath(const Archive& archive) {
    const auto container = archive.files.value(QStringLiteral("META-INF/container.xml"));
    if (container.isEmpty()) {
        return error(EpubParseErrorCode::InvalidContainer,
                     QStringLiteral("META-INF/container.xml is missing."));
    }
    QXmlStreamReader xml(container);
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement() && localName(xml.qualifiedName()) == QStringLiteral("rootfile")) {
            const auto path = xml.attributes().value(QStringLiteral("full-path")).toString();
            if (!isSafeArchivePath(path) || !archive.files.contains(path)) {
                return error(EpubParseErrorCode::InvalidContainer,
                             QStringLiteral("The package document path is invalid or missing."));
            }
            return path;
        }
    }
    return error(EpubParseErrorCode::InvalidContainer,
                 xml.hasError()
                     ? QStringLiteral("container.xml is malformed: %1").arg(xml.errorString())
                     : QStringLiteral("container.xml has no rootfile."));
}

std::variant<Package, EpubParseError> readPackage(const Archive& archive, QString packagePath) {
    Package package;
    package.packagePath = std::move(packagePath);
    QXmlStreamReader xml(archive.files.value(package.packagePath));
    bool inMetadata = false;
    bool inManifest = false;
    bool inSpine = false;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement()) {
            const auto name = localName(xml.qualifiedName());
            if (name == QStringLiteral("metadata")) {
                inMetadata = true;
            } else if (name == QStringLiteral("manifest")) {
                inManifest = true;
            } else if (name == QStringLiteral("spine")) {
                inSpine = true;
                package.tocId = xml.attributes().value(QStringLiteral("toc")).toString();
            } else if (inMetadata && name == QStringLiteral("title") && package.title.isEmpty()) {
                package.title = xml.readElementText().trimmed();
            } else if (inMetadata && name == QStringLiteral("creator")) {
                const auto creator = xml.readElementText().trimmed();
                if (!creator.isEmpty()) {
                    package.authors.append(creator);
                }
            } else if (inMetadata && name == QStringLiteral("language") &&
                       package.language.isEmpty()) {
                package.language = xml.readElementText().trimmed();
            } else if (inManifest && name == QStringLiteral("item")) {
                const auto attributes = xml.attributes();
                ManifestItem item;
                item.id = attributes.value(QStringLiteral("id")).toString();
                item.mediaType = attributes.value(QStringLiteral("media-type")).toString();
                item.properties = attributes.value(QStringLiteral("properties")).toString();
                const auto path =
                    resolvedPath(package.packagePath, attributes.value(QStringLiteral("href")));
                if (item.id.isEmpty() || !path.has_value() || package.manifest.contains(item.id)) {
                    return error(EpubParseErrorCode::InvalidPackage,
                                 QStringLiteral("The package manifest contains an invalid item."));
                }
                item.path = *path;
                package.manifest.insert(item.id, item);
            } else if (inSpine && name == QStringLiteral("itemref")) {
                const auto id = xml.attributes().value(QStringLiteral("idref")).toString();
                if (id.isEmpty()) {
                    return error(EpubParseErrorCode::InvalidPackage,
                                 QStringLiteral("The package spine contains an empty idref."));
                }
                if (xml.attributes().value(QStringLiteral("linear")) != QStringLiteral("no")) {
                    package.spine.append(id);
                }
            }
        } else if (xml.isEndElement()) {
            const auto name = localName(xml.qualifiedName());
            inMetadata = inMetadata && name != QStringLiteral("metadata");
            inManifest = inManifest && name != QStringLiteral("manifest");
            inSpine = inSpine && name != QStringLiteral("spine");
        }
    }
    if (xml.hasError() || package.manifest.isEmpty() || package.spine.isEmpty()) {
        return error(
            EpubParseErrorCode::InvalidPackage,
            xml.hasError()
                ? QStringLiteral("The package document is malformed: %1").arg(xml.errorString())
                : QStringLiteral("The package manifest or spine is empty."));
    }
    for (const auto& id : package.spine) {
        if (!package.manifest.contains(id) ||
            !archive.files.contains(package.manifest.value(id).path)) {
            return error(EpubParseErrorCode::MissingManifestItem,
                         QStringLiteral("A spine resource is missing from the EPUB: %1").arg(id));
        }
    }
    return package;
}

QString navigationType(const QXmlStreamAttributes& attributes) {
    for (const auto& attribute : attributes) {
        if (localName(attribute.qualifiedName()) == QStringLiteral("type")) {
            return attribute.value().toString();
        }
    }
    return {};
}

std::variant<QList<NavigationEntry>, EpubParseError>
readHtmlNavigation(const Archive& archive, const ManifestItem& navItem) {
    QList<NavigationEntry> entries;
    QXmlStreamReader xml(archive.files.value(navItem.path));
    bool inToc = false;
    int tocDepth = -1;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement()) {
            const auto name = localName(xml.qualifiedName());
            if (name == QStringLiteral("nav") && navigationType(xml.attributes())
                                                     .split(QLatin1Char(' '))
                                                     .contains(QStringLiteral("toc"))) {
                inToc = true;
                tocDepth = 1;
            } else if (inToc) {
                ++tocDepth;
                if (name == QStringLiteral("a")) {
                    const QUrl href(xml.attributes().value(QStringLiteral("href")).toString());
                    const auto path = resolvedPath(navItem.path, href.path(QUrl::FullyDecoded));
                    const auto title =
                        xml.readElementText(QXmlStreamReader::IncludeChildElements).simplified();
                    --tocDepth;
                    if (!path.has_value() || title.isEmpty()) {
                        return error(
                            EpubParseErrorCode::InvalidNavigation,
                            QStringLiteral("The EPUB navigation contains an invalid link."));
                    }
                    entries.append({*path, href.fragment(QUrl::FullyDecoded), title});
                }
            }
        } else if (xml.isEndElement() && inToc) {
            --tocDepth;
            if (tocDepth == 0) {
                inToc = false;
            }
        }
    }
    if (xml.hasError()) {
        return error(
            EpubParseErrorCode::InvalidNavigation,
            QStringLiteral("The EPUB navigation document is malformed: %1").arg(xml.errorString()));
    }
    return entries;
}

std::variant<QList<NavigationEntry>, EpubParseError>
readNcxNavigation(const Archive& archive, const Package& package, const ManifestItem& ncxItem) {
    static_cast<void>(package);
    QList<NavigationEntry> entries;
    QXmlStreamReader xml(archive.files.value(ncxItem.path));
    QString pendingTitle;
    while (!xml.atEnd()) {
        xml.readNext();
        if (!xml.isStartElement()) {
            continue;
        }
        const auto name = localName(xml.qualifiedName());
        if (name == QStringLiteral("text")) {
            pendingTitle = xml.readElementText().simplified();
        } else if (name == QStringLiteral("content")) {
            const QUrl href(xml.attributes().value(QStringLiteral("src")).toString());
            const auto path = resolvedPath(ncxItem.path, href.path(QUrl::FullyDecoded));
            if (!path.has_value() || pendingTitle.isEmpty()) {
                return error(EpubParseErrorCode::InvalidNavigation,
                             QStringLiteral("The EPUB NCX contains an invalid target."));
            }
            entries.append({*path, href.fragment(QUrl::FullyDecoded), pendingTitle});
            pendingTitle.clear();
        }
    }
    if (xml.hasError()) {
        return error(EpubParseErrorCode::InvalidNavigation,
                     QStringLiteral("The EPUB NCX is malformed: %1").arg(xml.errorString()));
    }
    return entries;
}

std::variant<QList<NavigationEntry>, EpubParseError> readNavigation(const Archive& archive,
                                                                    const Package& package) {
    for (const auto& item : package.manifest) {
        if (item.properties.split(QLatin1Char(' ')).contains(QStringLiteral("nav"))) {
            if (!archive.files.contains(item.path)) {
                return error(EpubParseErrorCode::MissingManifestItem,
                             QStringLiteral("The EPUB navigation resource is missing."));
            }
            return readHtmlNavigation(archive, item);
        }
    }
    if (!package.tocId.isEmpty()) {
        const auto item = package.manifest.value(package.tocId);
        if (item.id.isEmpty() || !archive.files.contains(item.path)) {
            return error(EpubParseErrorCode::MissingManifestItem,
                         QStringLiteral("The EPUB NCX resource is missing."));
        }
        return readNcxNavigation(archive, package, item);
    }
    return QList<NavigationEntry>{};
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
    if (cursor < tag.size() && tag.at(cursor) == '/') {
        ++cursor;
    }
    while (cursor < tag.size() && (tag.at(cursor) == '!' || tag.at(cursor) == '?')) {
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
    return localName(QString::fromLatin1(tag.sliced(start, cursor - start)));
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
        QStringLiteral(R"(&(?:#x[0-9A-Fa-f]+|#[0-9]+|amp|lt|gt|quot|apos);)"));
    auto matches = entity.globalMatch(text);
    while (matches.hasNext()) {
        const auto match = matches.next();
        result += text.mid(cursor, match.capturedStart() - cursor);
        const auto token = match.captured();
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
            const auto hexadecimal = token.startsWith(QStringLiteral("&#x"), Qt::CaseInsensitive);
            const auto digits =
                token.mid(hexadecimal ? 3 : 2, token.size() - (hexadecimal ? 4 : 3));
            const auto codePoint = digits.toUInt(&ok, hexadecimal ? 16 : 10);
            if (ok && codePoint <= 0x10FFFF && !(codePoint >= 0xD800 && codePoint <= 0xDFFF)) {
                const auto character = static_cast<char32_t>(codePoint);
                result += QString::fromUcs4(&character, 1);
            } else {
                result += token;
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
        if (remainder.startsWith("<![CDATA[")) {
            const auto relativeEnd = remainder.indexOf("]]>", 9);
            if (relativeEnd < 0) {
                break;
            }
            if (ignoredDepth == 0) {
                output += QString::fromUtf8(remainder.sliced(9, relativeEnd - 9));
            }
            cursor = opening + relativeEnd + 3;
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
    return loreforge::text::TextNormalizer::normalizeBlockLine(decodeEntities(output).simplified());
}

std::optional<document::BlockType> blockType(QStringView name) {
    if (name.size() == 2 && name.at(0) == QLatin1Char('h') && name.at(1) >= QLatin1Char('1') &&
        name.at(1) <= QLatin1Char('6')) {
        return document::BlockType::Heading;
    }
    static const QSet<QString> paragraphs = {QStringLiteral("p"),          QStringLiteral("li"),
                                             QStringLiteral("blockquote"), QStringLiteral("pre"),
                                             QStringLiteral("dt"),         QStringLiteral("dd")};
    if (paragraphs.contains(name.toString())) {
        return document::BlockType::Paragraph;
    }
    return std::nullopt;
}

std::variant<ExtractedResource, EpubParseError> extractXhtml(const QByteArray& bytes, QString path,
                                                             QString sourceId) {
    QStringDecoder decoder(QStringDecoder::Utf8, QStringConverter::Flag::Stateless);
    static_cast<void>(decoder.decode(bytes));
    if (decoder.hasError()) {
        return error(EpubParseErrorCode::InvalidXhtml,
                     QStringLiteral("An XHTML resource is not valid UTF-8: %1").arg(path));
    }
    QXmlStreamReader validator(bytes);
    while (!validator.atEnd()) {
        validator.readNext();
        if (validator.isDTD()) {
            return error(
                EpubParseErrorCode::InvalidXhtml,
                QStringLiteral("DTD declarations are not accepted in EPUB content: %1").arg(path));
        }
    }
    if (validator.hasError()) {
        return error(EpubParseErrorCode::InvalidXhtml,
                     QStringLiteral("An XHTML resource is malformed (%1): %2")
                         .arg(path, validator.errorString()));
    }

    ExtractedResource resource;
    resource.path = std::move(path);
    struct ActiveBlock final {
        document::BlockType type;
        QString name;
        QString anchor;
        qsizetype start = 0;
        qsizetype contentStart = 0;
        int sameNameDepth = 1;
    };
    std::optional<ActiveBlock> active;
    bool inBody = false;
    QString ignoredElement;
    int ignoredDepth = 0;

    qsizetype cursor = 0;
    while ((cursor = bytes.indexOf('<', cursor)) >= 0) {
        if (QByteArrayView(bytes).sliced(cursor).startsWith("<!--")) {
            const auto commentEnd = bytes.indexOf("-->", cursor + 4);
            cursor = commentEnd < 0 ? bytes.size() : commentEnd + 3;
            continue;
        }
        if (QByteArrayView(bytes).sliced(cursor).startsWith("<![CDATA[")) {
            const auto cdataEnd = bytes.indexOf("]]>", cursor + 9);
            cursor = cdataEnd < 0 ? bytes.size() : cdataEnd + 3;
            continue;
        }
        const auto end = tagEnd(bytes, cursor);
        if (end < 0) {
            break;
        }
        const QByteArrayView tag(bytes.constData() + cursor, end - cursor);
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
        if (!closing) {
            if (name == QStringLiteral("body")) {
                inBody = true;
                cursor = end;
                continue;
            }
            if (!inBody) {
                cursor = end;
                continue;
            }
            if (name == QStringLiteral("script") || name == QStringLiteral("style") ||
                name == QStringLiteral("svg")) {
                if (!selfClosing) {
                    ignoredElement = name;
                    ignoredDepth = 1;
                }
                cursor = end;
                continue;
            }
            auto anchor = attributeValue(tag, QStringLiteral("id"));
            if (anchor.isEmpty()) {
                anchor = attributeValue(tag, QStringLiteral("xml:id"));
            }
            if (!anchor.isEmpty() && !resource.anchors.contains(anchor)) {
                resource.anchors.insert(anchor, cursor);
            }
            if (active.has_value() && name == active->name && !selfClosing) {
                ++active->sameNameDepth;
            } else if (!active.has_value()) {
                if (name == QStringLiteral("hr")) {
                    resource.blocks.append(
                        {{document::BlockType::SceneBreak, QString{}, {sourceId, cursor, end}},
                         anchor});
                } else if (const auto type = blockType(name); type.has_value()) {
                    active = ActiveBlock{*type, name, anchor, cursor, end, 1};
                }
            }
        } else if (active.has_value() && name == active->name) {
            --active->sameNameDepth;
            if (active->sameNameDepth == 0) {
                const auto text = fragmentText(QByteArrayView(bytes).sliced(
                    active->contentStart, cursor - active->contentStart));
                if (!text.isEmpty()) {
                    resource.blocks.append(
                        {{active->type, text, {sourceId, active->start, end}}, active->anchor});
                }
                active.reset();
            }
        } else if (name == QStringLiteral("body")) {
            inBody = false;
        }
        cursor = end;
    }
    return resource;
}

QString fallbackTitle(const ExtractedResource& resource, QStringView bookTitle) {
    for (const auto& block : resource.blocks) {
        if (block.block.type == document::BlockType::Heading) {
            return block.block.text;
        }
    }
    const auto baseName = QFileInfo(resource.path).completeBaseName().trimmed();
    return baseName.isEmpty() ? bookTitle.toString() : baseName;
}

void appendChapter(document::Document& document, QString title, QList<document::Block> blocks) {
    const auto index = document.chapters.size();
    const auto chapterKey =
        document.id.toString() + QStringLiteral(":chapter:") + QString::number(index);
    document.chapters.append(
        {core::ChapterId::fromStableKey(chapterKey), index,
         title.trimmed().isEmpty() ? QStringLiteral("Untitled chapter") : title.trimmed(),
         std::move(blocks)});
}

std::optional<qint64> navigationOffset(const NavigationEntry& entry,
                                       const ExtractedResource& resource) {
    if (entry.fragment.isEmpty()) {
        return 0;
    }
    if (!resource.anchors.contains(entry.fragment)) {
        return std::nullopt;
    }
    const auto anchorOffset = resource.anchors.value(entry.fragment);
    for (const auto& block : resource.blocks) {
        if (block.block.sourceSpan.endByte > anchorOffset) {
            return block.block.sourceSpan.startByte;
        }
    }
    return anchorOffset;
}

std::optional<EpubParseError> mapChapters(document::Document& document,
                                          const QList<ExtractedResource>& resources,
                                          const QList<NavigationEntry>& navigation) {
    QList<document::Block> carriedBlocks;
    for (const auto& resource : resources) {
        QList<std::pair<qint64, NavigationEntry>> starts;
        for (const auto& entry : navigation) {
            if (entry.path != resource.path) {
                continue;
            }
            const auto offset = navigationOffset(entry, resource);
            if (!offset.has_value()) {
                return error(EpubParseErrorCode::InvalidNavigation,
                             QStringLiteral("A navigation fragment does not exist: %1#%2")
                                 .arg(entry.path, entry.fragment));
            }
            starts.append({*offset, entry});
        }
        std::stable_sort(starts.begin(), starts.end(), [](const auto& left, const auto& right) {
            return left.first < right.first;
        });

        if (navigation.isEmpty()) {
            QList<document::Block> blocks;
            for (const auto& extracted : resource.blocks) {
                blocks.append(extracted.block);
            }
            appendChapter(document, fallbackTitle(resource, document.metadata.title),
                          std::move(blocks));
            continue;
        }
        if (starts.isEmpty()) {
            if (document.chapters.isEmpty()) {
                for (const auto& extracted : resource.blocks) {
                    carriedBlocks.append(extracted.block);
                }
            } else {
                for (const auto& extracted : resource.blocks) {
                    document.chapters.last().blocks.append(extracted.block);
                }
            }
            continue;
        }

        for (qsizetype index = 0; index < starts.size(); ++index) {
            const auto lower = starts.at(index).first;
            const auto upper = index + 1 < starts.size() ? starts.at(index + 1).first
                                                         : std::numeric_limits<qint64>::max();
            QList<document::Block> blocks;
            if (index == 0 && !carriedBlocks.isEmpty()) {
                blocks = std::move(carriedBlocks);
                carriedBlocks.clear();
            }
            for (const auto& extracted : resource.blocks) {
                if (extracted.block.sourceSpan.startByte >= lower &&
                    extracted.block.sourceSpan.startByte < upper) {
                    blocks.append(extracted.block);
                }
            }
            appendChapter(document, starts.at(index).second.title, std::move(blocks));
        }
    }
    if (!carriedBlocks.isEmpty()) {
        appendChapter(document, document.metadata.title, std::move(carriedBlocks));
    }
    return std::nullopt;
}

} // namespace

EpubParseResult EpubParser::parse(QByteArrayView source, const EpubImportOptions& options) {
    const auto sourceLocator = options.sourceLocator.trimmed();
    if (sourceLocator.isEmpty()) {
        return error(EpubParseErrorCode::MissingSourceLocator,
                     QStringLiteral("A source locator is required for reproducible import."));
    }
    const auto archiveResult = readArchive(source);
    if (std::holds_alternative<EpubParseError>(archiveResult)) {
        return std::get<EpubParseError>(archiveResult);
    }
    const auto& archive = std::get<Archive>(archiveResult);
    const auto pathResult = packagePath(archive);
    if (std::holds_alternative<EpubParseError>(pathResult)) {
        return std::get<EpubParseError>(pathResult);
    }
    const auto packageResult = readPackage(archive, std::get<QString>(pathResult));
    if (std::holds_alternative<EpubParseError>(packageResult)) {
        return std::get<EpubParseError>(packageResult);
    }
    const auto& package = std::get<Package>(packageResult);
    const auto navigationResult = readNavigation(archive, package);
    if (std::holds_alternative<EpubParseError>(navigationResult)) {
        return std::get<EpubParseError>(navigationResult);
    }

    const auto bookTitle = !options.title.trimmed().isEmpty()
                               ? loreforge::text::TextNormalizer::normalizeBlockLine(options.title)
                               : loreforge::text::TextNormalizer::normalizeBlockLine(package.title);
    const auto title =
        bookTitle.isEmpty() ? QFileInfo(sourceLocator).completeBaseName() : bookTitle;
    const auto bookId = core::BookId::fromStableKey(sourceLocator);
    document::Document document{
        bookId,
        {title.isEmpty() ? QStringLiteral("Untitled") : title,
         options.authors.isEmpty() ? package.authors : options.authors,
         options.language.trimmed().isEmpty()
             ? (package.language.trimmed().isEmpty() ? QStringLiteral("und")
                                                     : package.language.trimmed())
             : options.language.trimmed(),
         QStringLiteral("epub"), sourceLocator, core::ContentHash::sha256(source)},
        {},
    };

    QList<ExtractedResource> resources;
    for (const auto& spineId : package.spine) {
        const auto item = package.manifest.value(spineId);
        if (item.mediaType != QStringLiteral("application/xhtml+xml") &&
            item.mediaType != QStringLiteral("text/html")) {
            return error(EpubParseErrorCode::MissingManifestItem,
                         QStringLiteral("A spine item is not an HTML resource: %1").arg(spineId));
        }
        const auto extracted = extractXhtml(archive.files.value(item.path), item.path,
                                            sourceLocator + QStringLiteral("!/") + item.path);
        if (std::holds_alternative<EpubParseError>(extracted)) {
            return std::get<EpubParseError>(extracted);
        }
        resources.append(std::get<ExtractedResource>(extracted));
    }
    if (const auto mappingError =
            mapChapters(document, resources, std::get<QList<NavigationEntry>>(navigationResult));
        mappingError.has_value()) {
        return *mappingError;
    }
    const auto hasContent = std::ranges::any_of(
        document.chapters, [](const auto& chapter) { return !chapter.blocks.isEmpty(); });
    if (!hasContent) {
        return error(EpubParseErrorCode::EmptyInput,
                     QStringLiteral("The EPUB contains no importable text."));
    }
    const auto validation = document::validateDocument(document);
    if (!validation.isValid()) {
        return error(EpubParseErrorCode::InvalidDocument,
                     QStringLiteral("The imported document failed domain validation: %1")
                         .arg(validation.errors.first().message));
    }
    return document;
}

EpubParseResult EpubParser::parseFile(QStringView filePath, EpubImportOptions options) {
    const QFileInfo fileInfo(filePath.toString());
    if (!fileInfo.exists() || !fileInfo.isFile()) {
        return error(EpubParseErrorCode::FileNotFound,
                     QStringLiteral("The EPUB file does not exist."));
    }
    QFile file(fileInfo.absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly)) {
        return error(EpubParseErrorCode::ReadFailure,
                     QStringLiteral("The EPUB file could not be read: %1").arg(file.errorString()));
    }
    if (options.sourceLocator.trimmed().isEmpty()) {
        options.sourceLocator = QDir::cleanPath(fileInfo.absoluteFilePath());
    }
    const auto source = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        return error(EpubParseErrorCode::ReadFailure,
                     QStringLiteral("The EPUB file could not be read completely: %1")
                         .arg(file.errorString()));
    }
    return parse(source, options);
}

} // namespace loreforge::parser
