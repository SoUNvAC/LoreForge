#pragma once

#include "loreforge/narrative/narrative_types.h"

namespace loreforge::narrative {

struct ExtractionMetrics final {
    double segmentCoverage = 0.0;
    double boundaryCorrectness = 0.0;
    double speakerCorrectness = 0.0;
    double unknownSpeakerHandling = 0.0;

    friend bool operator==(const ExtractionMetrics&, const ExtractionMetrics&) = default;
};

class ExtractionEvaluator final {
  public:
    [[nodiscard]] static ExtractionMetrics compare(const ChapterSegmentation& expected,
                                                   const ChapterSegmentation& actual);
};

} // namespace loreforge::narrative
