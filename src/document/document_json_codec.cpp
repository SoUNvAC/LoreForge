#include "loreforge/document/document_json_codec.h"

#include "loreforge/document/document_hash.h"
#include "loreforge/document/document_validation.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

#include <cmath>
#include <limits>
#include <optional>
#include <utility>

namespace loreforge::document {
namespace {

constexpr qint64 maximumSafeJsonInteger = 9'007'199'254'740'991;

DocumentJsonError error(DocumentJsonErrorCode code, QString path, QString message) {
    return {code, std::move(path), std::move(message)};
}

std::optional<QString> readString(const QJsonObject& object, QStringView key, QStringView path,
                                  DocumentJsonError& outputError) {
    const auto keyString = key.toString();
    if (!object.contains(keyString)) {
        outputError = error(DocumentJsonErrorCode::MissingField, path.toString(),
                            QStringLiteral("Required field is missing."));
        return std::nullopt;
    }

    const auto value = object.value(keyString);
    if (!value.isString()) {
        outputError = error(DocumentJsonErrorCode::WrongType, path.toString(),
                            QStringLiteral("Expected a string."));
        return std::nullopt;
    }
    return value.toString();
}

std::optional<QJsonObject> readObject(const QJsonObject& object, QStringView key, QStringView path,
                                      DocumentJsonError& outputError) {
    const auto keyString = key.toString();
    if (!object.contains(keyString)) {
        outputError = error(DocumentJsonErrorCode::MissingField, path.toString(),
                            QStringLiteral("Required field is missing."));
        return std::nullopt;
    }

    const auto value = object.value(keyString);
    if (!value.isObject()) {
        outputError = error(DocumentJsonErrorCode::WrongType, path.toString(),
                            QStringLiteral("Expected an object."));
        return std::nullopt;
    }
    return value.toObject();
}

std::optional<QJsonArray> readArray(const QJsonObject& object, QStringView key, QStringView path,
                                    DocumentJsonError& outputError) {
    const auto keyString = key.toString();
    if (!object.contains(keyString)) {
        outputError = error(DocumentJsonErrorCode::MissingField, path.toString(),
                            QStringLiteral("Required field is missing."));
        return std::nullopt;
    }

    const auto value = object.value(keyString);
    if (!value.isArray()) {
        outputError = error(DocumentJsonErrorCode::WrongType, path.toString(),
                            QStringLiteral("Expected an array."));
        return std::nullopt;
    }
    return value.toArray();
}

std::optional<qint64> readInteger(const QJsonObject& object, QStringView key, QStringView path,
                                  DocumentJsonError& outputError) {
    const auto keyString = key.toString();
    if (!object.contains(keyString)) {
        outputError = error(DocumentJsonErrorCode::MissingField, path.toString(),
                            QStringLiteral("Required field is missing."));
        return std::nullopt;
    }

    const auto value = object.value(keyString);
    if (!value.isDouble()) {
        outputError = error(DocumentJsonErrorCode::WrongType, path.toString(),
                            QStringLiteral("Expected an integer."));
        return std::nullopt;
    }

    const auto numeric = value.toDouble();
    if (!std::isfinite(numeric) || std::floor(numeric) != numeric || numeric < 0 ||
        numeric > static_cast<double>(maximumSafeJsonInteger)) {
        outputError = error(DocumentJsonErrorCode::InvalidValue, path.toString(),
                            QStringLiteral("Expected a non-negative JSON-safe integer."));
        return std::nullopt;
    }
    return static_cast<qint64>(numeric);
}

QJsonObject sourceSpanToJson(const core::SourceSpan& span) {
    return {
        {QStringLiteral("end_byte"), span.endByte},
        {QStringLiteral("source_id"), span.sourceId},
        {QStringLiteral("start_byte"), span.startByte},
    };
}

QJsonObject blockToJson(const Block& block) {
    return {
        {QStringLiteral("source_span"), sourceSpanToJson(block.sourceSpan)},
        {QStringLiteral("text"), block.text},
        {QStringLiteral("type"), blockTypeToString(block.type)},
    };
}

QJsonObject chapterToJson(const Chapter& chapter) {
    QJsonArray blocks;
    for (const auto& block : chapter.blocks) {
        blocks.append(blockToJson(block));
    }

    return {
        {QStringLiteral("blocks"), blocks},
        {QStringLiteral("id"), chapter.id.toString()},
        {QStringLiteral("index"), chapter.index},
        {QStringLiteral("title"), chapter.title},
    };
}

QJsonObject metadataToJson(const BookMetadata& metadata) {
    QJsonArray authors;
    for (const auto& author : metadata.authors) {
        authors.append(author);
    }

    return {
        {QStringLiteral("authors"), authors},
        {QStringLiteral("language"), metadata.language},
        {QStringLiteral("source_format"), metadata.sourceFormat},
        {QStringLiteral("source_hash"), metadata.sourceHash.toHex()},
        {QStringLiteral("source_locator"), metadata.sourceLocator},
        {QStringLiteral("title"), metadata.title},
    };
}

std::optional<core::SourceSpan> parseSourceSpan(const QJsonObject& object, const QString& path,
                                                DocumentJsonError& outputError) {
    const auto sourceId = readString(object, QStringLiteral("source_id"),
                                     path + QStringLiteral(".source_id"), outputError);
    if (!sourceId) {
        return std::nullopt;
    }
    const auto startByte = readInteger(object, QStringLiteral("start_byte"),
                                       path + QStringLiteral(".start_byte"), outputError);
    if (!startByte) {
        return std::nullopt;
    }
    const auto endByte = readInteger(object, QStringLiteral("end_byte"),
                                     path + QStringLiteral(".end_byte"), outputError);
    if (!endByte) {
        return std::nullopt;
    }

    return core::SourceSpan{*sourceId, *startByte, *endByte};
}

std::optional<Block> parseBlock(const QJsonObject& object, const QString& path,
                                DocumentJsonError& outputError) {
    const auto typeText =
        readString(object, QStringLiteral("type"), path + QStringLiteral(".type"), outputError);
    if (!typeText) {
        return std::nullopt;
    }
    const auto type = blockTypeFromString(*typeText);
    if (!type) {
        outputError = error(DocumentJsonErrorCode::InvalidValue, path + QStringLiteral(".type"),
                            QStringLiteral("Unknown block type."));
        return std::nullopt;
    }

    const auto text =
        readString(object, QStringLiteral("text"), path + QStringLiteral(".text"), outputError);
    if (!text) {
        return std::nullopt;
    }
    const auto spanObject = readObject(object, QStringLiteral("source_span"),
                                       path + QStringLiteral(".source_span"), outputError);
    if (!spanObject) {
        return std::nullopt;
    }
    const auto span =
        parseSourceSpan(*spanObject, path + QStringLiteral(".source_span"), outputError);
    if (!span) {
        return std::nullopt;
    }

    return Block{*type, *text, *span};
}

std::optional<Chapter> parseChapter(const QJsonObject& object, const QString& path,
                                    DocumentJsonError& outputError) {
    const auto idText =
        readString(object, QStringLiteral("id"), path + QStringLiteral(".id"), outputError);
    if (!idText) {
        return std::nullopt;
    }
    const auto id = core::ChapterId::fromString(*idText);
    if (!id) {
        outputError = error(DocumentJsonErrorCode::InvalidValue, path + QStringLiteral(".id"),
                            QStringLiteral("Invalid chapter ID."));
        return std::nullopt;
    }

    const auto index =
        readInteger(object, QStringLiteral("index"), path + QStringLiteral(".index"), outputError);
    if (!index || *index > std::numeric_limits<qsizetype>::max()) {
        if (index) {
            outputError =
                error(DocumentJsonErrorCode::InvalidValue, path + QStringLiteral(".index"),
                      QStringLiteral("Chapter index is too large."));
        }
        return std::nullopt;
    }
    const auto title =
        readString(object, QStringLiteral("title"), path + QStringLiteral(".title"), outputError);
    if (!title) {
        return std::nullopt;
    }
    const auto blocksJson =
        readArray(object, QStringLiteral("blocks"), path + QStringLiteral(".blocks"), outputError);
    if (!blocksJson) {
        return std::nullopt;
    }

    QList<Block> blocks;
    blocks.reserve(blocksJson->size());
    for (qsizetype position = 0; position < blocksJson->size(); ++position) {
        const auto blockPath = path + QStringLiteral(".blocks[%1]").arg(position);
        const auto value = blocksJson->at(position);
        if (!value.isObject()) {
            outputError = error(DocumentJsonErrorCode::WrongType, blockPath,
                                QStringLiteral("Expected a block object."));
            return std::nullopt;
        }
        const auto block = parseBlock(value.toObject(), blockPath, outputError);
        if (!block) {
            return std::nullopt;
        }
        blocks.append(*block);
    }

    return Chapter{*id, static_cast<qsizetype>(*index), *title, std::move(blocks)};
}

std::optional<BookMetadata> parseMetadata(const QJsonObject& object, const QString& path,
                                          DocumentJsonError& outputError) {
    const auto title =
        readString(object, QStringLiteral("title"), path + QStringLiteral(".title"), outputError);
    if (!title) {
        return std::nullopt;
    }
    const auto authorsJson = readArray(object, QStringLiteral("authors"),
                                       path + QStringLiteral(".authors"), outputError);
    if (!authorsJson) {
        return std::nullopt;
    }

    QStringList authors;
    authors.reserve(authorsJson->size());
    for (qsizetype position = 0; position < authorsJson->size(); ++position) {
        if (!authorsJson->at(position).isString()) {
            outputError = error(DocumentJsonErrorCode::WrongType,
                                path + QStringLiteral(".authors[%1]").arg(position),
                                QStringLiteral("Expected an author string."));
            return std::nullopt;
        }
        authors.append(authorsJson->at(position).toString());
    }

    const auto language = readString(object, QStringLiteral("language"),
                                     path + QStringLiteral(".language"), outputError);
    if (!language) {
        return std::nullopt;
    }
    const auto sourceFormat = readString(object, QStringLiteral("source_format"),
                                         path + QStringLiteral(".source_format"), outputError);
    if (!sourceFormat) {
        return std::nullopt;
    }
    const auto sourceLocator = readString(object, QStringLiteral("source_locator"),
                                          path + QStringLiteral(".source_locator"), outputError);
    if (!sourceLocator) {
        return std::nullopt;
    }
    const auto sourceHashText = readString(object, QStringLiteral("source_hash"),
                                           path + QStringLiteral(".source_hash"), outputError);
    if (!sourceHashText) {
        return std::nullopt;
    }
    const auto sourceHash = core::ContentHash::fromHex(*sourceHashText);
    if (!sourceHash) {
        outputError =
            error(DocumentJsonErrorCode::InvalidValue, path + QStringLiteral(".source_hash"),
                  QStringLiteral("Invalid SHA-256 source hash."));
        return std::nullopt;
    }

    return BookMetadata{*title, authors, *language, *sourceFormat, *sourceLocator, *sourceHash};
}

} // namespace

DocumentEncodeResult DocumentJsonCodec::encode(const Document& document) {
    const auto validation = validateDocument(document);
    if (!validation.isValid()) {
        const auto& first = validation.errors.first();
        return error(DocumentJsonErrorCode::InvalidDocument, first.path, first.message);
    }

    QJsonArray chapters;
    for (const auto& chapter : document.chapters) {
        chapters.append(chapterToJson(chapter));
    }

    const QJsonObject documentObject{
        {QStringLiteral("chapters"), chapters},
        {QStringLiteral("content_hash"), computeContentHash(document).toHex()},
        {QStringLiteral("id"), document.id.toString()},
        {QStringLiteral("metadata"), metadataToJson(document.metadata)},
    };
    const QJsonObject root{
        {QStringLiteral("document"), documentObject},
        {QStringLiteral("schema_version"), schemaVersion},
    };
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

DocumentDecodeResult DocumentJsonCodec::decode(QByteArrayView json) {
    QJsonParseError parseError;
    const auto jsonDocument = QJsonDocument::fromJson(json.toByteArray(), &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        return error(DocumentJsonErrorCode::InvalidJson, QStringLiteral("$"),
                     parseError.errorString());
    }
    if (!jsonDocument.isObject()) {
        return error(DocumentJsonErrorCode::InvalidJson, QStringLiteral("$"),
                     QStringLiteral("Document root must be a JSON object."));
    }

    const auto root = jsonDocument.object();
    DocumentJsonError decodeError;
    const auto version = readInteger(root, QStringLiteral("schema_version"),
                                     QStringLiteral("$.schema_version"), decodeError);
    if (!version) {
        return decodeError;
    }
    if (*version != schemaVersion) {
        return error(DocumentJsonErrorCode::UnsupportedSchema, QStringLiteral("$.schema_version"),
                     QStringLiteral("Unsupported document schema version."));
    }

    const auto documentJson =
        readObject(root, QStringLiteral("document"), QStringLiteral("$.document"), decodeError);
    if (!documentJson) {
        return decodeError;
    }
    const auto idText = readString(*documentJson, QStringLiteral("id"),
                                   QStringLiteral("$.document.id"), decodeError);
    if (!idText) {
        return decodeError;
    }
    const auto id = core::BookId::fromString(*idText);
    if (!id) {
        return error(DocumentJsonErrorCode::InvalidValue, QStringLiteral("$.document.id"),
                     QStringLiteral("Invalid book ID."));
    }

    const auto expectedHashText =
        readString(*documentJson, QStringLiteral("content_hash"),
                   QStringLiteral("$.document.content_hash"), decodeError);
    if (!expectedHashText) {
        return decodeError;
    }
    const auto expectedHash = core::ContentHash::fromHex(*expectedHashText);
    if (!expectedHash) {
        return error(DocumentJsonErrorCode::InvalidValue, QStringLiteral("$.document.content_hash"),
                     QStringLiteral("Invalid SHA-256 content hash."));
    }

    const auto metadataJson = readObject(*documentJson, QStringLiteral("metadata"),
                                         QStringLiteral("$.document.metadata"), decodeError);
    if (!metadataJson) {
        return decodeError;
    }
    const auto metadata =
        parseMetadata(*metadataJson, QStringLiteral("$.document.metadata"), decodeError);
    if (!metadata) {
        return decodeError;
    }

    const auto chaptersJson = readArray(*documentJson, QStringLiteral("chapters"),
                                        QStringLiteral("$.document.chapters"), decodeError);
    if (!chaptersJson) {
        return decodeError;
    }
    QList<Chapter> chapters;
    chapters.reserve(chaptersJson->size());
    for (qsizetype position = 0; position < chaptersJson->size(); ++position) {
        const auto chapterPath = QStringLiteral("$.document.chapters[%1]").arg(position);
        const auto value = chaptersJson->at(position);
        if (!value.isObject()) {
            return error(DocumentJsonErrorCode::WrongType, chapterPath,
                         QStringLiteral("Expected a chapter object."));
        }
        const auto chapter = parseChapter(value.toObject(), chapterPath, decodeError);
        if (!chapter) {
            return decodeError;
        }
        chapters.append(*chapter);
    }

    Document document{*id, *metadata, std::move(chapters)};
    const auto validation = validateDocument(document);
    if (!validation.isValid()) {
        const auto& first = validation.errors.first();
        return error(DocumentJsonErrorCode::InvalidDocument, first.path, first.message);
    }
    if (computeContentHash(document) != *expectedHash) {
        return error(DocumentJsonErrorCode::ContentHashMismatch,
                     QStringLiteral("$.document.content_hash"),
                     QStringLiteral("Document content does not match its stored hash."));
    }

    return document;
}

} // namespace loreforge::document
