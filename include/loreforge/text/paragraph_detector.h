#pragma once

#include <QString>
#include <QStringList>
#include <QStringView>

namespace loreforge::text {

class ParagraphDetector final {
  public:
    [[nodiscard]] static bool isBlank(QStringView line);
    [[nodiscard]] static bool isSceneBreak(QStringView line);
    [[nodiscard]] static QString joinWrappedLines(const QStringList& lines);
};

} // namespace loreforge::text
