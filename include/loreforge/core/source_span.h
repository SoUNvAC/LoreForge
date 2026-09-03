#pragma once

#include <QString>

namespace loreforge::core {

struct SourceSpan final {
    QString sourceId;
    qint64 startByte = -1;
    qint64 endByte = -1;

    [[nodiscard]] bool isValid() const noexcept;
    [[nodiscard]] qint64 lengthBytes() const noexcept;
    [[nodiscard]] bool contains(qint64 byteOffset) const noexcept;
    [[nodiscard]] bool overlaps(const SourceSpan& other) const noexcept;

    friend bool operator==(const SourceSpan&, const SourceSpan&) = default;
};

} // namespace loreforge::core
