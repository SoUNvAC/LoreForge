#pragma once

#include "loreforge/document/document.h"

namespace loreforge::app {

struct ChapterMetrics final {
    qsizetype wordCount = 0;
    qsizetype blockCount = 0;
    qsizetype sourceSpanCount = 0;

    friend bool operator==(const ChapterMetrics&, const ChapterMetrics&) = default;
};

class DocumentMetrics final {
  public:
    [[nodiscard]] static ChapterMetrics forChapter(const document::Chapter& chapter);
    [[nodiscard]] static qsizetype wordCount(const document::Document& document);
};

} // namespace loreforge::app
