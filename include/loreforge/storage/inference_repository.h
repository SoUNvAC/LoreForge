#pragma once

#include "loreforge/inference/inference_types.h"
#include "loreforge/storage/storage_error.h"

namespace loreforge::storage {

class ProjectDatabase;

struct StoredInferenceSnapshot final {
    inference::PromptVersion promptVersion;
    inference::OutputSchema outputSchema;
    inference::ContextSnapshot contextSnapshot;
    inference::LLMRunArtifacts runArtifacts;

    friend bool operator==(const StoredInferenceSnapshot&,
                           const StoredInferenceSnapshot&) = default;
};

class InferenceRepository final {
  public:
    explicit InferenceRepository(ProjectDatabase& database);

    [[nodiscard]] StorageStatus savePromptVersion(const inference::PromptVersion& version);
    [[nodiscard]] StorageResult<inference::PromptVersion>
    findPromptVersion(const core::PromptTemplateId& templateId, int version) const;

    [[nodiscard]] StorageStatus saveOutputSchema(const inference::OutputSchema& schema);
    [[nodiscard]] StorageResult<inference::OutputSchema>
    findOutputSchema(const core::OutputSchemaId& schemaId, int version) const;

    [[nodiscard]] StorageStatus saveContextSnapshot(const inference::ContextSnapshot& snapshot);
    [[nodiscard]] StorageResult<inference::ContextSnapshot>
    findContextSnapshot(const core::ContextSnapshotId& snapshotId) const;

    [[nodiscard]] StorageStatus createRunArtifacts(const inference::LLMRunArtifacts& artifacts);
    [[nodiscard]] StorageStatus finalizeRunArtifacts(const core::LLMRunId& runId,
                                                     std::optional<QByteArray> rawResponse,
                                                     std::optional<QJsonDocument> parsedResponse,
                                                     inference::ValidationReport validation);
    [[nodiscard]] StorageResult<inference::LLMRunArtifacts>
    findRunArtifacts(const core::LLMRunId& runId) const;
    [[nodiscard]] StorageResult<StoredInferenceSnapshot>
    inspectRun(const core::LLMRunId& runId) const;

  private:
    ProjectDatabase& database_;
};

} // namespace loreforge::storage
