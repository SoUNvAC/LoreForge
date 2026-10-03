#pragma once

#include "loreforge/proofreading/proofreading_types.h"

namespace loreforge::proofreading {

class ProtectedTermRegistry final {
  public:
    explicit ProtectedTermRegistry(QList<ProtectedTerm> terms);

    [[nodiscard]] QList<core::SourceSpan> protectedSpans(const ProofreadingSource& source) const;
    [[nodiscard]] bool protects(const ProofreadingSource& source,
                                const core::SourceSpan& candidateSpan) const;

  private:
    QList<ProtectedTerm> terms_;
};

} // namespace loreforge::proofreading
