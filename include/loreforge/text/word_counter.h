#pragma once

#include <QStringView>

namespace loreforge::text {

class WordCounter final {
  public:
    [[nodiscard]] static qsizetype count(QStringView text);
};

} // namespace loreforge::text
