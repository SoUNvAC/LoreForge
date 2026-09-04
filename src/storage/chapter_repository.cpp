#include "loreforge/storage/chapter_repository.h"

#include "loreforge/core/content_hash.h"
#include "loreforge/document/document_validation.h"
#include "loreforge/storage/project_database.h"

#include <QSet>
#include <QSqlError>
#include <QSqlQuery>

#include <cmath>
#include <utility>
#include <variant>

namespace loreforge::storage {
namespace {

StorageError queryError(QString message, const QSqlError& error) {
    return {StorageErrorCode::SqlError, std::move(message),
            (error.databaseText() + QLatin1Char(' ') + error.driverText()).trimmed(), true};
}

StorageStatus validateChapters(const core::BookId& bookId,
                               const QList<document::Chapter>& chapters) {
    const document::Document validationDocument{
        bookId,
        {
            QStringLiteral("Storage validation"),
            {},
            QStringLiteral("und"),
            QStringLiteral("txt"),
            QStringLiteral("storage-validation"),
            core::ContentHash::sha256(QByteArrayView{}),
        },
        chapters,
    };
    const auto validation = document::validateDocument(validationDocument);
    if (!validation.isValid()) {
        return StorageError{StorageErrorCode::InvalidDocument,
                            QStringLiteral("The chapters failed domain validation."),
                            validation.errors.first().message, true};
    }
    return std::nullopt;
}

StorageResult<QList<document::Block>> loadBlocks(QSqlDatabase& connection,
                                                 const core::ChapterId& chapterId) {
    QSqlQuery query(connection);
    query.prepare(QStringLiteral(
        "SELECT block_index, block_type, text, source_id, start_byte, end_byte, "
        "extraction_confidence FROM blocks WHERE chapter_id = ? ORDER BY block_index"));
    query.addBindValue(chapterId.toString());
    if (!query.exec()) {
        return queryError(QStringLiteral("Chapter blocks could not be read."), query.lastError());
    }

    QList<document::Block> blocks;
    while (query.next()) {
        bool indexOk = false;
        const auto blockIndex = query.value(0).toLongLong(&indexOk);
        const auto blockType = document::blockTypeFromString(query.value(1).toString());
        bool startOk = false;
        bool endOk = false;
        const auto startByte = query.value(4).toLongLong(&startOk);
        const auto endByte = query.value(5).toLongLong(&endOk);
        const core::SourceSpan span{query.value(3).toString(), startByte, endByte};
        std::optional<double> extractionConfidence;
        bool confidenceOk = true;
        if (!query.value(6).isNull()) {
            extractionConfidence = query.value(6).toDouble(&confidenceOk);
            confidenceOk = confidenceOk && std::isfinite(*extractionConfidence) &&
                           *extractionConfidence >= 0.0 && *extractionConfidence <= 1.0;
        }
        if (!indexOk || blockIndex != blocks.size() || !blockType.has_value() || !startOk ||
            !endOk || !span.isValid() || !confidenceOk) {
            return StorageError{StorageErrorCode::CorruptData,
                                QStringLiteral("Stored chapter block data is invalid."),
                                {},
                                false};
        }
        blocks.append({*blockType, query.value(2).toString(), span, extractionConfidence});
    }
    return blocks;
}

} // namespace

ChapterRepository::ChapterRepository(ProjectDatabase& database) : database_(database) {}

StorageStatus ChapterRepository::replaceForBook(const core::BookId& bookId,
                                                const QList<document::Chapter>& chapters) {
    if (const auto validation = validateChapters(bookId, chapters); validation.has_value()) {
        return validation;
    }
    return database_.runInTransaction(
        [this, &bookId, &chapters] { return replaceInCurrentTransaction(bookId, chapters); });
}

StorageStatus
ChapterRepository::replaceInCurrentTransaction(const core::BookId& bookId,
                                               const QList<document::Chapter>& chapters) {
    auto connection = database_.database();
    QSqlQuery deleteChapters(connection);
    deleteChapters.prepare(QStringLiteral("DELETE FROM chapters WHERE book_id = ?"));
    deleteChapters.addBindValue(bookId.toString());
    if (!deleteChapters.exec()) {
        return queryError(QStringLiteral("Existing chapters could not be replaced."),
                          deleteChapters.lastError());
    }

    QSqlQuery insertChapter(connection);
    insertChapter.prepare(QStringLiteral(
        "INSERT INTO chapters(id, book_id, chapter_index, title) VALUES(?, ?, ?, ?)"));
    QSqlQuery insertBlock(connection);
    insertBlock.prepare(QStringLiteral(
        "INSERT INTO blocks(chapter_id, block_index, block_type, text, source_id, start_byte, "
        "end_byte, extraction_confidence) VALUES(?, ?, ?, ?, ?, ?, ?, ?)"));

    for (const auto& chapter : chapters) {
        insertChapter.bindValue(0, chapter.id.toString());
        insertChapter.bindValue(1, bookId.toString());
        insertChapter.bindValue(2, static_cast<qlonglong>(chapter.index));
        insertChapter.bindValue(3, chapter.title);
        if (!insertChapter.exec()) {
            return queryError(QStringLiteral("A chapter could not be stored."),
                              insertChapter.lastError());
        }

        for (qsizetype blockIndex = 0; blockIndex < chapter.blocks.size(); ++blockIndex) {
            const auto& block = chapter.blocks.at(blockIndex);
            insertBlock.bindValue(0, chapter.id.toString());
            insertBlock.bindValue(1, static_cast<qlonglong>(blockIndex));
            insertBlock.bindValue(2, document::blockTypeToString(block.type));
            insertBlock.bindValue(3, block.text);
            insertBlock.bindValue(4, block.sourceSpan.sourceId);
            insertBlock.bindValue(5, block.sourceSpan.startByte);
            insertBlock.bindValue(6, block.sourceSpan.endByte);
            insertBlock.bindValue(7, block.extractionConfidence.has_value()
                                         ? QVariant(*block.extractionConfidence)
                                         : QVariant());
            if (!insertBlock.exec()) {
                return queryError(QStringLiteral("A chapter block could not be stored."),
                                  insertBlock.lastError());
            }
        }
    }
    return std::nullopt;
}

StorageResult<QList<document::Chapter>>
ChapterRepository::loadForBook(const core::BookId& bookId) const {
    auto connection = database_.database();
    QSqlQuery query(connection);
    query.prepare(QStringLiteral(
        "SELECT id, chapter_index, title FROM chapters WHERE book_id = ? ORDER BY chapter_index"));
    query.addBindValue(bookId.toString());
    if (!query.exec()) {
        return queryError(QStringLiteral("Chapters could not be read."), query.lastError());
    }

    QList<document::Chapter> chapters;
    while (query.next()) {
        const auto chapterId = core::ChapterId::fromString(query.value(0).toString());
        bool indexOk = false;
        const auto index = query.value(1).toLongLong(&indexOk);
        if (!chapterId.has_value() || !indexOk || index != chapters.size()) {
            return StorageError{StorageErrorCode::CorruptData,
                                QStringLiteral("Stored chapter metadata is invalid."),
                                {},
                                false};
        }

        auto blocks = loadBlocks(connection, *chapterId);
        if (std::holds_alternative<StorageError>(blocks)) {
            return std::get<StorageError>(blocks);
        }
        chapters.append({*chapterId, static_cast<qsizetype>(index), query.value(2).toString(),
                         std::get<QList<document::Block>>(std::move(blocks))});
    }

    if (const auto validation = validateChapters(bookId, chapters); validation.has_value()) {
        return StorageError{StorageErrorCode::CorruptData,
                            QStringLiteral("Stored chapters failed domain validation."),
                            validation->technicalDetails, false};
    }
    return chapters;
}

} // namespace loreforge::storage
