#pragma once

#include "loreforge/core/identifier.h"
#include "loreforge/core/source_span.h"

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringView>

#include <optional>

namespace loreforge::narrative {

enum class SegmentType {
    Dialogue,
    Narration,
};

[[nodiscard]] QString segmentTypeToString(SegmentType type);
[[nodiscard]] std::optional<SegmentType> segmentTypeFromString(QStringView value);

struct NarrativeSegment final {
    SegmentType type = SegmentType::Narration;
    QString text;
    std::optional<QString> speaker;
    double confidence = 0.0;
    core::SourceSpan sourceSpan;

    friend bool operator==(const NarrativeSegment&, const NarrativeSegment&) = default;
};

struct ChapterSegmentation final {
    core::ChapterId chapterId;
    core::SourceSpan sourceSpan;
    QList<NarrativeSegment> segments;

    [[nodiscard]] QByteArray reconstructedSource() const;
    friend bool operator==(const ChapterSegmentation&, const ChapterSegmentation&) = default;
};

} // namespace loreforge::narrative
