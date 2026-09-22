#pragma once

#include "loreforge/context/context_types.h"

namespace loreforge::context {

class ContextBuilder final {
  public:
    [[nodiscard]] static ContextBuildResult build(ContextBuildInput input, ContextBudget budget,
                                                  QDateTime createdAt);
};

} // namespace loreforge::context
