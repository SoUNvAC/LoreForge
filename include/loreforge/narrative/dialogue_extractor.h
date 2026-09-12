#pragma once

#include "loreforge/narrative/narrative_types.h"

#include <QByteArrayView>
#include <QJsonObject>
#include <QList>
#include <QString>

#include <optional>

namespace loreforge::narrative {

enum class ExtractionErrorCode {
    InvalidChapter,
    InvalidSourceSpan,
    InvalidUtf8,
    SchemaViolation,
    ChapterMismatch,
    MissingSegments,
    InvalidConfidence,
    InvalidSpeaker,
    InvalidSegmentSpan,
    CoverageGap,
    CoverageOverlap,
};

struct ExtractionError final {
    ExtractionErrorCode code;
    QString path;
    QString message;

    friend bool operator==(const ExtractionError&, const ExtractionError&) = default;
};

struct DialogueExtractionResult final {
    std::optional<ChapterSegmentation> extraction;
    QList<ExtractionError> errors;

    [[nodiscard]] bool isValid() const noexcept;
};

class DialogueExtractor final {
  public:
    [[nodiscard]] static QJsonObject outputSchema();
    [[nodiscard]] static DialogueExtractionResult extract(const core::ChapterId& chapterId,
                                                          const core::SourceSpan& chapterSpan,
                                                          QByteArrayView sourceUtf8,
                                                          const QJsonObject& modelOutput);
};

} // namespace loreforge::narrative
