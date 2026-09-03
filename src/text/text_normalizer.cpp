#include "loreforge/text/text_normalizer.h"

namespace loreforge::text {

QString TextNormalizer::normalize(QStringView text) {
    QString normalized = text.toString();
    if (normalized.startsWith(QChar::ByteOrderMark)) {
        normalized.removeFirst();
    }
    normalized.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    normalized.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    return normalized.normalized(QString::NormalizationForm_C);
}

QString TextNormalizer::normalizeBlockLine(QStringView line) {
    return normalize(line).trimmed();
}

} // namespace loreforge::text
