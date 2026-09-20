#include "loreforge/storage/story_state_repository.h"

#include "loreforge/narrative/story_memory_json.h"
#include "loreforge/storage/project_database.h"

#include <QSqlError>
#include <QSqlQuery>

#include <limits>
#include <utility>
#include <variant>

namespace loreforge::storage {
namespace {

StorageError queryError(QString message, const QSqlError& error) {
    const auto details = (error.databaseText() + QLatin1Char(' ') + error.driverText()).trimmed();
    const auto conflict =
        details.contains(QStringLiteral("UNIQUE constraint failed"), Qt::CaseInsensitive) ||
        details.contains(QStringLiteral("FOREIGN KEY constraint failed"), Qt::CaseInsensitive);
    return {conflict ? StorageErrorCode::Conflict : StorageErrorCode::SqlError, std::move(message),
            details, true};
}

StorageError corrupt(QString message, QString details = {}) {
    return {StorageErrorCode::CorruptData, std::move(message), std::move(details), false};
}

StorageError invalid(QString message, QString details = {}) {
    return {StorageErrorCode::InvalidArgument, std::move(message), std::move(details), true};
}

QString storyErrors(const QList<narrative::StoryMemoryError>& errors) {
    QStringList messages;
    for (const auto& error : errors) {
        messages.append(error.path + QStringLiteral(": ") + error.message);
    }
    return messages.join(QLatin1Char('\n'));
}

} // namespace

StoryStateRepository::StoryStateRepository(ProjectDatabase& database) : database_(database) {}

StorageStatus
StoryStateRepository::saveChapterRecord(const narrative::ChapterMemoryRecord& record) {
    const auto validation = narrative::StoryStateRebuilder::validateRecord(record);
    if (!validation.isEmpty()) {
        return invalid(QStringLiteral("The chapter-memory record is invalid."),
                       storyErrors(validation));
    }
    const auto json = narrative::encodeChapterAnalysis(record.analysis);
    const auto hash = core::ContentHash::sha256(json);
    return database_.runInTransaction([this, &record, &json, &hash]() -> StorageStatus {
        auto connection = database_.database();
        QSqlQuery ownership(connection);
        ownership.prepare(
            QStringLiteral("SELECT 1 FROM chapters c JOIN books b ON b.id = c.book_id "
                           "WHERE c.id = ? AND b.project_id = ?"));
        ownership.addBindValue(record.analysis.chapterId.toString());
        ownership.addBindValue(record.projectId.toString());
        if (!ownership.exec()) {
            return queryError(QStringLiteral("Chapter ownership could not be verified."),
                              ownership.lastError());
        }
        if (!ownership.next()) {
            return invalid(QStringLiteral("The analyzed chapter does not belong to the project."));
        }

        QSqlQuery invalidate(connection);
        invalidate.prepare(QStringLiteral("DELETE FROM story_state_snapshots WHERE project_id = ? "
                                          "AND through_chapter_sequence >= ?"));
        invalidate.addBindValue(record.projectId.toString());
        invalidate.addBindValue(static_cast<qlonglong>(record.chapterSequence));
        if (!invalidate.exec()) {
            return queryError(
                QStringLiteral("Dependent story-state snapshots could not be invalidated."),
                invalidate.lastError());
        }

        QSqlQuery query(connection);
        query.prepare(QStringLiteral(
            "INSERT INTO chapter_memory_records(project_id, chapter_sequence, chapter_id, "
            "analysis_json, analysis_hash) VALUES(?, ?, ?, ?, ?) "
            "ON CONFLICT(project_id, chapter_sequence) DO UPDATE SET "
            "chapter_id = excluded.chapter_id, analysis_json = excluded.analysis_json, "
            "analysis_hash = excluded.analysis_hash"));
        query.addBindValue(record.projectId.toString());
        query.addBindValue(static_cast<qlonglong>(record.chapterSequence));
        query.addBindValue(record.analysis.chapterId.toString());
        query.addBindValue(QString::fromUtf8(json));
        query.addBindValue(hash.toHex());
        if (!query.exec()) {
            return queryError(QStringLiteral("The chapter-memory record could not be saved."),
                              query.lastError());
        }
        return std::nullopt;
    });
}

StorageResult<QList<narrative::ChapterMemoryRecord>>
StoryStateRepository::loadChapterRecords(const core::ProjectId& projectId,
                                         qsizetype throughChapterSequence) const {
    if (!projectId.isValid() || throughChapterSequence < 0) {
        return invalid(QStringLiteral("A valid project and ending chapter sequence are required."));
    }
    auto connection = database_.database();
    QSqlQuery query(connection);
    query.prepare(
        QStringLiteral("SELECT chapter_sequence, chapter_id, analysis_json, analysis_hash "
                       "FROM chapter_memory_records WHERE project_id = ? AND chapter_sequence <= ? "
                       "ORDER BY chapter_sequence"));
    query.addBindValue(projectId.toString());
    query.addBindValue(static_cast<qlonglong>(throughChapterSequence));
    if (!query.exec()) {
        return queryError(QStringLiteral("Chapter-memory records could not be read."),
                          query.lastError());
    }

    QList<narrative::ChapterMemoryRecord> records;
    while (query.next()) {
        bool sequenceOk = false;
        const auto sequence = query.value(0).toLongLong(&sequenceOk);
        const auto chapterId = core::ChapterId::fromString(query.value(1).toString());
        const auto json = query.value(2).toString().toUtf8();
        const auto storedHash = core::ContentHash::fromHex(query.value(3).toString());
        const auto decoded = narrative::decodeChapterAnalysis(json);
        if (!sequenceOk || sequence < 0 ||
            sequence > static_cast<qlonglong>(std::numeric_limits<qsizetype>::max()) ||
            !chapterId.has_value() || !storedHash.has_value() ||
            *storedHash != core::ContentHash::sha256(json) || !decoded.isValid() ||
            decoded.analysis->chapterId != *chapterId) {
            return corrupt(QStringLiteral("Stored chapter-memory data is invalid."), decoded.error);
        }
        records.append({projectId, static_cast<qsizetype>(sequence), *decoded.analysis});
    }
    return records;
}

StorageResult<narrative::StoryStateSnapshot>
StoryStateRepository::rebuildAndSave(const core::ProjectId& projectId,
                                     qsizetype throughChapterSequence) {
    const auto loaded = loadChapterRecords(projectId, throughChapterSequence);
    if (std::holds_alternative<StorageError>(loaded)) {
        return std::get<StorageError>(loaded);
    }
    const auto rebuilt = narrative::StoryStateRebuilder::rebuild(
        projectId, std::get<QList<narrative::ChapterMemoryRecord>>(loaded));
    if (!rebuilt.isValid() || rebuilt.snapshot->throughChapterSequence != throughChapterSequence) {
        return invalid(QStringLiteral("Story state could not be rebuilt from Chapter 1..N."),
                       storyErrors(rebuilt.errors));
    }
    const auto snapshot = *rebuilt.snapshot;
    const auto json = narrative::encodeStoryStateSnapshot(snapshot);
    const auto status = database_.runInTransaction([this, &snapshot, &json]() -> StorageStatus {
        auto connection = database_.database();
        QSqlQuery query(connection);
        query.prepare(QStringLiteral(
            "INSERT INTO story_state_snapshots(id, project_id, through_chapter_sequence, "
            "source_hash, state_hash, state_json) VALUES(?, ?, ?, ?, ?, ?) "
            "ON CONFLICT(project_id, through_chapter_sequence) DO UPDATE SET "
            "id = excluded.id, source_hash = excluded.source_hash, "
            "state_hash = excluded.state_hash, state_json = excluded.state_json"));
        query.addBindValue(snapshot.id.toString());
        query.addBindValue(snapshot.projectId.toString());
        query.addBindValue(static_cast<qlonglong>(snapshot.throughChapterSequence));
        query.addBindValue(snapshot.sourceHash.toHex());
        query.addBindValue(snapshot.stateHash.toHex());
        query.addBindValue(QString::fromUtf8(json));
        if (!query.exec()) {
            return queryError(QStringLiteral("The story-state snapshot could not be saved."),
                              query.lastError());
        }
        return std::nullopt;
    });
    if (status.has_value()) {
        return *status;
    }
    return snapshot;
}

StorageResult<narrative::StoryStateSnapshot>
StoryStateRepository::loadSnapshot(const core::ProjectId& projectId,
                                   qsizetype throughChapterSequence) const {
    if (!projectId.isValid() || throughChapterSequence < 0) {
        return invalid(QStringLiteral("A valid project and ending chapter sequence are required."));
    }
    auto connection = database_.database();
    QSqlQuery query(connection);
    query.prepare(
        QStringLiteral("SELECT id, source_hash, state_hash, state_json FROM story_state_snapshots "
                       "WHERE project_id = ? AND through_chapter_sequence = ?"));
    query.addBindValue(projectId.toString());
    query.addBindValue(static_cast<qlonglong>(throughChapterSequence));
    if (!query.exec()) {
        return queryError(QStringLiteral("The story-state snapshot could not be read."),
                          query.lastError());
    }
    if (!query.next()) {
        return StorageError{StorageErrorCode::NotFound,
                            QStringLiteral("The story-state snapshot was not found."),
                            {},
                            true};
    }
    const auto id = core::StoryStateSnapshotId::fromString(query.value(0).toString());
    const auto sourceHash = core::ContentHash::fromHex(query.value(1).toString());
    const auto stateHash = core::ContentHash::fromHex(query.value(2).toString());
    const auto storedJson = query.value(3).toString().toUtf8();
    if (!id.has_value() || !sourceHash.has_value() || !stateHash.has_value()) {
        return corrupt(QStringLiteral("Stored story-state metadata is invalid."));
    }

    const auto loaded = loadChapterRecords(projectId, throughChapterSequence);
    if (std::holds_alternative<StorageError>(loaded)) {
        return std::get<StorageError>(loaded);
    }
    const auto rebuilt = narrative::StoryStateRebuilder::rebuild(
        projectId, std::get<QList<narrative::ChapterMemoryRecord>>(loaded));
    if (!rebuilt.isValid()) {
        return corrupt(QStringLiteral("Stored story state cannot be rebuilt."),
                       storyErrors(rebuilt.errors));
    }
    const auto& snapshot = *rebuilt.snapshot;
    if (snapshot.id != *id || snapshot.sourceHash != *sourceHash ||
        snapshot.stateHash != *stateHash ||
        narrative::encodeStoryStateSnapshot(snapshot) != storedJson) {
        return corrupt(QStringLiteral("The stored story-state snapshot is stale or corrupted."));
    }
    return snapshot;
}

} // namespace loreforge::storage
