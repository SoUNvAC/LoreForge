#pragma once

#include <QString>
#include <QStringView>

namespace loreforge::text {

class TextNormalizer final {
  public:
    [[nodiscard]] static QString normalize(QStringView text);
    [[nodiscard]] static QString normalizeBlockLine(QStringView line);
};

} // namespace loreforge::text
