#include "loreforge/context/context_engine.h"

#include "loreforge/storage/inference_repository.h"

#include <QJsonObject>

#include <utility>

namespace loreforge::context {
namespace {

ContextOperationError storageError(const storage::StorageError& error) {
    return {ContextOperationErrorCode::StorageFailed, error.message, error.technicalDetails};
}

QString buildDetails(const QList<ContextBuildError>& errors) {
    QStringList details;
    for (const auto& error : errors) {
        details.append(error.path + QStringLiteral(": ") + error.message);
    }
    return details.join(QLatin1Char('\n'));
}

} // namespace

ContextEngine::ContextEngine(storage::InferenceRepository& repository) : repository_(repository) {}

ContextPreparationResult ContextEngine::prepareAndStore(ContextBuildInput input,
                                                        ContextBudget budget, QDateTime createdAt) {
    auto built = ContextBuilder::build(std::move(input), budget, std::move(createdAt));
    if (!built.isValid()) {
        return ContextOperationError{ContextOperationErrorCode::BuildFailed,
                                     QStringLiteral("The bounded context could not be built."),
                                     buildDetails(built.errors)};
    }
    auto context = std::move(*built.context);
    if (const auto status = repository_.saveContextSnapshot(context.snapshot); status.has_value()) {
        if (status->code != storage::StorageErrorCode::Conflict) {
            return storageError(*status);
        }
        const auto existing = repository_.findContextSnapshot(context.snapshot.id);
        if (std::holds_alternative<storage::StorageError>(existing)) {
            return storageError(std::get<storage::StorageError>(existing));
        }
        const auto& snapshot = std::get<inference::ContextSnapshot>(existing);
        if (snapshot.projectId != context.snapshot.projectId ||
            snapshot.contentHash != context.snapshot.contentHash ||
            snapshot.content != context.snapshot.content) {
            return ContextOperationError{
                ContextOperationErrorCode::StorageFailed,
                QStringLiteral("The context snapshot ID conflicts with different stored content."),
                {}};
        }
        context.snapshot = snapshot;
    }
    return context;
}

StoredContextRequestResult
ContextEngine::requestFromStoredSnapshot(const core::ContextSnapshotId& snapshotId,
                                         llm::LLMRequest requestTemplate) const {
    const auto stored = repository_.findContextSnapshot(snapshotId);
    if (std::holds_alternative<storage::StorageError>(stored)) {
        return storageError(std::get<storage::StorageError>(stored));
    }
    const auto snapshot = std::get<inference::ContextSnapshot>(stored);
    const auto inspector = inspectorFromSnapshot(snapshot);
    if (!inspector.has_value()) {
        return ContextOperationError{
            ContextOperationErrorCode::InvalidStoredSnapshot,
            QStringLiteral("The stored snapshot is not a Context Engine snapshot."),
            {}};
    }
    if (requestTemplate.model.trimmed().isEmpty() || requestTemplate.timeoutMs <= 0 ||
        requestTemplate.maxCompletionTokens < 0 ||
        requestTemplate.maxCompletionTokens > inspector->budget.reservedCompletionTokens ||
        requestTemplate.retryPolicy.maxRetries < 0 ||
        requestTemplate.retryPolicy.initialDelayMs < 0 ||
        requestTemplate.retryPolicy.maximumDelayMs < requestTemplate.retryPolicy.initialDelayMs) {
        return ContextOperationError{
            ContextOperationErrorCode::InvalidRequestTemplate,
            QStringLiteral("The request template exceeds the stored budget or is invalid."),
            {}};
    }
    if (requestTemplate.maxCompletionTokens == 0) {
        requestTemplate.maxCompletionTokens = inspector->budget.reservedCompletionTokens;
    }
    requestTemplate.messages = {{llm::LLMRole::System, inspector->systemRules},
                                {llm::LLMRole::User, inspector->userPrompt}};
    requestTemplate.responseFormat = {
        {QStringLiteral("type"), QStringLiteral("json_schema")},
        {QStringLiteral("json_schema"),
         QJsonObject{{QStringLiteral("name"), QStringLiteral("loreforge_context_output")},
                     {QStringLiteral("strict"), true},
                     {QStringLiteral("schema"), inspector->outputSchema}}},
    };
    return StoredContextRequest{std::move(snapshot), std::move(requestTemplate)};
}

} // namespace loreforge::context
