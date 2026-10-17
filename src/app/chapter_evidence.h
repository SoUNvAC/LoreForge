#pragma once

#include <QJsonObject>
#include <QStringList>

namespace loreforge::app {
// Network contracts are versioned separately from the byte-range domain schema.
inline constexpr int chapterPromptVersion = 8;
inline constexpr int chapterWireSchemaVersion = 6;
[[nodiscard]] QJsonObject blockReferenceSchema(const QJsonObject& byteRangeSchema);
[[nodiscard]] QJsonObject chapterAnalysisWireSchema(const QJsonObject& byteRangeSchema);
[[nodiscard]] QJsonObject narrativeOutput(QJsonObject normalized);
// Outermost balanced quotation ranges, in QString UTF-16 positions. Not speaker inference.
[[nodiscard]] QList<QPair<qsizetype, qsizetype>> quotationRanges(const QString& text);
struct ResolvedChapterEvidence final {
    QJsonObject output;
    QStringList errors;
};
[[nodiscard]] ResolvedChapterEvidence
resolveChapterEvidence(const QJsonObject& wireOutput, const QJsonObject& snapshot, bool review);
} // namespace loreforge::app
