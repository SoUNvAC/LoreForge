#include "loreforge/storage/book_repository.h"

#include "loreforge/document/document_validation.h"
#include "loreforge/storage/chapter_repository.h"
#include "loreforge/storage/project_database.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSqlError>
#include <QSqlQuery>

#include <utility>
#include <variant>

namespace loreforge::storage {
namespace {

StorageError queryError(QString message, const QSqlError& error) {
    const auto details = (error.databaseText() + QLatin1Char(' ') + error.driverText()).trimmed();
    const auto conflict =
        details.contains(QStringLiteral("UNIQUE constraint failed"), Qt::CaseInsensitive);
    return {conflict ? StorageErrorCode::Conflict : StorageErrorCode::SqlError, std::move(message),
            details, true};
}

QString encodeAuthors(const QStringList& authors) {
    QJsonArray values;
    for (const auto& author : authors) {
        values.append(author);
    }
    return QString::fromUtf8(QJsonDocument(values).toJson(QJsonDocument::Compact));
}

StorageResult<QStringList> decodeAuthors(const QString& encoded) {
    QJsonParseError parseError;
    const auto json = QJsonDocument::fromJson(encoded.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !json.isArray()) {
        return StorageError{StorageErrorCode::CorruptData,
                            QStringLiteral("Stored author metadata is invalid."),
                            parseError.errorString(), false};
    }

    QStringList authors;
    for (const auto& value : json.array()) {
        if (!value.isString()) {
            return StorageError{StorageErrorCode::CorruptData,
                                QStringLiteral("Stored author metadata is invalid."),
                                {},
                                false};
        }
        authors.append(value.toString());
    }
    return authors;
}

} // namespace

BookRepository::BookRepository(ProjectDatabase& database) : database_(database) {}

StorageStatus BookRepository::saveDocument(const core::ProjectId& projectId,
                                           const document::Document& document) {
    const auto validation = document::validateDocument(document);
    if (!projectId.isValid() || !validation.isValid()) {
        return StorageError{StorageErrorCode::InvalidDocument,
                            QStringLiteral("The document cannot be stored because it is invalid."),
                            validation.isValid() ? QStringLiteral("Project ID is invalid.")
                                                 : validation.errors.first().message,
                            true};
    }

    return database_.runInTransaction([this, &projectId, &document] {
        auto connection = database_.database();
        QSqlQuery query(connection);
        query.prepare(QStringLiteral(
            "INSERT INTO books(id, project_id, title, authors_json, language, source_format, "
            "source_locator, source_hash) VALUES(?, ?, ?, ?, ?, ?, ?, ?) "
            "ON CONFLICT(id) DO UPDATE SET project_id=excluded.project_id, title=excluded.title, "
            "authors_json=excluded.authors_json, language=excluded.language, "
            "source_format=excluded.source_format, source_locator=excluded.source_locator, "
            "source_hash=excluded.source_hash"));
        query.addBindValue(document.id.toString());
        query.addBindValue(projectId.toString());
        query.addBindValue(document.metadata.title);
        query.addBindValue(encodeAuthors(document.metadata.authors));
        query.addBindValue(document.metadata.language);
        query.addBindValue(document.metadata.sourceFormat);
        query.addBindValue(document.metadata.sourceLocator);
        query.addBindValue(document.metadata.sourceHash.toHex());
        if (!query.exec()) {
            return StorageStatus(queryError(
                QStringLiteral("The book metadata could not be stored."), query.lastError()));
        }

        ChapterRepository chapters(database_);
        return chapters.replaceInCurrentTransaction(document.id, document.chapters);
    });
}

StorageResult<document::Document> BookRepository::loadDocument(const core::BookId& bookId) const {
    auto connection = database_.database();
    QSqlQuery query(connection);
    query.prepare(QStringLiteral(
        "SELECT id, title, authors_json, language, source_format, source_locator, source_hash "
        "FROM books WHERE id = ?"));
    query.addBindValue(bookId.toString());
    if (!query.exec()) {
        return queryError(QStringLiteral("The book could not be read."), query.lastError());
    }
    if (!query.next()) {
        return StorageError{
            StorageErrorCode::NotFound, QStringLiteral("The book was not found."), {}, true};
    }

    const auto storedId = core::BookId::fromString(query.value(0).toString());
    auto authors = decodeAuthors(query.value(2).toString());
    const auto sourceHash = core::ContentHash::fromHex(query.value(6).toString());
    if (!storedId.has_value() || std::holds_alternative<StorageError>(authors) ||
        !sourceHash.has_value()) {
        if (std::holds_alternative<StorageError>(authors)) {
            return std::get<StorageError>(authors);
        }
        return StorageError{StorageErrorCode::CorruptData,
                            QStringLiteral("Stored book metadata is invalid."),
                            {},
                            false};
    }

    ChapterRepository chapterRepository(database_);
    auto chapters = chapterRepository.loadForBook(*storedId);
    if (std::holds_alternative<StorageError>(chapters)) {
        return std::get<StorageError>(chapters);
    }

    document::Document loaded{
        *storedId,
        {
            query.value(1).toString(),
            std::get<QStringList>(std::move(authors)),
            query.value(3).toString(),
            query.value(4).toString(),
            query.value(5).toString(),
            *sourceHash,
        },
        std::get<QList<document::Chapter>>(std::move(chapters)),
    };
    const auto validation = document::validateDocument(loaded);
    if (!validation.isValid()) {
        return StorageError{StorageErrorCode::CorruptData,
                            QStringLiteral("Stored document data failed domain validation."),
                            validation.errors.first().message, false};
    }
    return loaded;
}

StorageResult<QList<core::BookId>>
BookRepository::listBookIds(const core::ProjectId& projectId) const {
    auto connection = database_.database();
    QSqlQuery query(connection);
    query.prepare(QStringLiteral("SELECT id FROM books WHERE project_id = ? ORDER BY title, id"));
    query.addBindValue(projectId.toString());
    if (!query.exec()) {
        return queryError(QStringLiteral("Books could not be listed."), query.lastError());
    }

    QList<core::BookId> bookIds;
    while (query.next()) {
        const auto bookId = core::BookId::fromString(query.value(0).toString());
        if (!bookId.has_value()) {
            return StorageError{StorageErrorCode::CorruptData,
                                QStringLiteral("A stored book ID is invalid."),
                                {},
                                false};
        }
        bookIds.append(*bookId);
    }
    return bookIds;
}

} // namespace loreforge::storage
