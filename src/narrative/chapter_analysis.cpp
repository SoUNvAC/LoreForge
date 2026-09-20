#include "loreforge/narrative/chapter_analysis.h"

namespace loreforge::narrative {

bool ClaimSupport::isGrounded() const noexcept {
    return basis == ClaimBasis::Inference || !evidence.isEmpty();
}

} // namespace loreforge::narrative
