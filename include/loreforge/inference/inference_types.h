#pragma once

#include "loreforge/core/content_hash.h"
#include "loreforge/core/identifier.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <optional>

namespace loreforge::inference {

struct PromptTemplate final {
    core::PromptTemplateId id;
    QString name;

    friend bool operator==(const PromptTemplate&, const PromptTemplate&) = default;
};

struct PromptVersion final {
    PromptTemplate prompt;
    int version = 0;
    QString text;
    core::ContentHash contentHash;
    QDateTime createdAt;

    friend bool operator==(const PromptVersion&, const PromptVersion&) = default;
};

struct OutputSchema final {
    core::OutputSchemaId id;
    QString name;
    int version = 0;
    QJsonObject schema;
    core::ContentHash contentHash;
    QDateTime createdAt;

    friend bool operator==(const OutputSchema&, const OutputSchema&) = default;
};

struct ContextSnapshot final {
    core::ContextSnapshotId id;
    core::ProjectId projectId;
    QJsonObject content;
    core::ContentHash contentHash;
    QDateTime createdAt;

    friend bool operator==(const ContextSnapshot&, const ContextSnapshot&) = default;
};

enum class ValidationStatus {
    Pending,
    Valid,
    Invalid,
    Unavailable,
};

struct ValidationReport final {
    ValidationStatus status = ValidationStatus::Pending;
    QStringList errors;

    [[nodiscard]] bool isValid() const noexcept;
    friend bool operator==(const ValidationReport&, const ValidationReport&) = default;
};

struct LLMRunArtifacts final {
    core::LLMRunId runId;
    core::PromptTemplateId promptTemplateId;
    int promptVersion = 0;
    core::OutputSchemaId outputSchemaId;
    int outputSchemaVersion = 0;
    core::ContextSnapshotId contextSnapshotId;
    QByteArray rawRequest;
    std::optional<QByteArray> rawResponse;
    std::optional<QJsonDocument> parsedResponse;
    ValidationReport validation;

    friend bool operator==(const LLMRunArtifacts&, const LLMRunArtifacts&) = default;
};

[[nodiscard]] QByteArray canonicalJson(const QJsonObject& object);
[[nodiscard]] PromptVersion makePromptVersion(PromptTemplate prompt, int version, QString text,
                                              QDateTime createdAt);
[[nodiscard]] OutputSchema makeOutputSchema(core::OutputSchemaId id, QString name, int version,
                                            QJsonObject schema, QDateTime createdAt);
[[nodiscard]] ContextSnapshot makeContextSnapshot(core::ContextSnapshotId id,
                                                  core::ProjectId projectId, QJsonObject content,
                                                  QDateTime createdAt);

} // namespace loreforge::inference
