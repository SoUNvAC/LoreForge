#include "loreforge/storage/repair_queue_repository.h"

#include "loreforge/storage/project_database.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QSqlError>
#include <QSqlQuery>

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

StorageError invalid(QString message) {
    return {StorageErrorCode::InvalidArgument, std::move(message), {}, true};
}

StorageError corrupt(QString message) {
    return {StorageErrorCode::CorruptData, std::move(message), {}, false};
}

QString originName(proofreading::CandidateOrigin origin) {
    return origin == proofreading::CandidateOrigin::Deterministic ? QStringLiteral("DETERMINISTIC")
                                                                  : QStringLiteral("SEMANTIC");
}

std::optional<proofreading::CandidateOrigin> originFromName(QStringView name) {
    if (name == QStringLiteral("DETERMINISTIC")) {
        return proofreading::CandidateOrigin::Deterministic;
    }
    if (name == QStringLiteral("SEMANTIC")) {
        return proofreading::CandidateOrigin::Semantic;
    }
    return std::nullopt;
}

StorageResult<proofreading::RepairQueueItem> itemFromQuery(const QSqlQuery& query) {
    const auto id = core::ProofreadingCandidateId::fromString(query.value(0).toString());
    const auto projectId = core::ProjectId::fromString(query.value(1).toString());
    const auto chapterId = core::ChapterId::fromString(query.value(2).toString());
    const core::SourceSpan span{query.value(3).toString(), query.value(4).toLongLong(),
                                query.value(5).toLongLong()};
    const auto category = proofreading::candidateCategoryFromName(query.value(9).toString());
    const auto impact = proofreading::semanticImpactFromName(query.value(13).toString());
    const auto origin = originFromName(query.value(14).toString());
    const auto status = proofreading::candidateStatusFromName(query.value(15).toString());
    const auto hash = core::ContentHash::fromHex(query.value(17).toString());
    const auto createdAt = QDateTime::fromString(query.value(18).toString(), Qt::ISODateWithMs);
    const auto updatedAt = QDateTime::fromString(query.value(19).toString(), Qt::ISODateWithMs);
    if (!id || !projectId || !chapterId || !span.isValid() || !category || !impact || !origin ||
        !status || !hash || !createdAt.isValid() || !updatedAt.isValid()) {
        return corrupt(QStringLiteral("Stored repair-queue data is invalid."));
    }
    const proofreading::ProofreadingCandidate candidate{
        *id,
        *chapterId,
        span,
        query.value(6).toString(),
        query.value(7).toString(),
        *category,
        query.value(10).toDouble(),
        query.value(11).toString(),
        *impact,
        *origin,
        query.value(16).toString(),
        *hash,
    };
    return proofreading::RepairQueueItem{
        *projectId, candidate, query.value(8).toString(), query.value(12).toString(), *status,
        createdAt,  updatedAt};
}

QString selectItems() {
    return QStringLiteral(
        "SELECT id, project_id, chapter_id, source_id, start_byte, end_byte, original_text, "
        "detected_suggestion, current_suggestion, category, confidence, evidence, source_context, "
        "semantic_impact, origin, status, detector_version, source_hash, created_at, updated_at "
        "FROM proofreading_candidates ");
}

StorageStatus storeUpdated(QSqlDatabase connection, const proofreading::RepairQueueItem& before,
                           const proofreading::RepairQueueItem& after) {
    QSqlQuery update(connection);
    update.prepare(QStringLiteral(
        "UPDATE proofreading_candidates SET current_suggestion = ?, status = ?, updated_at = ? "
        "WHERE id = ? AND status = ?"));
    update.addBindValue(after.currentSuggestion);
    update.addBindValue(proofreading::candidateStatusName(after.status));
    update.addBindValue(after.updatedAt.toUTC().toString(Qt::ISODateWithMs));
    update.addBindValue(after.candidate.id.toString());
    update.addBindValue(proofreading::candidateStatusName(before.status));
    if (!update.exec()) {
        return queryError(QStringLiteral("The repair-queue decision could not be saved."),
                          update.lastError());
    }
    if (update.numRowsAffected() != 1) {
        return StorageError{StorageErrorCode::Conflict,
                            QStringLiteral("The candidate changed while it was being reviewed."),
                            {},
                            true};
    }
    return std::nullopt;
}

StorageStatus workflowError(const proofreading::RepairQueueResult& result) {
    if (std::holds_alternative<proofreading::RepairQueueError>(result)) {
        return invalid(std::get<proofreading::RepairQueueError>(result).message);
    }
    return std::nullopt;
}

} // namespace

RepairQueueRepository::RepairQueueRepository(ProjectDatabase& database) : database_(database) {}

