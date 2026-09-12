#include "loreforge/narrative/narrative_types.h"

namespace loreforge::narrative {

QString segmentTypeToString(SegmentType type) {
    switch (type) {
    case SegmentType::Dialogue:
        return QStringLiteral("dialogue");
    case SegmentType::Narration:
        return QStringLiteral("narration");
    }
    return {};
}

std::optional<SegmentType> segmentTypeFromString(QStringView value) {
    if (value == QStringLiteral("dialogue")) {
        return SegmentType::Dialogue;
    }
    if (value == QStringLiteral("narration")) {
        return SegmentType::Narration;
    }
    return std::nullopt;
}

QByteArray ChapterSegmentation::reconstructedSource() const {
    QByteArray source;
    for (const auto& segment : segments) {
        source.append(segment.text.toUtf8());
    }
    return source;
}

} // namespace loreforge::narrative
