#include "loreforge/narrative/extraction_evaluator.h"

#include <QList>
#include <QSet>

#include <algorithm>

namespace loreforge::narrative {
namespace {

double coveredFraction(const ChapterSegmentation& expected, const ChapterSegmentation& actual) {
    const auto total = expected.sourceSpan.lengthBytes();
    if (total == 0) {
        return 1.0;
    }
    QList<QPair<qint64, qint64>> intervals;
    for (const auto& segment : actual.segments) {
        if (segment.sourceSpan.sourceId != expected.sourceSpan.sourceId) {
            continue;
        }
        const auto start = qMax(segment.sourceSpan.startByte, expected.sourceSpan.startByte);
        const auto end = qMin(segment.sourceSpan.endByte, expected.sourceSpan.endByte);
        if (end > start) {
            intervals.append({start, end});
        }
    }
    std::sort(intervals.begin(), intervals.end());
    qint64 covered = 0;
    qint64 rangeStart = -1;
    qint64 rangeEnd = -1;
    for (const auto& interval : intervals) {
        if (rangeStart < 0) {
            rangeStart = interval.first;
            rangeEnd = interval.second;
        } else if (interval.first > rangeEnd) {
            covered += rangeEnd - rangeStart;
            rangeStart = interval.first;
            rangeEnd = interval.second;
        } else {
            rangeEnd = qMax(rangeEnd, interval.second);
        }
    }
    if (rangeStart >= 0) {
        covered += rangeEnd - rangeStart;
    }
    return static_cast<double>(covered) / static_cast<double>(total);
}

QSet<qint64> internalBoundaries(const ChapterSegmentation& segmentation) {
    QSet<qint64> boundaries;
    for (const auto& segment : segmentation.segments) {
        if (segment.sourceSpan.startByte > segmentation.sourceSpan.startByte &&
            segment.sourceSpan.startByte < segmentation.sourceSpan.endByte) {
            boundaries.insert(segment.sourceSpan.startByte);
        }
        if (segment.sourceSpan.endByte > segmentation.sourceSpan.startByte &&
            segment.sourceSpan.endByte < segmentation.sourceSpan.endByte) {
            boundaries.insert(segment.sourceSpan.endByte);
        }
    }
    return boundaries;
}

double boundaryF1(const ChapterSegmentation& expected, const ChapterSegmentation& actual) {
    const auto expectedBoundaries = internalBoundaries(expected);
    const auto actualBoundaries = internalBoundaries(actual);
    if (expectedBoundaries.isEmpty() && actualBoundaries.isEmpty()) {
        return 1.0;
    }
    qsizetype matches = 0;
    for (const auto boundary : expectedBoundaries) {
        if (actualBoundaries.contains(boundary)) {
            ++matches;
        }
    }
    return (2.0 * static_cast<double>(matches)) /
           static_cast<double>(expectedBoundaries.size() + actualBoundaries.size());
}

const NarrativeSegment* matchingDialogue(const ChapterSegmentation& actual,
                                         const NarrativeSegment& expected) {
    for (const auto& segment : actual.segments) {
        if (segment.type == SegmentType::Dialogue && segment.sourceSpan == expected.sourceSpan) {
            return &segment;
        }
    }
    return nullptr;
}

QPair<double, double> speakerScores(const ChapterSegmentation& expected,
                                    const ChapterSegmentation& actual) {
    qsizetype dialogueCount = 0;
    qsizetype speakerMatches = 0;
    qsizetype unknownCount = 0;
    qsizetype unknownMatches = 0;
    for (const auto& expectedSegment : expected.segments) {
        if (expectedSegment.type != SegmentType::Dialogue) {
            continue;
        }
        ++dialogueCount;
        const auto* actualSegment = matchingDialogue(actual, expectedSegment);
        if (actualSegment != nullptr && actualSegment->speaker == expectedSegment.speaker) {
            ++speakerMatches;
        }
        if (!expectedSegment.speaker.has_value()) {
            ++unknownCount;
            if (actualSegment != nullptr && !actualSegment->speaker.has_value()) {
                ++unknownMatches;
            }
        }
    }
    const auto speakerAccuracy = dialogueCount == 0 ? 1.0
                                                    : static_cast<double>(speakerMatches) /
                                                          static_cast<double>(dialogueCount);
    const auto unknownAccuracy =
        unknownCount == 0 ? 1.0
                          : static_cast<double>(unknownMatches) / static_cast<double>(unknownCount);
    return {speakerAccuracy, unknownAccuracy};
}

} // namespace

ExtractionMetrics ExtractionEvaluator::compare(const ChapterSegmentation& expected,
                                               const ChapterSegmentation& actual) {
    if (expected.chapterId != actual.chapterId || expected.sourceSpan != actual.sourceSpan) {
        return {};
    }
    const auto [speakerCorrectness, unknownSpeakerHandling] = speakerScores(expected, actual);
    return {
        coveredFraction(expected, actual),
        boundaryF1(expected, actual),
        speakerCorrectness,
        unknownSpeakerHandling,
    };
}

} // namespace loreforge::narrative
