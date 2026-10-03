#include "proofreading_support.h"

#include <QRegularExpression>
#include <QSet>
#include <QStringDecoder>

#include <algorithm>
#include <cmath>
#include <utility>

namespace loreforge::proofreading::detail {
namespace {

void addError(QList<ProofreadingError>& errors, ProofreadingErrorCode code, QString path,
              QString message) {
    errors.append({code, std::move(path), std::move(message)});
}

bool containsCjk(QStringView text) {
    const auto codePoints = text.toString().toUcs4();
    return std::any_of(codePoints.cbegin(), codePoints.cend(), [](char32_t codePoint) {
        return (codePoint >= 0x3400 && codePoint <= 0x4DBF) ||
               (codePoint >= 0x4E00 && codePoint <= 0x9FFF) ||
               (codePoint >= 0xF900 && codePoint <= 0xFAFF) ||
               (codePoint >= 0x20000 && codePoint <= 0x2EBEF) ||
               (codePoint >= 0x3040 && codePoint <= 0x30FF) ||
               (codePoint >= 0xAC00 && codePoint <= 0xD7AF);
    });
}

bool validTerm(QStringView value) {
    return !value.isEmpty() && value == value.trimmed();
}

} // namespace

std::optional<QString> decodeSource(const ProofreadingSource& source) {
    QStringDecoder decoder(QStringDecoder::Utf8);
    const QString text = decoder.decode(source.utf8);
    if (decoder.hasError() || text.toUtf8() != source.utf8) {
        return std::nullopt;
    }
    return text;
}

void validateSource(const ProofreadingSource& source, QStringView path,
                    QList<ProofreadingError>& errors) {
    if (!source.chapterId.isValid() || !source.sourceSpan.isValid() ||
        source.sourceSpan.lengthBytes() != source.utf8.size() || !source.sourceHash.isValid() ||
        !decodeSource(source).has_value()) {
        addError(errors, ProofreadingErrorCode::InvalidSource, path.toString(),
                 QStringLiteral("The chapter ID, exact UTF-8 source span, bytes, and source hash "
                                "must be valid."));
    }
}

void validatePolicy(const ProofreadingPolicy& policy, QList<ProofreadingError>& errors) {
    if (policy.minimumNameOccurrences < 1) {
        addError(errors, ProofreadingErrorCode::InvalidPolicy,
                 QStringLiteral("$.policy.minimum_name_occurrences"),
                 QStringLiteral("The minimum name occurrence count must be positive."));
    }
    QSet<QString> terminology;
    for (qsizetype index = 0; index < policy.terminology.size(); ++index) {
        const auto& rule = policy.terminology.at(index);
        const auto path = QStringLiteral("$.policy.terminology[%1]").arg(index);
        const auto canonicalKey =
            rule.canonicalSpelling.normalized(QString::NormalizationForm_C).toCaseFolded();
        if (!validTerm(rule.canonicalSpelling) || terminology.contains(canonicalKey)) {
            addError(errors, ProofreadingErrorCode::InvalidPolicy, path,
                     QStringLiteral("Canonical terminology must be trimmed and unique."));
        }
        terminology.insert(canonicalKey);
        QSet<QString> variants{canonicalKey};
        for (const auto& variant : rule.knownVariants) {
            const auto key = variant.normalized(QString::NormalizationForm_C).toCaseFolded();
            if (!validTerm(variant) || variants.contains(key) || terminology.contains(key)) {
                addError(
                    errors, ProofreadingErrorCode::InvalidPolicy, path,
                    QStringLiteral("Terminology spellings must be trimmed and globally unique."));
            }
            variants.insert(key);
            terminology.insert(key);
        }
    }
    QSet<QString> protectedKeys;
    for (qsizetype index = 0; index < policy.protectedTerms.size(); ++index) {
        const auto& term = policy.protectedTerms.at(index);
        const auto path = QStringLiteral("$.policy.protected_terms[%1]").arg(index);
        const auto scope =
            term.chapterScope.has_value() ? term.chapterScope->toString() : QString{};
        const auto canonicalKey =
            scope + QLatin1Char(':') +
            term.canonicalSpelling.normalized(QString::NormalizationForm_C).toCaseFolded();
        if (!validTerm(term.canonicalSpelling) || protectedKeys.contains(canonicalKey) ||
            (term.chapterScope.has_value() && !term.chapterScope->isValid())) {
            addError(errors, ProofreadingErrorCode::InvalidPolicy, path,
                     QStringLiteral("Protected terms and optional chapter scopes must be valid."));
        }
        protectedKeys.insert(canonicalKey);
        QSet<QString> variants{
            term.canonicalSpelling.normalized(QString::NormalizationForm_C).toCaseFolded()};
        for (const auto& variant : term.allowedVariants) {
            const auto key = variant.normalized(QString::NormalizationForm_C).toCaseFolded();
            if (!validTerm(variant) || variants.contains(key)) {
                addError(errors, ProofreadingErrorCode::InvalidPolicy, path,
                         QStringLiteral("Protected variants must be trimmed and unique."));
            }
            variants.insert(key);
        }
    }
    QSet<QString> names;
    for (qsizetype index = 0; index < policy.knownNames.size(); ++index) {
        const auto& name = policy.knownNames.at(index);
        const auto key = name.normalized(QString::NormalizationForm_C).toCaseFolded();
        if (!validTerm(name) || names.contains(key)) {
            addError(errors, ProofreadingErrorCode::InvalidPolicy,
                     QStringLiteral("$.policy.known_names[%1]").arg(index),
                     QStringLiteral("Known names must be trimmed and unique."));
        }
        names.insert(key);
    }
}

core::SourceSpan absoluteSpan(const ProofreadingSource& source, QStringView decodedText,
                              qsizetype start, qsizetype end) {
    const auto prefixBytes = decodedText.first(start).toString().toUtf8().size();
    const auto selectedBytes = decodedText.sliced(start, end - start).toString().toUtf8().size();
    return {source.sourceSpan.sourceId, source.sourceSpan.startByte + prefixBytes,
            source.sourceSpan.startByte + prefixBytes + selectedBytes};
}

std::optional<QString> sourceText(const ProofreadingSource& source, const core::SourceSpan& span) {
    if (!span.isValid() || span.sourceId != source.sourceSpan.sourceId ||
        span.startByte < source.sourceSpan.startByte || span.endByte > source.sourceSpan.endByte) {
        return std::nullopt;
    }
    const auto relativeStart = span.startByte - source.sourceSpan.startByte;
    const auto length = span.endByte - span.startByte;
    QStringDecoder decoder(QStringDecoder::Utf8);
    const QString text = decoder.decode(QByteArrayView(source.utf8).sliced(relativeStart, length));
    if (decoder.hasError() || text.toUtf8().size() != length) {
        return std::nullopt;
    }
    return text;
}

QList<QPair<qsizetype, qsizetype>> literalMatches(QStringView text, QStringView term,
                                                  Qt::CaseSensitivity sensitivity) {
    if (term.isEmpty()) {
        return {};
    }
    auto pattern = QRegularExpression::escape(term.toString());
    if (!containsCjk(term)) {
        pattern = QStringLiteral("(?<![\\p{L}\\p{M}\\p{N}_])(?:%1)(?![\\p{L}\\p{M}\\p{N}_])")
                      .arg(pattern);
    }
    QRegularExpression::PatternOptions options = QRegularExpression::UseUnicodePropertiesOption;
    if (sensitivity == Qt::CaseInsensitive) {
        options |= QRegularExpression::CaseInsensitiveOption;
    }
    const QRegularExpression expression(pattern, options);
    QList<QPair<qsizetype, qsizetype>> result;
    auto iterator = expression.globalMatchView(text);
    while (iterator.hasNext()) {
        const auto match = iterator.next();
        result.append({match.capturedStart(), match.capturedEnd()});
    }
    return result;
}

ProofreadingCandidate makeCandidate(const ProofreadingSource& source, core::SourceSpan span,
                                    QString originalText, QString suggestedText,
                                    CandidateCategory category, double confidence, QString evidence,
                                    SemanticImpact semanticImpact, CandidateOrigin origin,
                                    QString detectorVersion) {
    const auto stableKey = source.sourceHash.toHex() + QLatin1Char(':') +
                           source.chapterId.toString() + QLatin1Char(':') + span.sourceId +
                           QLatin1Char(':') + QString::number(span.startByte) + QLatin1Char(':') +
                           QString::number(span.endByte) + QLatin1Char(':') +
                           candidateCategoryName(category) + QLatin1Char(':') + suggestedText +
                           QLatin1Char(':') + detectorVersion;
    return {core::ProofreadingCandidateId::fromStableKey(stableKey),
            source.chapterId,
            std::move(span),
            std::move(originalText),
            std::move(suggestedText),
            category,
            confidence,
            std::move(evidence),
            semanticImpact,
            origin,
            std::move(detectorVersion),
            source.sourceHash};
}

bool candidateLess(const ProofreadingCandidate& left, const ProofreadingCandidate& right) {
    if (left.sourceSpan.sourceId != right.sourceSpan.sourceId) {
        return left.sourceSpan.sourceId < right.sourceSpan.sourceId;
    }
    if (left.sourceSpan.startByte != right.sourceSpan.startByte) {
        return left.sourceSpan.startByte < right.sourceSpan.startByte;
    }
    if (left.sourceSpan.endByte != right.sourceSpan.endByte) {
        return left.sourceSpan.endByte < right.sourceSpan.endByte;
    }
    if (left.category != right.category) {
        return left.category < right.category;
    }
    return left.id.toString() < right.id.toString();
}

} // namespace loreforge::proofreading::detail
