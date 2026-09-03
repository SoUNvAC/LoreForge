#pragma once

#include <QString>
#include <QStringView>

#include <optional>

namespace loreforge::text {

class ChapterHeadingDetector final {
  public:
    [[nodiscard]] static std::optional<QString> detect(QStringView line);
};

} // namespace loreforge::text