StorageStatus RepairQueueRepository::enqueue(const core::ProjectId& projectId,
                                             const proofreading::ProofreadingCandidate& candidate,
                                             QString sourceContext, const QDateTime& now) {
    const auto reviewed = proofreading::RepairQueueWorkflow::review(projectId, candidate,
                                                                    std::move(sourceContext), now);
    if (const auto error = workflowError(reviewed); error.has_value()) {
        return error;
    }
    const auto& item = std::get<proofreading::RepairQueueItem>(reviewed);
    return database_.runInTransaction([this, &item]() -> StorageStatus {
        auto connection = database_.database();
        QSqlQuery ownership(connection);
        ownership.prepare(
            QStringLiteral("SELECT 1 FROM chapters c JOIN books b ON b.id = c.book_id "
                           "WHERE c.id = ? AND b.project_id = ?"));
        ownership.addBindValue(item.candidate.chapterId.toString());
        ownership.addBindValue(item.projectId.toString());
        if (!ownership.exec()) {
            return queryError(QStringLiteral("Candidate ownership could not be verified."),
                              ownership.lastError());
        }
        if (!ownership.next()) {
            return invalid(QStringLiteral("The candidate chapter does not belong to the project."));
        }
        QSqlQuery query(connection);
        query.prepare(QStringLiteral(
            "INSERT INTO proofreading_candidates(id, project_id, chapter_id, source_id, "
            "start_byte, end_byte, original_text, detected_suggestion, current_suggestion, "
            "category, confidence, evidence, source_context, semantic_impact, origin, status, "
            "detector_version, source_hash, created_at, updated_at) "
            "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));
        query.addBindValue(item.candidate.id.toString());
        query.addBindValue(item.projectId.toString());
        query.addBindValue(item.candidate.chapterId.toString());
        query.addBindValue(item.candidate.sourceSpan.sourceId);
        query.addBindValue(item.candidate.sourceSpan.startByte);
        query.addBindValue(item.candidate.sourceSpan.endByte);
        query.addBindValue(item.candidate.originalText);
        query.addBindValue(item.candidate.suggestedText);
        query.addBindValue(item.currentSuggestion);
        query.addBindValue(proofreading::candidateCategoryName(item.candidate.category));
        query.addBindValue(item.candidate.confidence);
        query.addBindValue(item.candidate.evidence);
        query.addBindValue(item.sourceContext);
        query.addBindValue(proofreading::semanticImpactName(item.candidate.semanticImpact));
        query.addBindValue(originName(item.candidate.origin));
        query.addBindValue(proofreading::candidateStatusName(item.status));
        query.addBindValue(item.candidate.detectorVersion);
        query.addBindValue(item.candidate.sourceHash.toHex());
        query.addBindValue(item.createdAt.toUTC().toString(Qt::ISODateWithMs));
        query.addBindValue(item.updatedAt.toUTC().toString(Qt::ISODateWithMs));
        if (!query.exec()) {
            return queryError(QStringLiteral("The proofreading candidate could not be queued."),
                              query.lastError());
        }
        return std::nullopt;
    });
}

StorageResult<proofreading::RepairQueueItem>
RepairQueueRepository::find(const core::ProofreadingCandidateId& candidateId) const {
    auto connection = database_.database();
    QSqlQuery query(connection);
    query.prepare(selectItems() + QStringLiteral("WHERE id = ?"));
    query.addBindValue(candidateId.toString());
    if (!query.exec()) {
        return queryError(QStringLiteral("The repair-queue candidate could not be read."),
                          query.lastError());
    }
    if (!query.next()) {
        return StorageError{StorageErrorCode::NotFound,
                            QStringLiteral("The repair-queue candidate was not found."),
                            {},
                            true};
    }
    return itemFromQuery(query);
}

StorageResult<QList<proofreading::RepairQueueItem>>
RepairQueueRepository::list(const core::ProjectId& projectId) const {
    if (!projectId.isValid()) {
        return invalid(QStringLiteral("A valid project is required."));
    }
    auto connection = database_.database();
    QSqlQuery query(connection);
    query.prepare(selectItems() + QStringLiteral("WHERE project_id = ? ORDER BY created_at, id"));
    query.addBindValue(projectId.toString());
    if (!query.exec()) {
        return queryError(QStringLiteral("The repair queue could not be listed."),
                          query.lastError());
    }
    QList<proofreading::RepairQueueItem> items;
    while (query.next()) {
        auto item = itemFromQuery(query);
        if (std::holds_alternative<StorageError>(item)) {
            return std::get<StorageError>(item);
        }
        items.append(std::get<proofreading::RepairQueueItem>(std::move(item)));
    }
    return items;
}

StorageStatus RepairQueueRepository::approve(const core::ProofreadingCandidateId& candidateId,
                                             const QDateTime& now) {
    const auto found = find(candidateId);
    if (std::holds_alternative<StorageError>(found)) {
        return std::get<StorageError>(found);
    }
    const auto& before = std::get<proofreading::RepairQueueItem>(found);
    const auto result = proofreading::RepairQueueWorkflow::approve(before, now);
    if (const auto error = workflowError(result); error.has_value()) {
        return error;
    }
    return storeUpdated(database_.database(), before,
                        std::get<proofreading::RepairQueueItem>(result));
}

StorageStatus RepairQueueRepository::reject(const core::ProofreadingCandidateId& candidateId,
                                            const QDateTime& now) {
    const auto found = find(candidateId);
    if (std::holds_alternative<StorageError>(found)) {
        return std::get<StorageError>(found);
    }
    const auto& before = std::get<proofreading::RepairQueueItem>(found);
    const auto result = proofreading::RepairQueueWorkflow::reject(before, now);
    if (const auto error = workflowError(result); error.has_value()) {
        return error;
    }
    return storeUpdated(database_.database(), before,
                        std::get<proofreading::RepairQueueItem>(result));
}

StorageStatus
RepairQueueRepository::editSuggestion(const core::ProofreadingCandidateId& candidateId,
                                      QString suggestion, const QDateTime& now) {
    const auto found = find(candidateId);
    if (std::holds_alternative<StorageError>(found)) {
        return std::get<StorageError>(found);
    }
    const auto& before = std::get<proofreading::RepairQueueItem>(found);
    const auto result =
        proofreading::RepairQueueWorkflow::editSuggestion(before, std::move(suggestion), now);
    if (const auto error = workflowError(result); error.has_value()) {
        return error;
    }
    return storeUpdated(database_.database(), before,
                        std::get<proofreading::RepairQueueItem>(result));
}

StorageStatus
RepairQueueRepository::ignoreAndProtect(const core::ProofreadingCandidateId& candidateId,
                                        QString notes, const QDateTime& now) {
    const auto found = find(candidateId);
    if (std::holds_alternative<StorageError>(found)) {
        return std::get<StorageError>(found);
    }
    const auto& before = std::get<proofreading::RepairQueueItem>(found);
    const auto rejected = proofreading::RepairQueueWorkflow::reject(before, now);
    if (const auto error = workflowError(rejected); error.has_value()) {
        return error;
    }
    const auto& after = std::get<proofreading::RepairQueueItem>(rejected);
    return database_.runInTransaction([this, &before, &after, &notes, &now]() -> StorageStatus {
        auto connection = database_.database();
        const auto canonical = before.candidate.originalText.trimmed();
        if (canonical.isEmpty()) {
            return invalid(QStringLiteral("A protected term cannot be empty."));
        }
        QSqlQuery term(connection);
        term.prepare(
            QStringLiteral("INSERT INTO protected_terms(id, project_id, canonical_spelling, "
                           "allowed_variants_json, notes, chapter_scope, created_at) VALUES(?, ?, "
                           "?, ?, ?, NULL, ?)"));
        term.addBindValue(core::ContentHash::sha256(before.projectId.toString() +
                                                    QStringLiteral("\n") + canonical)
                              .toHex());
        term.addBindValue(before.projectId.toString());
        term.addBindValue(canonical);
        term.addBindValue(QStringLiteral("[]"));
        term.addBindValue(notes.trimmed());
        term.addBindValue(now.toUTC().toString(Qt::ISODateWithMs));
        if (!term.exec()) {
            return queryError(QStringLiteral("The protected term could not be saved."),
                              term.lastError());
        }
        return storeUpdated(connection, before, after);
    });
}

StorageResult<QList<proofreading::ProtectedTerm>>
RepairQueueRepository::protectedTerms(const core::ProjectId& projectId) const {
    auto connection = database_.database();
    QSqlQuery query(connection);
    query.prepare(QStringLiteral(
        "SELECT canonical_spelling, allowed_variants_json, notes, chapter_scope "
        "FROM protected_terms WHERE project_id = ? ORDER BY canonical_spelling, chapter_scope"));
    query.addBindValue(projectId.toString());
    if (!query.exec()) {
        return queryError(QStringLiteral("Protected terms could not be listed."),
                          query.lastError());
    }
    QList<proofreading::ProtectedTerm> terms;
    while (query.next()) {
        QJsonParseError parseError;
        const auto variants =
            QJsonDocument::fromJson(query.value(1).toString().toUtf8(), &parseError);
        std::optional<core::ChapterId> chapterScope;
        if (!query.value(3).isNull()) {
            chapterScope = core::ChapterId::fromString(query.value(3).toString());
        }
        if (parseError.error != QJsonParseError::NoError || !variants.isArray() ||
            (!query.value(3).isNull() && !chapterScope.has_value())) {
            return corrupt(QStringLiteral("Stored protected-term data is invalid."));
        }
        QStringList allowed;
        for (const auto& value : variants.array()) {
            if (!value.isString()) {
                return corrupt(QStringLiteral("Stored protected-term variants are invalid."));
            }
            allowed.append(value.toString());
        }
        terms.append({query.value(0).toString(), allowed, query.value(2).toString(), chapterScope});
    }
    return terms;
}

} // namespace loreforge::storage
