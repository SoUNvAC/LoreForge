#include "loreforge/text/chapter_heading_detector.h"

#include "loreforge/text/text_normalizer.h"

#include <QRegularExpression>

namespace loreforge::text {

std::optional<QString> ChapterHeadingDetector::detect(QStringView line) {
    const auto candidate = TextNormalizer::normalizeBlockLine(line);
    if (candidate.isEmpty()) {
        return std::nullopt;
    }

    static const QRegularExpression englishPattern(
        QStringLiteral(
            R"(^(?:(?:chapter|book|part)\s+(?:[0-9]+|[ivxlcdm]+|[a-z]+)|prologue|epilogue)(?:\s*[:.\-\x{2014}]\s*.+)?$)"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression chinesePattern(QStringLiteral(
        R"(^(?:\x{7b2c}[\x{96f6}\x{3007}\x{4e00}\x{4e8c}\x{4e09}\x{56db}\x{4e94}\x{516d}\x{4e03}\x{516b}\x{4e5d}\x{5341}\x{767e}\x{5343}\x{4e07}\x{4e24}0-9]+[\x{7ae0}\x{8282}\x{56de}\x{5377}\x{90e8}](?:(?:\s*[:\x{ff1a}.\-\x{2014}]\s*|\s+).+)?|\x{5e8f}\x{7ae0}|\x{6954}\x{5b50}|\x{5c3e}\x{58f0})$)"));

    if (englishPattern.matchView(candidate).hasMatch() ||
        chinesePattern.matchView(candidate).hasMatch()) {
        return candidate;
    }
    return std::nullopt;
}

} // namespace loreforge::text
