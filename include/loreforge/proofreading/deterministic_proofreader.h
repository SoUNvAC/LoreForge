#pragma once

#include "loreforge/proofreading/proofreading_types.h"

namespace loreforge::proofreading {

class DeterministicProofreader final {
  public:
    [[nodiscard]] static ProofreadingResult analyze(QList<ProofreadingSource> sources,
                                                    ProofreadingPolicy policy);
};

} // namespace loreforge::proofreading
