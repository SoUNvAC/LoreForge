#pragma once

#include <QJsonObject>
#include <QStringList>

namespace loreforge::app {
// Network contracts are versioned separately from the byte-range domain schema.
inline constexpr int chapterPromptVersion = 3;
inline constexpr int chapterWireSchemaVersion = 2;
[[nodiscard]] QJsonObject blockReferenceSchema(const QJsonObject& byteRangeSchema);
struct ResolvedChapterEvidence final {
    QJsonObject output;
    QStringList errors;
};
[[nodiscard]] ResolvedChapterEvidence
resolveChapterEvidence(const QJsonObject& wireOutput, const QJsonObject& snapshot, bool review);
} // namespace loreforge::app
