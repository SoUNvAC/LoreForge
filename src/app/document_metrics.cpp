#include "document_metrics.h"

#include "loreforge/text/word_counter.h"

namespace loreforge::app {

ChapterMetrics DocumentMetrics::forChapter(const document::Chapter& chapter) {
    ChapterMetrics metrics;
    metrics.blockCount = chapter.blocks.size();
    for (const auto& block : chapter.blocks) {
        if (block.type == document::BlockType::Paragraph) {
            const auto counts = text::WordCounter::countCharacters(block.text);
            metrics.characterCount += counts.total;
            metrics.hanCharacterCount += counts.han;
        }
        if (block.sourceSpan.isValid()) {
            ++metrics.sourceSpanCount;
        }
    }
    return metrics;
}

ChapterMetrics DocumentMetrics::forDocument(const document::Document& document) {
    ChapterMetrics totals;
    for (const auto& chapter : document.chapters) {
        const auto metrics = forChapter(chapter);
        totals.characterCount += metrics.characterCount;
        totals.hanCharacterCount += metrics.hanCharacterCount;
        totals.blockCount += metrics.blockCount;
        totals.sourceSpanCount += metrics.sourceSpanCount;
    }
    return totals;
}

} // namespace loreforge::app
