#pragma once

#include <QStringView>

namespace loreforge::text {

struct CharacterCounts final {
    qsizetype total = 0;
    qsizetype han = 0;
};

class WordCounter final {
  public:
    [[nodiscard]] static qsizetype count(QStringView text);
    // Unicode letters/numbers, excluding punctuation, symbols, whitespace and combining marks.
    // Han is the subset with Unicode Script=Han. Each supplementary character counts once.
    [[nodiscard]] static CharacterCounts countCharacters(QStringView text);
};

} // namespace loreforge::text
