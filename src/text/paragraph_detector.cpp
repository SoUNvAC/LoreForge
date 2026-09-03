#include "loreforge/text/paragraph_detector.h"

#include "loreforge/text/text_normalizer.h"

#include <QRegularExpression>

namespace loreforge::text {

bool ParagraphDetector::isBlank(QStringView line) {
    return line.trimmed().isEmpty();
}

bool ParagraphDetector::isSceneBreak(QStringView line) {
    static const QRegularExpression sceneBreakPattern(
        QStringLiteral(R"(^\s*(?:\*\s*\*\s*\*|-\s*-\s*-|#\s*#\s*#)\s*$)"));
    return sceneBreakPattern.matchView(line).hasMatch();
}

QString ParagraphDetector::joinWrappedLines(const QStringList& lines) {
    QStringList normalizedLines;
    normalizedLines.reserve(lines.size());
    for (const auto& line : lines) {
        const auto normalized = TextNormalizer::normalizeBlockLine(line);
        if (!normalized.isEmpty()) {
            normalizedLines.append(normalized);
        }
    }
    return normalizedLines.join(QLatin1Char(' '));
}

} // namespace loreforge::text
