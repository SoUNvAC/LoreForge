#pragma once

#include "loreforge/proofreading/proofreading_types.h"

#include <QJsonObject>

namespace loreforge::proofreading {

class SemanticProofreader final {
  public:
    [[nodiscard]] static QJsonObject outputSchema();
    [[nodiscard]] static ProofreadingResult extract(const ProofreadingSource& source,
                                                    const QJsonObject& modelOutput,
                                                    const ProofreadingPolicy& policy);
};

} // namespace loreforge::proofreading
