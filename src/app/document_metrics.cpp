#include "document_metrics.h"

#include "loreforge/text/word_counter.h"

namespace loreforge::app {

ChapterMetrics DocumentMetrics::forChapter(const document::Chapter& chapter) {
    ChapterMetrics metrics;
    metrics.blockCount = chapter.blocks.size();
    for (const auto& block : chapter.blocks) {
        if (block.type == document::BlockType::Paragraph) {
            metrics.wordCount += text::WordCounter::count(block.text);
        }
        if (block.sourceSpan.isValid()) {
            ++metrics.sourceSpanCount;
        }
    }
    return metrics;
}

qsizetype DocumentMetrics::wordCount(const document::Document& document) {
    qsizetype words = 0;
    for (const auto& chapter : document.chapters) {
        words += forChapter(chapter).wordCount;
    }
    return words;
}

} // namespace loreforge::app
