#include "loreforge/inference/inference_types.h"

#include <utility>

namespace loreforge::inference {

bool ValidationReport::isValid() const noexcept {
    return status == ValidationStatus::Valid && errors.isEmpty();
}

QByteArray canonicalJson(const QJsonObject& object) {
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

PromptVersion makePromptVersion(PromptTemplate prompt, int version, QString text,
                                QDateTime createdAt) {
    const auto contentHash = core::ContentHash::sha256(QStringView(text));
    return {std::move(prompt), version, std::move(text), contentHash, std::move(createdAt)};
}

OutputSchema makeOutputSchema(core::OutputSchemaId id, QString name, int version,
                              QJsonObject schema, QDateTime createdAt) {
    const auto contentHash = core::ContentHash::sha256(canonicalJson(schema));
    return {std::move(id),     std::move(name), version,
            std::move(schema), contentHash,     std::move(createdAt)};
}

ContextSnapshot makeContextSnapshot(core::ContextSnapshotId id, core::ProjectId projectId,
                                    QJsonObject content, QDateTime createdAt) {
    const auto contentHash = core::ContentHash::sha256(canonicalJson(content));
    return {std::move(id), std::move(projectId), std::move(content), contentHash,
            std::move(createdAt)};
}

} // namespace loreforge::inference
