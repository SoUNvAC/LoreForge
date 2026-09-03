#include "loreforge/text/word_counter.h"

#include <QList>
#include <QString>

namespace loreforge::text {
namespace {

bool isCjkIdeograph(char32_t codePoint) {
    return (codePoint >= 0x3400 && codePoint <= 0x4DBF) ||
           (codePoint >= 0x4E00 && codePoint <= 0x9FFF) ||
           (codePoint >= 0x20000 && codePoint <= 0x2EBEF);
}

bool isApostrophe(char32_t codePoint) {
    return codePoint == U'\'' || codePoint == 0x2019;
}

} // namespace

qsizetype WordCounter::count(QStringView text) {
    const auto codePoints = text.toString().toUcs4();
    qsizetype words = 0;
    bool inWord = false;

    for (qsizetype index = 0; index < codePoints.size(); ++index) {
        const auto codePoint = codePoints.at(index);
        if (isCjkIdeograph(codePoint)) {
            ++words;
            inWord = false;
            continue;
        }

        if (QChar::isLetterOrNumber(codePoint)) {
            if (!inWord) {
                ++words;
                inWord = true;
            }
            continue;
        }

        const auto category = QChar::category(codePoint);
        const bool combiningMark = category == QChar::Mark_NonSpacing ||
                                   category == QChar::Mark_SpacingCombining ||
                                   category == QChar::Mark_Enclosing;
        if (combiningMark && inWord) {
            continue;
        }

        const bool followedByWordCharacter =
            index + 1 < codePoints.size() && QChar::isLetterOrNumber(codePoints.at(index + 1));
        if (!isApostrophe(codePoint) || !inWord || !followedByWordCharacter) {
            inWord = false;
        }
    }

    return words;
}

} // namespace loreforge::text
