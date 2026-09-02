#pragma once

#include <QString>

namespace loreforge::core {

class BuildInfo final {
  public:
    [[nodiscard]] static QString version();
};

} // namespace loreforge::core
