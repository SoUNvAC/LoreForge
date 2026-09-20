#pragma once

#include "loreforge/narrative/chapter_analysis.h"

#include <QByteArrayView>
#include <QJsonObject>
#include <QList>
#include <QString>

#include <optional>

namespace loreforge::narrative {

enum class ChapterAnalysisErrorCode {
    InvalidChapter,
    InvalidSourceSpan,
    InvalidUtf8,
    SchemaViolation,
    ChapterMismatch,
    InvalidText,
    DuplicateName,
    InvalidEvidence,
    DuplicateEvidence,
    InvalidConfidence,
    UngroundedClaim,
};

struct ChapterAnalysisError final {
    ChapterAnalysisErrorCode code;
    QString path;
    QString message;

    friend bool operator==(const ChapterAnalysisError&, const ChapterAnalysisError&) = default;
};

struct ChapterAnalysisResult final {
    std::optional<ChapterAnalysis> analysis;
    QList<ChapterAnalysisError> errors;

    [[nodiscard]] bool isValid() const noexcept;
};

class ChapterAnalyzer final {
  public:
    [[nodiscard]] static QJsonObject outputSchema();
    [[nodiscard]] static ChapterAnalysisResult analyze(const core::ChapterId& chapterId,
                                                       const core::SourceSpan& chapterSpan,
                                                       QByteArrayView sourceUtf8,
                                                       const QJsonObject& modelOutput);
};

} // namespace loreforge::narrative
