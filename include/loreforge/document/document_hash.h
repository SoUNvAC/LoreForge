#pragma once

#include "loreforge/core/content_hash.h"
#include "loreforge/document/document.h"

namespace loreforge::document {

[[nodiscard]] core::ContentHash computeContentHash(const Document& document);

} // namespace loreforge::document
