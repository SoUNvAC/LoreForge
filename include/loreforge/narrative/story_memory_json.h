#pragma once

#include "loreforge/narrative/story_memory.h"

#include <QByteArray>
#include <QByteArrayView>
#include <QString>

#include <optional>

namespace loreforge::narrative {

struct ChapterMemoryDecodeResult final {
    std::optional<ChapterAnalysis> analysis;
    QString error;

    [[nodiscard]] bool isValid() const noexcept;
};

[[nodiscard]] QByteArray encodeChapterAnalysis(const ChapterAnalysis& analysis);
[[nodiscard]] ChapterMemoryDecodeResult decodeChapterAnalysis(QByteArrayView json);
[[nodiscard]] QByteArray encodeStoryStateSnapshot(const StoryStateSnapshot& snapshot);

} // namespace loreforge::narrative
