#include "loreforge/storage/llm_run_repository.h"

#include "loreforge/storage/project_database.h"

#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include <utility>

namespace loreforge::storage {
namespace {

StorageError queryError(QString message, const QSqlError& error) {
    const auto details = (error.databaseText() + QLatin1Char(' ') + error.driverText()).trimmed();
    const auto conflict =
        details.contains(QStringLiteral("UNIQUE constraint failed"), Qt::CaseInsensitive);
    return {conflict ? StorageErrorCode::Conflict : StorageErrorCode::SqlError, std::move(message),
            details, true};
}

QString statusName(LLMRunStatus status) {
    switch (status) {
    case LLMRunStatus::Queued:
        return QStringLiteral("queued");
    case LLMRunStatus::Running:
        return QStringLiteral("running");
    case LLMRunStatus::Succeeded:
        return QStringLiteral("succeeded");
    case LLMRunStatus::Failed:
        return QStringLiteral("failed");
    case LLMRunStatus::Cancelled:
        return QStringLiteral("cancelled");
    }
    return {};
}

std::optional<LLMRunStatus> statusFromName(QStringView value) {
    if (value == QStringLiteral("queued")) {
        return LLMRunStatus::Queued;
    }
    if (value == QStringLiteral("running")) {
        return LLMRunStatus::Running;
    }
    if (value == QStringLiteral("succeeded")) {
        return LLMRunStatus::Succeeded;
    }
    if (value == QStringLiteral("failed")) {
        return LLMRunStatus::Failed;
    }
    if (value == QStringLiteral("cancelled")) {
        return LLMRunStatus::Cancelled;
    }
    return std::nullopt;
}

bool isTerminal(LLMRunStatus status) {
    return status == LLMRunStatus::Succeeded || status == LLMRunStatus::Failed ||
           status == LLMRunStatus::Cancelled;
}

StorageStatus validate(const LLMRunRecord& run) {
    const bool completionStateValid =
        isTerminal(run.status) ? run.completedAt.has_value() && run.latencyMs.has_value()
                               : !run.completedAt.has_value() && !run.latencyMs.has_value();
    if (!run.id.isValid() || !run.projectId.isValid() || run.requestId.isNull() ||
        run.provider.trimmed().isEmpty() || run.model.trimmed().isEmpty() || run.attemptCount < 0 ||
        run.promptTokens < 0 || run.completionTokens < 0 || run.totalTokens < 0 ||
        !run.startedAt.isValid() || !completionStateValid ||
        (run.completedAt.has_value() && !run.completedAt->isValid()) ||
        (run.latencyMs.has_value() && *run.latencyMs < 0)) {
        return StorageError{StorageErrorCode::InvalidArgument,
                            QStringLiteral("The LLM run record is invalid."),
                            {},
                            true};
    }
    return std::nullopt;
}

StorageResult<LLMRunRecord> recordFromQuery(const QSqlQuery& query) {
    const auto id = core::LLMRunId::fromString(query.value(0).toString());
    const auto projectId = core::ProjectId::fromString(query.value(1).toString());
    const QUuid requestId(query.value(2).toString());
    const auto status = statusFromName(query.value(5).toString());
    const auto startedAt = QDateTime::fromString(query.value(10).toString(), Qt::ISODateWithMs);
    std::optional<QDateTime> completedAt;
    if (!query.isNull(11)) {
        completedAt = QDateTime::fromString(query.value(11).toString(), Qt::ISODateWithMs);
    }
    std::optional<qint64> latencyMs;
    if (!query.isNull(12)) {
        latencyMs = query.value(12).toLongLong();
    }
    if (!id.has_value() || !projectId.has_value() || requestId.isNull() || !status.has_value()) {
        return StorageError{StorageErrorCode::CorruptData,
                            QStringLiteral("Stored LLM run data is invalid."),
                            {},
                            false};
    }
    LLMRunRecord run{*id,
                     *projectId,
                     requestId,
                     query.value(3).toString(),
                     query.value(4).toString(),
                     *status,
                     query.value(6).toInt(),
                     query.value(7).toInt(),
                     query.value(8).toInt(),
                     query.value(9).toInt(),
                     startedAt,
                     completedAt,
                     latencyMs,
                     query.value(13).toString(),
                     query.value(14).toString()};
    if (const auto validation = validate(run); validation.has_value()) {
        return StorageError{StorageErrorCode::CorruptData,
                            QStringLiteral("Stored LLM run data is invalid."), validation->message,
                            false};
    }
    return run;
}

QString selectColumns() {
    return QStringLiteral("id, project_id, request_id, provider, model, status, attempt_count, "
                          "prompt_tokens, completion_tokens, total_tokens, started_at, "
                          "completed_at, latency_ms, error_code, error_message");
}

} // namespace

LLMRunRepository::LLMRunRepository(ProjectDatabase& database) : database_(database) {}

StorageStatus LLMRunRepository::save(const LLMRunRecord& run) {
    if (const auto validation = validate(run); validation.has_value()) {
        return validation;
    }
    auto connection = database_.database();
    QSqlQuery query(connection);
    query.prepare(QStringLiteral(
        "INSERT INTO llm_runs(id, project_id, request_id, provider, model, status, attempt_count, "
        "prompt_tokens, completion_tokens, total_tokens, started_at, completed_at, latency_ms, "
        "error_code, error_message) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(id) DO UPDATE SET project_id=excluded.project_id, "
        "request_id=excluded.request_id, provider=excluded.provider, model=excluded.model, "
        "status=excluded.status, attempt_count=excluded.attempt_count, "
        "prompt_tokens=excluded.prompt_tokens, completion_tokens=excluded.completion_tokens, "
        "total_tokens=excluded.total_tokens, started_at=excluded.started_at, "
        "completed_at=excluded.completed_at, latency_ms=excluded.latency_ms, "
        "error_code=excluded.error_code, error_message=excluded.error_message"));
    query.addBindValue(run.id.toString());
    query.addBindValue(run.projectId.toString());
    query.addBindValue(run.requestId.toString(QUuid::WithoutBraces));
    query.addBindValue(run.provider);
    query.addBindValue(run.model);
    query.addBindValue(statusName(run.status));
    query.addBindValue(run.attemptCount);
    query.addBindValue(run.promptTokens);
    query.addBindValue(run.completionTokens);
    query.addBindValue(run.totalTokens);
    query.addBindValue(run.startedAt.toUTC().toString(Qt::ISODateWithMs));
    query.addBindValue(run.completedAt.has_value()
                           ? QVariant(run.completedAt->toUTC().toString(Qt::ISODateWithMs))
                           : QVariant());
    query.addBindValue(run.latencyMs.has_value() ? QVariant::fromValue(*run.latencyMs)
                                                 : QVariant());
    query.addBindValue(run.errorCode.isNull() ? QStringLiteral("") : run.errorCode);
    query.addBindValue(run.errorMessage.isNull() ? QStringLiteral("") : run.errorMessage);
    if (!query.exec()) {
        return queryError(QStringLiteral("The LLM run could not be stored."), query.lastError());
    }
    return std::nullopt;
}

StorageResult<LLMRunRecord> LLMRunRepository::find(const core::LLMRunId& runId) const {
    auto connection = database_.database();
    QSqlQuery query(connection);
    query.prepare(QStringLiteral("SELECT %1 FROM llm_runs WHERE id = ?").arg(selectColumns()));
    query.addBindValue(runId.toString());
    if (!query.exec()) {
        return queryError(QStringLiteral("The LLM run could not be read."), query.lastError());
    }
    if (!query.next()) {
        return StorageError{
            StorageErrorCode::NotFound, QStringLiteral("The LLM run was not found."), {}, true};
    }
    return recordFromQuery(query);
}

StorageResult<QList<LLMRunRecord>>
LLMRunRepository::listForProject(const core::ProjectId& projectId) const {
    auto connection = database_.database();
    QSqlQuery query(connection);
    query.prepare(QStringLiteral("SELECT %1 FROM llm_runs WHERE project_id = ? "
                                 "ORDER BY started_at, id")
                      .arg(selectColumns()));
    query.addBindValue(projectId.toString());
    if (!query.exec()) {
        return queryError(QStringLiteral("LLM runs could not be listed."), query.lastError());
    }
    QList<LLMRunRecord> runs;
    while (query.next()) {
        auto run = recordFromQuery(query);
        if (std::holds_alternative<StorageError>(run)) {
            return std::get<StorageError>(run);
        }
        runs.append(std::get<LLMRunRecord>(std::move(run)));
    }
    return runs;
}

} // namespace loreforge::storage
