#pragma once

#include <QStringView>

namespace loreforge::text {

class TokenEstimator final {
  public:
    [[nodiscard]] static qsizetype estimate(QStringView text);
};

} // namespace loreforge::text
