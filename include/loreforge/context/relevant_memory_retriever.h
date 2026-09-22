#pragma once

#include "loreforge/context/context_types.h"

#include <QStringView>

namespace loreforge::context {

class RelevantMemoryRetriever final {
  public:
    [[nodiscard]] static QList<MemoryCandidate>
    rank(const narrative::StoryStateSnapshot& storyState, QStringView currentChapter,
         QStringView taskInstructions);
};

} // namespace loreforge::context
