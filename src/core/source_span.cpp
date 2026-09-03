#include "loreforge/core/source_span.h"

namespace loreforge::core {

bool SourceSpan::isValid() const noexcept {
    return !sourceId.trimmed().isEmpty() && startByte >= 0 && endByte >= startByte;
}

qint64 SourceSpan::lengthBytes() const noexcept {
    return isValid() ? endByte - startByte : 0;
}

bool SourceSpan::contains(qint64 byteOffset) const noexcept {
    return isValid() && byteOffset >= startByte && byteOffset < endByte;
}

bool SourceSpan::overlaps(const SourceSpan& other) const noexcept {
    return isValid() && other.isValid() && sourceId == other.sourceId &&
           startByte < other.endByte && other.startByte < endByte;
}

} // namespace loreforge::core
