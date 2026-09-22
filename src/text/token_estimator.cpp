#include "loreforge/text/token_estimator.h"

#include <QList>
#include <QString>

namespace loreforge::text {
namespace {

bool isCjk(char32_t codePoint) {
    return (codePoint >= 0x3400 && codePoint <= 0x4DBF) ||
           (codePoint >= 0x4E00 && codePoint <= 0x9FFF) ||
           (codePoint >= 0xF900 && codePoint <= 0xFAFF) ||
           (codePoint >= 0x20000 && codePoint <= 0x2EBEF) ||
           (codePoint >= 0x3040 && codePoint <= 0x30FF) ||
           (codePoint >= 0xAC00 && codePoint <= 0xD7AF);
}

qsizetype estimatedRun(qsizetype length) {
    return length == 0 ? 0 : (length + 3) / 4;
}

} // namespace

qsizetype TokenEstimator::estimate(QStringView text) {
    const auto codePoints = text.toString().toUcs4();
    qsizetype tokens = 0;
    qsizetype runLength = 0;
    const auto flushRun = [&tokens, &runLength] {
        tokens += estimatedRun(runLength);
        runLength = 0;
    };

    for (const auto codePoint : codePoints) {
        if (isCjk(codePoint)) {
            flushRun();
            ++tokens;
        } else if (QChar::isLetterOrNumber(codePoint) || codePoint == U'_' || codePoint == U'\'') {
            ++runLength;
        } else if (QChar::isSpace(codePoint)) {
            flushRun();
        } else {
            flushRun();
            ++tokens;
        }
    }
    flushRun();
    return tokens;
}

} // namespace loreforge::text
