#include "loreforge/storage/inference_repository.h"

#include "loreforge/inference/output_validator.h"
#include "loreforge/storage/project_database.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include <utility>
#include <variant>

namespace loreforge::storage {
namespace {

StorageError error(StorageErrorCode code, QString message, QString details = {},
                   bool recoverable = true) {
    return {code, std::move(message), std::move(details), recoverable};
}

StorageError queryError(QString message, const QSqlError& sqlError) {
    const auto details =
        (sqlError.databaseText() + QLatin1Char(' ') + sqlError.driverText()).trimmed();
    const auto conflict =
        details.contains(QStringLiteral("constraint failed"), Qt::CaseInsensitive);
    return error(conflict ? StorageErrorCode::Conflict : StorageErrorCode::SqlError,
                 std::move(message), details);
}

StorageResult<QJsonObject> decodeObject(const QString& encoded, QString message) {
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(encoded.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return error(StorageErrorCode::CorruptData, std::move(message), parseError.errorString(),
                     false);
    }
    return document.object();
}

QString validationStatusName(inference::ValidationStatus status) {
    switch (status) {
    case inference::ValidationStatus::Pending:
        return QStringLiteral("pending");
    case inference::ValidationStatus::Valid:
        return QStringLiteral("valid");
    case inference::ValidationStatus::Invalid:
        return QStringLiteral("invalid");
    case inference::ValidationStatus::Unavailable:
        return QStringLiteral("unavailable");
    }
    return {};
}

std::optional<inference::ValidationStatus> validationStatusFromName(QStringView name) {
    if (name == QStringLiteral("pending")) {
        return inference::ValidationStatus::Pending;
    }
    if (name == QStringLiteral("valid")) {
        return inference::ValidationStatus::Valid;
    }
    if (name == QStringLiteral("invalid")) {
        return inference::ValidationStatus::Invalid;
    }
    if (name == QStringLiteral("unavailable")) {
        return inference::ValidationStatus::Unavailable;
    }
    return std::nullopt;
}

QString encodeErrors(const QStringList& errors) {
    QJsonArray values;
    for (const auto& validationError : errors) {
        values.append(validationError);
    }
    return QString::fromUtf8(QJsonDocument(values).toJson(QJsonDocument::Compact));
}

StorageResult<QStringList> decodeErrors(const QString& encoded) {
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(encoded.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isArray()) {
        return error(StorageErrorCode::CorruptData,
                     QStringLiteral("Stored validation errors are invalid."),
                     parseError.errorString(), false);
    }
    QStringList errors;
    for (const auto& value : document.array()) {
        if (!value.isString()) {
            return error(StorageErrorCode::CorruptData,
                         QStringLiteral("Stored validation errors are invalid."), {}, false);
        }
        errors.append(value.toString());
    }
    return errors;
}

StorageStatus validatePromptVersion(const inference::PromptVersion& version) {
    if (!version.prompt.id.isValid() || version.prompt.name.trimmed().isEmpty() ||
        version.version <= 0 || version.text.trimmed().isEmpty() || !version.createdAt.isValid() ||
        version.contentHash != core::ContentHash::sha256(QStringView(version.text))) {
        return error(StorageErrorCode::InvalidArgument,
                     QStringLiteral("The prompt version is invalid."));
    }
    return std::nullopt;
}

StorageStatus validateOutputSchema(const inference::OutputSchema& schema) {
    const auto schemaErrors = inference::OutputValidator::validateSchema(schema.schema);
    if (!schema.id.isValid() || schema.name.trimmed().isEmpty() || schema.version <= 0 ||
        schema.schema.isEmpty() || !schema.createdAt.isValid() ||
        schema.contentHash != core::ContentHash::sha256(inference::canonicalJson(schema.schema)) ||
        !schemaErrors.isEmpty()) {
        return error(StorageErrorCode::InvalidArgument,
                     QStringLiteral("The output schema is invalid."),
                     schemaErrors.join(QLatin1Char('\n')));
    }
    return std::nullopt;
}

StorageStatus validateContextSnapshot(const inference::ContextSnapshot& snapshot) {
    if (!snapshot.id.isValid() || !snapshot.projectId.isValid() || snapshot.content.isEmpty() ||
        !snapshot.createdAt.isValid() ||
        snapshot.contentHash !=
            core::ContentHash::sha256(inference::canonicalJson(snapshot.content))) {
        return error(StorageErrorCode::InvalidArgument,
                     QStringLiteral("The context snapshot is invalid."));
    }
    return std::nullopt;
}

bool isJsonObject(const QByteArray& bytes) {
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(bytes, &parseError);
    return parseError.error == QJsonParseError::NoError && document.isObject();
}

StorageStatus validatePendingArtifacts(const inference::LLMRunArtifacts& artifacts) {
    if (!artifacts.runId.isValid() || !artifacts.promptTemplateId.isValid() ||
        artifacts.promptVersion <= 0 || !artifacts.outputSchemaId.isValid() ||
        artifacts.outputSchemaVersion <= 0 || !artifacts.contextSnapshotId.isValid() ||
        artifacts.rawRequest.isEmpty() || !isJsonObject(artifacts.rawRequest) ||
        artifacts.rawResponse.has_value() || artifacts.parsedResponse.has_value() ||
        artifacts.validation.status != inference::ValidationStatus::Pending ||
        !artifacts.validation.errors.isEmpty()) {
        return error(StorageErrorCode::InvalidArgument,
                     QStringLiteral("The pending LLM run artifacts are invalid."));
    }
    return std::nullopt;
}

StorageStatus validateFinalArtifacts(const std::optional<QByteArray>& rawResponse,
                                     const std::optional<QJsonDocument>& parsedResponse,
                                     const inference::ValidationReport& validation) {
    const bool hasRawResponse = rawResponse.has_value() && !rawResponse->isEmpty();
    const bool hasParsedResponse = parsedResponse.has_value() && !parsedResponse->isNull();
    switch (validation.status) {
    case inference::ValidationStatus::Pending:
        break;
    case inference::ValidationStatus::Valid:
        if (hasRawResponse && hasParsedResponse && validation.errors.isEmpty()) {
            return std::nullopt;
        }
        break;
    case inference::ValidationStatus::Invalid:
        if (hasRawResponse && !validation.errors.isEmpty()) {
            return std::nullopt;
        }
        break;
    case inference::ValidationStatus::Unavailable:
        if (!rawResponse.has_value() && !parsedResponse.has_value() &&
            !validation.errors.isEmpty()) {
            return std::nullopt;
        }
        break;
    }
    return error(StorageErrorCode::InvalidArgument,
                 QStringLiteral("The finalized LLM run artifacts are invalid."));
}

QJsonValue documentValue(const QJsonDocument& document) {
    return document.isObject() ? QJsonValue(document.object()) : QJsonValue(document.array());
}

} // namespace

InferenceRepository::InferenceRepository(ProjectDatabase& database) : database_(database) {}

StorageStatus InferenceRepository::savePromptVersion(const inference::PromptVersion& version) {
    if (const auto status = validatePromptVersion(version); status.has_value()) {
        return status;
    }
    return database_.runInTransaction([this, &version] {
        auto connection = database_.database();
        QSqlQuery prompt(connection);
        prompt.prepare(
            QStringLiteral("INSERT OR IGNORE INTO prompt_templates(id, name) VALUES(?, ?)"));
        prompt.addBindValue(version.prompt.id.toString());
        prompt.addBindValue(version.prompt.name);
        if (!prompt.exec()) {
            return StorageStatus(queryError(
                QStringLiteral("The prompt template could not be stored."), prompt.lastError()));
        }
        QSqlQuery existingPrompt(connection);
        existingPrompt.prepare(QStringLiteral("SELECT name FROM prompt_templates WHERE id = ?"));
        existingPrompt.addBindValue(version.prompt.id.toString());
        if (!existingPrompt.exec() || !existingPrompt.next()) {
            return StorageStatus(
                queryError(QStringLiteral("The prompt template could not be verified."),
                           existingPrompt.lastError()));
        }
        if (existingPrompt.value(0).toString() != version.prompt.name) {
            return StorageStatus(
                error(StorageErrorCode::Conflict,
                      QStringLiteral("The prompt template ID already has another name.")));
        }

        QSqlQuery query(connection);
        query.prepare(
            QStringLiteral("INSERT INTO prompt_versions(template_id, version, template_text, "
                           "content_hash, created_at) VALUES(?, ?, ?, ?, ?)"));
        query.addBindValue(version.prompt.id.toString());
        query.addBindValue(version.version);
        query.addBindValue(version.text);
        query.addBindValue(version.contentHash.toHex());
        query.addBindValue(version.createdAt.toUTC().toString(Qt::ISODateWithMs));
        if (!query.exec()) {
            return StorageStatus(queryError(
                QStringLiteral("The prompt version could not be stored."), query.lastError()));
        }
        return StorageStatus{};
    });
}

StorageResult<inference::PromptVersion>
InferenceRepository::findPromptVersion(const core::PromptTemplateId& templateId,
                                       int version) const {
    auto connection = database_.database();
    QSqlQuery query(connection);
    query.prepare(QStringLiteral("SELECT t.id, t.name, v.version, v.template_text, v.content_hash, "
                                 "v.created_at FROM prompt_templates t JOIN prompt_versions v ON "
                                 "v.template_id=t.id WHERE t.id=? AND v.version=?"));
    query.addBindValue(templateId.toString());
    query.addBindValue(version);
    if (!query.exec()) {
        return queryError(QStringLiteral("The prompt version could not be read."),
                          query.lastError());
    }
    if (!query.next()) {
        return error(StorageErrorCode::NotFound,
                     QStringLiteral("The prompt version was not found."));
    }
    const auto id = core::PromptTemplateId::fromString(query.value(0).toString());
    const auto hash = core::ContentHash::fromHex(query.value(4).toString());
    const auto createdAt = QDateTime::fromString(query.value(5).toString(), Qt::ISODateWithMs);
    if (!id.has_value() || !hash.has_value()) {
        return error(StorageErrorCode::CorruptData,
                     QStringLiteral("Stored prompt version data is invalid."), {}, false);
    }
    inference::PromptVersion result{{*id, query.value(1).toString()},
                                    query.value(2).toInt(),
                                    query.value(3).toString(),
                                    *hash,
                                    createdAt};
    if (const auto status = validatePromptVersion(result); status.has_value()) {
        return error(StorageErrorCode::CorruptData,
                     QStringLiteral("Stored prompt version data is invalid."), status->message,
                     false);
    }
    return result;
}

StorageStatus InferenceRepository::saveOutputSchema(const inference::OutputSchema& schema) {
    if (const auto status = validateOutputSchema(schema); status.has_value()) {
        return status;
    }
    auto connection = database_.database();
    QSqlQuery query(connection);
    query.prepare(QStringLiteral("INSERT INTO output_schemas(id, version, name, schema_json, "
                                 "content_hash, created_at) VALUES(?, ?, ?, ?, ?, ?)"));
    query.addBindValue(schema.id.toString());
    query.addBindValue(schema.version);
    query.addBindValue(schema.name);
    query.addBindValue(QString::fromUtf8(inference::canonicalJson(schema.schema)));
    query.addBindValue(schema.contentHash.toHex());
    query.addBindValue(schema.createdAt.toUTC().toString(Qt::ISODateWithMs));
    if (!query.exec()) {
        return queryError(QStringLiteral("The output schema could not be stored."),
                          query.lastError());
    }
    return std::nullopt;
}

StorageResult<inference::OutputSchema>
InferenceRepository::findOutputSchema(const core::OutputSchemaId& schemaId, int version) const {
    auto connection = database_.database();
    QSqlQuery query(connection);
    query.prepare(QStringLiteral("SELECT id, name, version, schema_json, content_hash, created_at "
                                 "FROM output_schemas WHERE id=? AND version=?"));
    query.addBindValue(schemaId.toString());
    query.addBindValue(version);
    if (!query.exec()) {
        return queryError(QStringLiteral("The output schema could not be read."),
                          query.lastError());
    }
    if (!query.next()) {
        return error(StorageErrorCode::NotFound,
                     QStringLiteral("The output schema was not found."));
    }
    const auto id = core::OutputSchemaId::fromString(query.value(0).toString());
    const auto schema = decodeObject(query.value(3).toString(),
                                     QStringLiteral("Stored output schema JSON is invalid."));
    const auto hash = core::ContentHash::fromHex(query.value(4).toString());
    const auto createdAt = QDateTime::fromString(query.value(5).toString(), Qt::ISODateWithMs);
    if (!id.has_value() || std::holds_alternative<StorageError>(schema) || !hash.has_value()) {
        if (std::holds_alternative<StorageError>(schema)) {
            return std::get<StorageError>(schema);
        }
        return error(StorageErrorCode::CorruptData,
                     QStringLiteral("Stored output schema data is invalid."), {}, false);
    }
    inference::OutputSchema result{*id,
                                   query.value(1).toString(),
                                   query.value(2).toInt(),
                                   std::get<QJsonObject>(schema),
                                   *hash,
                                   createdAt};
    if (const auto status = validateOutputSchema(result); status.has_value()) {
        return error(StorageErrorCode::CorruptData,
                     QStringLiteral("Stored output schema data is invalid."), status->message,
                     false);
    }
    return result;
}

StorageStatus InferenceRepository::saveContextSnapshot(const inference::ContextSnapshot& snapshot) {
    if (const auto status = validateContextSnapshot(snapshot); status.has_value()) {
        return status;
    }
    auto connection = database_.database();
    QSqlQuery query(connection);
    query.prepare(QStringLiteral("INSERT INTO context_snapshots(id, project_id, content_json, "
                                 "content_hash, created_at) VALUES(?, ?, ?, ?, ?)"));
    query.addBindValue(snapshot.id.toString());
    query.addBindValue(snapshot.projectId.toString());
    query.addBindValue(QString::fromUtf8(inference::canonicalJson(snapshot.content)));
    query.addBindValue(snapshot.contentHash.toHex());
    query.addBindValue(snapshot.createdAt.toUTC().toString(Qt::ISODateWithMs));
    if (!query.exec()) {
        return queryError(QStringLiteral("The context snapshot could not be stored."),
                          query.lastError());
    }
    return std::nullopt;
}

StorageResult<inference::ContextSnapshot>
InferenceRepository::findContextSnapshot(const core::ContextSnapshotId& snapshotId) const {
    auto connection = database_.database();
    QSqlQuery query(connection);
    query.prepare(QStringLiteral("SELECT id, project_id, content_json, content_hash, created_at "
                                 "FROM context_snapshots WHERE id=?"));
    query.addBindValue(snapshotId.toString());
    if (!query.exec()) {
        return queryError(QStringLiteral("The context snapshot could not be read."),
                          query.lastError());
    }
    if (!query.next()) {
        return error(StorageErrorCode::NotFound,
                     QStringLiteral("The context snapshot was not found."));
    }
    const auto id = core::ContextSnapshotId::fromString(query.value(0).toString());
    const auto projectId = core::ProjectId::fromString(query.value(1).toString());
    const auto content = decodeObject(query.value(2).toString(),
                                      QStringLiteral("Stored context snapshot JSON is invalid."));
    const auto hash = core::ContentHash::fromHex(query.value(3).toString());
    const auto createdAt = QDateTime::fromString(query.value(4).toString(), Qt::ISODateWithMs);
    if (!id.has_value() || !projectId.has_value() ||
        std::holds_alternative<StorageError>(content) || !hash.has_value()) {
        if (std::holds_alternative<StorageError>(content)) {
            return std::get<StorageError>(content);
        }
        return error(StorageErrorCode::CorruptData,
                     QStringLiteral("Stored context snapshot data is invalid."), {}, false);
    }
    inference::ContextSnapshot result{*id, *projectId, std::get<QJsonObject>(content), *hash,
                                      createdAt};
    if (const auto status = validateContextSnapshot(result); status.has_value()) {
        return error(StorageErrorCode::CorruptData,
                     QStringLiteral("Stored context snapshot data is invalid."), status->message,
                     false);
    }
    return result;
}

StorageStatus InferenceRepository::createRunArtifacts(const inference::LLMRunArtifacts& artifacts) {
    if (const auto status = validatePendingArtifacts(artifacts); status.has_value()) {
        return status;
    }
    auto connection = database_.database();
    QSqlQuery ownership(connection);
    ownership.prepare(QStringLiteral("SELECT 1 FROM llm_runs r JOIN context_snapshots c ON c.id=? "
                                     "WHERE r.id=? AND r.project_id=c.project_id"));
    ownership.addBindValue(artifacts.contextSnapshotId.toString());
    ownership.addBindValue(artifacts.runId.toString());
    if (!ownership.exec()) {
        return queryError(QStringLiteral("The inference snapshot relationship could not be read."),
                          ownership.lastError());
    }
    if (!ownership.next()) {
        return error(
            StorageErrorCode::InvalidArgument,
            QStringLiteral("The LLM run and context snapshot must belong to one project."));
    }
    QSqlQuery query(connection);
    query.prepare(QStringLiteral(
        "INSERT INTO llm_run_artifacts(run_id, prompt_template_id, prompt_version, "
        "output_schema_id, output_schema_version, context_snapshot_id, raw_request, "
        "raw_response, parsed_response_json, validation_status, validation_errors_json) "
        "VALUES(?, ?, ?, ?, ?, ?, ?, NULL, NULL, 'pending', '[]')"));
    query.addBindValue(artifacts.runId.toString());
    query.addBindValue(artifacts.promptTemplateId.toString());
    query.addBindValue(artifacts.promptVersion);
    query.addBindValue(artifacts.outputSchemaId.toString());
    query.addBindValue(artifacts.outputSchemaVersion);
    query.addBindValue(artifacts.contextSnapshotId.toString());
    query.addBindValue(artifacts.rawRequest);
    if (!query.exec()) {
        return queryError(QStringLiteral("The LLM run artifacts could not be stored."),
                          query.lastError());
    }
    return std::nullopt;
}

StorageStatus InferenceRepository::finalizeRunArtifacts(const core::LLMRunId& runId,
                                                        std::optional<QByteArray> rawResponse,
                                                        std::optional<QJsonDocument> parsedResponse,
                                                        inference::ValidationReport validation) {
    if (!runId.isValid()) {
        return error(StorageErrorCode::InvalidArgument,
                     QStringLiteral("The LLM run ID is invalid."));
    }
    if (const auto status = validateFinalArtifacts(rawResponse, parsedResponse, validation);
        status.has_value()) {
        return status;
    }
    const auto artifactsResult = findRunArtifacts(runId);
    if (std::holds_alternative<StorageError>(artifactsResult)) {
        return std::get<StorageError>(artifactsResult);
    }
    const auto& artifacts = std::get<inference::LLMRunArtifacts>(artifactsResult);
    if (artifacts.validation.status != inference::ValidationStatus::Pending) {
        return error(StorageErrorCode::Conflict,
                     QStringLiteral("The LLM run artifacts are already finalized."));
    }
    if (parsedResponse.has_value()) {
        const auto schemaResult =
            findOutputSchema(artifacts.outputSchemaId, artifacts.outputSchemaVersion);
        if (std::holds_alternative<StorageError>(schemaResult)) {
            return std::get<StorageError>(schemaResult);
        }
        const auto expected = inference::OutputValidator::validate(
            std::get<inference::OutputSchema>(schemaResult).schema, documentValue(*parsedResponse));
        if (validation != expected) {
            return error(StorageErrorCode::InvalidArgument,
                         QStringLiteral("The validation report does not match the stored schema."));
        }
    }

    auto connection = database_.database();
    QSqlQuery query(connection);
    query.prepare(QStringLiteral("UPDATE llm_run_artifacts SET raw_response=?, "
                                 "parsed_response_json=?, validation_status=?, "
                                 "validation_errors_json=? WHERE run_id=? AND "
                                 "validation_status='pending'"));
    query.addBindValue(rawResponse.has_value() ? QVariant(*rawResponse) : QVariant());
    query.addBindValue(
        parsedResponse.has_value()
            ? QVariant(QString::fromUtf8(parsedResponse->toJson(QJsonDocument::Compact)))
            : QVariant());
    query.addBindValue(validationStatusName(validation.status));
    query.addBindValue(encodeErrors(validation.errors));
    query.addBindValue(runId.toString());
    if (!query.exec()) {
        return queryError(QStringLiteral("The LLM run artifacts could not be finalized."),
                          query.lastError());
    }
    if (query.numRowsAffected() != 1) {
        return error(StorageErrorCode::Conflict,
                     QStringLiteral("The LLM run artifacts could not be finalized."));
    }
    return std::nullopt;
}

StorageResult<inference::LLMRunArtifacts>
InferenceRepository::findRunArtifacts(const core::LLMRunId& runId) const {
    auto connection = database_.database();
    QSqlQuery query(connection);
    query.prepare(
        QStringLiteral("SELECT run_id, prompt_template_id, prompt_version, output_schema_id, "
                       "output_schema_version, context_snapshot_id, raw_request, raw_response, "
                       "parsed_response_json, validation_status, validation_errors_json "
                       "FROM llm_run_artifacts WHERE run_id=?"));
    query.addBindValue(runId.toString());
    if (!query.exec()) {
        return queryError(QStringLiteral("The LLM run artifacts could not be read."),
                          query.lastError());
    }
    if (!query.next()) {
        return error(StorageErrorCode::NotFound,
                     QStringLiteral("The LLM run artifacts were not found."));
    }
    const auto storedRunId = core::LLMRunId::fromString(query.value(0).toString());
    const auto promptId = core::PromptTemplateId::fromString(query.value(1).toString());
    const auto schemaId = core::OutputSchemaId::fromString(query.value(3).toString());
    const auto snapshotId = core::ContextSnapshotId::fromString(query.value(5).toString());
    const auto validationStatus = validationStatusFromName(query.value(9).toString());
    const auto validationErrors = decodeErrors(query.value(10).toString());
    if (!storedRunId.has_value() || !promptId.has_value() || !schemaId.has_value() ||
        !snapshotId.has_value() || !validationStatus.has_value() ||
        std::holds_alternative<StorageError>(validationErrors)) {
        if (std::holds_alternative<StorageError>(validationErrors)) {
            return std::get<StorageError>(validationErrors);
        }
        return error(StorageErrorCode::CorruptData,
                     QStringLiteral("Stored LLM run artifacts are invalid."), {}, false);
    }

    std::optional<QByteArray> rawResponse;
    if (!query.isNull(7)) {
        rawResponse = query.value(7).toByteArray();
    }
    std::optional<QJsonDocument> parsedResponse;
    if (!query.isNull(8)) {
        QJsonParseError parseError;
        const auto document = QJsonDocument::fromJson(query.value(8).toByteArray(), &parseError);
        if (parseError.error != QJsonParseError::NoError || document.isNull()) {
            return error(StorageErrorCode::CorruptData,
                         QStringLiteral("Stored parsed response JSON is invalid."),
                         parseError.errorString(), false);
        }
        parsedResponse = document;
    }
    inference::LLMRunArtifacts result{*storedRunId,
                                      *promptId,
                                      query.value(2).toInt(),
                                      *schemaId,
                                      query.value(4).toInt(),
                                      *snapshotId,
                                      query.value(6).toByteArray(),
                                      rawResponse,
                                      parsedResponse,
                                      {*validationStatus, std::get<QStringList>(validationErrors)}};
    if (result.validation.status == inference::ValidationStatus::Pending) {
        if (const auto status = validatePendingArtifacts(result); status.has_value()) {
            return error(StorageErrorCode::CorruptData,
                         QStringLiteral("Stored LLM run artifacts are invalid."), status->message,
                         false);
        }
    } else if (const auto status = validateFinalArtifacts(result.rawResponse, result.parsedResponse,
                                                          result.validation);
               status.has_value()) {
        return error(StorageErrorCode::CorruptData,
                     QStringLiteral("Stored LLM run artifacts are invalid."), status->message,
                     false);
    }
    if (result.parsedResponse.has_value()) {
        const auto schemaResult =
            findOutputSchema(result.outputSchemaId, result.outputSchemaVersion);
        if (std::holds_alternative<StorageError>(schemaResult)) {
            return std::get<StorageError>(schemaResult);
        }
        const auto expected = inference::OutputValidator::validate(
            std::get<inference::OutputSchema>(schemaResult).schema,
            documentValue(*result.parsedResponse));
        if (result.validation != expected) {
            return error(StorageErrorCode::CorruptData,
                         QStringLiteral("Stored response validation is inconsistent."), {}, false);
        }
    }
    return result;
}

StorageResult<StoredInferenceSnapshot>
InferenceRepository::inspectRun(const core::LLMRunId& runId) const {
    auto artifacts = findRunArtifacts(runId);
    if (std::holds_alternative<StorageError>(artifacts)) {
        return std::get<StorageError>(artifacts);
    }
    const auto& artifactRecord = std::get<inference::LLMRunArtifacts>(artifacts);
    auto prompt = findPromptVersion(artifactRecord.promptTemplateId, artifactRecord.promptVersion);
    if (std::holds_alternative<StorageError>(prompt)) {
        return std::get<StorageError>(prompt);
    }
    auto schema =
        findOutputSchema(artifactRecord.outputSchemaId, artifactRecord.outputSchemaVersion);
    if (std::holds_alternative<StorageError>(schema)) {
        return std::get<StorageError>(schema);
    }
    auto snapshot = findContextSnapshot(artifactRecord.contextSnapshotId);
    if (std::holds_alternative<StorageError>(snapshot)) {
        return std::get<StorageError>(snapshot);
    }
    return StoredInferenceSnapshot{std::get<inference::PromptVersion>(std::move(prompt)),
                                   std::get<inference::OutputSchema>(std::move(schema)),
                                   std::get<inference::ContextSnapshot>(std::move(snapshot)),
                                   std::get<inference::LLMRunArtifacts>(std::move(artifacts))};
}

} // namespace loreforge::storage
