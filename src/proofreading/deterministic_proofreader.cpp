#include "loreforge/proofreading/deterministic_proofreader.h"

#include "loreforge/proofreading/protected_term_registry.h"
#include "proofreading_support.h"

#include <QHash>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <utility>

namespace loreforge::proofreading {
namespace {

constexpr auto kDetectorVersion = "deterministic-v1";

struct Token final {
    QString text;
    qsizetype start = 0;
    qsizetype end = 0;
};

QList<Token> wordTokens(QStringView text) {
    const QRegularExpression expression(QStringLiteral("[\\p{L}\\p{M}][\\p{L}\\p{M}\\p{N}_'’-]*"),
                                        QRegularExpression::UseUnicodePropertiesOption);
    QList<Token> result;
    auto matches = expression.globalMatchView(text);
    while (matches.hasNext()) {
        const auto match = matches.next();
        result.append({match.captured(), match.capturedStart(), match.capturedEnd()});
    }
    return result;
}

int editDistance(QStringView left, QStringView right) {
    const auto first = left.toString().toUcs4();
    const auto second = right.toString().toUcs4();
    QList<int> previous(second.size() + 1);
    for (qsizetype index = 0; index <= second.size(); ++index) {
        previous[index] = static_cast<int>(index);
    }
    for (qsizetype leftIndex = 0; leftIndex < first.size(); ++leftIndex) {
        QList<int> current(second.size() + 1);
        current[0] = static_cast<int>(leftIndex + 1);
        for (qsizetype rightIndex = 0; rightIndex < second.size(); ++rightIndex) {
            const auto substitution =
                previous[rightIndex] + (first.at(leftIndex) == second.at(rightIndex) ? 0 : 1);
            current[rightIndex + 1] =
                std::min({current[rightIndex] + 1, previous[rightIndex + 1] + 1, substitution});
        }
        previous = std::move(current);
    }
    return previous.last();
}

bool containsCjk(QStringView text) {
    const auto codePoints = text.toString().toUcs4();
    return std::any_of(codePoints.cbegin(), codePoints.cend(), [](char32_t codePoint) {
        return (codePoint >= 0x3400 && codePoint <= 0x4DBF) ||
               (codePoint >= 0x4E00 && codePoint <= 0x9FFF) ||
               (codePoint >= 0xF900 && codePoint <= 0xFAFF) ||
               (codePoint >= 0x3040 && codePoint <= 0x30FF) ||
               (codePoint >= 0xAC00 && codePoint <= 0xD7AF);
    });
}

void appendCandidate(QList<ProofreadingCandidate>& candidates,
                     const ProtectedTermRegistry& protectedTerms, const ProofreadingSource& source,
                     QStringView text, qsizetype start, qsizetype end, QString suggestedText,
                     CandidateCategory category, double confidence, QString evidence,
                     SemanticImpact impact) {
    const auto span = detail::absoluteSpan(source, text, start, end);
    if (protectedTerms.protects(source, span)) {
        return;
    }
    const auto original = text.sliced(start, end - start).toString();
    candidates.append(detail::makeCandidate(
        source, span, original, std::move(suggestedText), category, confidence, std::move(evidence),
        impact, CandidateOrigin::Deterministic, QString::fromLatin1(kDetectorVersion)));
}

void detectDuplicateTokens(const ProofreadingSource& source, QStringView text,
                           const ProtectedTermRegistry& protectedTerms,
                           QList<ProofreadingCandidate>& candidates) {
    const QRegularExpression expression(QStringLiteral("([\\p{L}\\p{M}\\p{N}_'’-]+)([ \\t]+)\\1"),
                                        QRegularExpression::UseUnicodePropertiesOption |
                                            QRegularExpression::CaseInsensitiveOption);
    auto matches = expression.globalMatchView(text);
    while (matches.hasNext()) {
        const auto match = matches.next();
        appendCandidate(candidates, protectedTerms, source, text, match.capturedStart(2),
                        match.capturedEnd(), {}, CandidateCategory::DuplicatedText, 0.82,
                        QStringLiteral("A token is repeated consecutively."),
                        SemanticImpact::TextOnly);
    }
}

void detectRepeatedLines(const ProofreadingSource& source, QStringView text,
                         const ProtectedTermRegistry& protectedTerms,
                         QList<ProofreadingCandidate>& candidates) {
    QString previous;
    qsizetype cursor = 0;
    while (cursor <= text.size()) {
        const auto newline = text.indexOf(QLatin1Char('\n'), cursor);
        const auto end = newline < 0 ? text.size() : newline;
        const auto line = text.sliced(cursor, end - cursor);
        const auto trimmed = line.trimmed();
        if (!trimmed.isEmpty() && trimmed == previous) {
            appendCandidate(candidates, protectedTerms, source, text, cursor, end, {},
                            CandidateCategory::DuplicatedText, 0.94,
                            QStringLiteral("This line repeats the immediately preceding line."),
                            SemanticImpact::TextOnly);
        }
        previous = trimmed.toString();
        if (newline < 0) {
            break;
        }
        cursor = newline + 1;
    }
}

void detectPattern(const ProofreadingSource& source, QStringView text,
                   const ProtectedTermRegistry& protectedTerms,
                   QList<ProofreadingCandidate>& candidates, const QRegularExpression& expression,
                   QString suggestedText, CandidateCategory category, double confidence,
                   QString evidence, SemanticImpact impact) {
    auto matches = expression.globalMatchView(text);
    while (matches.hasNext()) {
        const auto match = matches.next();
        appendCandidate(candidates, protectedTerms, source, text, match.capturedStart(),
                        match.capturedEnd(), suggestedText, category, confidence, evidence, impact);
    }
}

void detectSpacing(const ProofreadingSource& source, QStringView text,
                   const ProtectedTermRegistry& protectedTerms,
                   QList<ProofreadingCandidate>& candidates) {
    detectPattern(source, text, protectedTerms, candidates,
                  QRegularExpression(QStringLiteral("[ \\t]{2,}")), QStringLiteral(" "),
                  CandidateCategory::Spacing, 0.97,
                  QStringLiteral("Multiple horizontal whitespace characters occur together."),
                  SemanticImpact::TextOnly);
    detectPattern(source, text, protectedTerms, candidates,
                  QRegularExpression(QStringLiteral("[ \\t]+(?=[,.;:!?，。；：！？])")), {},
                  CandidateCategory::Spacing, 0.96,
                  QStringLiteral("Whitespace appears before punctuation."),
                  SemanticImpact::PunctuationOnly);
    detectPattern(source, text, protectedTerms, candidates,
                  QRegularExpression(QStringLiteral("(?<=[,;:!?])(?=[\\p{L}\\p{N}])"),
                                     QRegularExpression::UseUnicodePropertiesOption),
                  QStringLiteral(" "), CandidateCategory::Spacing, 0.78,
                  QStringLiteral("ASCII punctuation is immediately followed by a word."),
                  SemanticImpact::PunctuationOnly);
}

void detectPunctuation(const ProofreadingSource& source, QStringView text,
                       const ProtectedTermRegistry& protectedTerms,
                       QList<ProofreadingCandidate>& candidates) {
    const QRegularExpression repeated(QStringLiteral("([,;:!?，；：！？])\\1+"));
    auto repeatedMatches = repeated.globalMatchView(text);
    while (repeatedMatches.hasNext()) {
        const auto match = repeatedMatches.next();
        appendCandidate(candidates, protectedTerms, source, text, match.capturedStart(),
                        match.capturedEnd(), match.captured(1), CandidateCategory::Punctuation,
                        0.72, QStringLiteral("The same punctuation mark is repeated."),
                        SemanticImpact::PunctuationOnly);
    }
    detectPattern(source, text, protectedTerms, candidates,
                  QRegularExpression(QStringLiteral("(?<!\\.)\\.{2}(?!\\.)")),
                  QStringLiteral("..."), CandidateCategory::Punctuation, 0.84,
                  QStringLiteral("A two-dot sequence may be a damaged ellipsis."),
                  SemanticImpact::PunctuationOnly);

    struct Opening final {
        QChar character;
        qsizetype position;
    };
    const QHash<QChar, QChar> pairs{{QLatin1Char('('), QLatin1Char(')')},
                                    {QLatin1Char('['), QLatin1Char(']')},
                                    {QLatin1Char('{'), QLatin1Char('}')},
                                    {QChar(0xFF08), QChar(0xFF09)},
                                    {QChar(0x3010), QChar(0x3011)}};
    QHash<QChar, QChar> reversePairs;
    for (auto iterator = pairs.cbegin(); iterator != pairs.cend(); ++iterator) {
        reversePairs.insert(iterator.value(), iterator.key());
    }
    QList<Opening> stack;
    for (qsizetype index = 0; index < text.size(); ++index) {
        const auto character = text.at(index);
        if (pairs.contains(character)) {
            stack.append({character, index});
        } else if (reversePairs.contains(character)) {
            if (!stack.isEmpty() && stack.last().character == reversePairs.value(character)) {
                stack.removeLast();
            } else {
                appendCandidate(candidates, protectedTerms, source, text, index, index + 1, {},
                                CandidateCategory::Punctuation, 0.91,
                                QStringLiteral("A closing bracket has no matching opener."),
                                SemanticImpact::PunctuationOnly);
            }
        }
    }
    for (const auto& opening : stack) {
        appendCandidate(candidates, protectedTerms, source, text, opening.position,
                        opening.position + 1, {}, CandidateCategory::Punctuation, 0.91,
                        QStringLiteral("An opening bracket has no matching closer."),
                        SemanticImpact::PunctuationOnly);
    }
}

void detectTerminology(const ProofreadingSource& source, QStringView text,
                       const ProofreadingPolicy& policy,
                       const ProtectedTermRegistry& protectedTerms,
                       QList<ProofreadingCandidate>& candidates) {
    for (const auto& rule : policy.terminology) {
        for (const auto& variant : rule.knownVariants) {
            for (const auto& [start, end] : detail::literalMatches(
                     text, variant, rule.caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive)) {
                appendCandidate(candidates, protectedTerms, source, text, start, end,
                                rule.canonicalSpelling, CandidateCategory::TerminologyInconsistency,
                                0.99,
                                QStringLiteral("A known noncanonical term variant was found."),
                                SemanticImpact::TextOnly);
            }
        }
        if (rule.caseSensitive) {
            for (const auto& [start, end] :
                 detail::literalMatches(text, rule.canonicalSpelling, Qt::CaseInsensitive)) {
                if (text.sliced(start, end - start) != rule.canonicalSpelling) {
                    appendCandidate(candidates, protectedTerms, source, text, start, end,
                                    rule.canonicalSpelling,
                                    CandidateCategory::TerminologyInconsistency, 0.98,
                                    QStringLiteral("Canonical terminology uses inconsistent case."),
                                    SemanticImpact::TextOnly);
                }
            }
        }
    }
}

void detectNameVariants(const QList<ProofreadingSource>& sources, const ProofreadingPolicy& policy,
                        const QMap<QString, int>& frequencies,
                        const ProtectedTermRegistry& protectedTerms,
                        QList<ProofreadingCandidate>& candidates) {
    QHash<QString, int> tokenFrequency;
    QList<QPair<const ProofreadingSource*, QList<Token>>> tokensBySource;
    for (const auto& source : sources) {
        const auto decoded = detail::decodeSource(source);
        if (!decoded.has_value()) {
            continue;
        }
        auto tokens = wordTokens(*decoded);
        for (const auto& token : tokens) {
            ++tokenFrequency[token.text];
        }
        tokensBySource.append({&source, std::move(tokens)});
    }
    for (const auto& name : policy.knownNames) {
        if (frequencies.value(name) < policy.minimumNameOccurrences ||
            name.contains(QLatin1Char(' '))) {
            continue;
        }
        const auto foldedName = name.toCaseFolded();
        for (const auto& [source, tokens] : tokensBySource) {
            const auto decoded = *detail::decodeSource(*source);
            for (const auto& token : tokens) {
                if (token.text == name || tokenFrequency.value(token.text) > 1) {
                    continue;
                }
                const auto foldedToken = token.text.toCaseFolded();
                const auto distance = editDistance(foldedToken, foldedName);
                const bool plausibleShape =
                    (!token.text.isEmpty() && token.text.front().isUpper()) ||
                    (containsCjk(name) && containsCjk(token.text));
                if (plausibleShape && distance <= 1) {
                    appendCandidate(
                        candidates, protectedTerms, *source, decoded, token.start, token.end, name,
                        CandidateCategory::NameInconsistency, 0.74,
                        QStringLiteral("A rare spelling is close to a frequent known name."),
                        SemanticImpact::EntityChange);
                }
            }
        }
    }
}

} // namespace

ProofreadingResult DeterministicProofreader::analyze(QList<ProofreadingSource> sources,
                                                     ProofreadingPolicy policy) {
    ProofreadingResult result;
    detail::validatePolicy(policy, result.errors);
    QSet<QString> chapterIds;
    std::optional<core::ContentHash> sourceHash;
    for (qsizetype index = 0; index < sources.size(); ++index) {
        const auto path = QStringLiteral("$.sources[%1]").arg(index);
        detail::validateSource(sources.at(index), path, result.errors);
        for (qsizetype previous = 0; previous < index; ++previous) {
            if (sources.at(index).sourceSpan.overlaps(sources.at(previous).sourceSpan)) {
                result.errors.append(
                    {ProofreadingErrorCode::InvalidSource, path,
                     QStringLiteral("Chapter source spans in one analysis must not overlap.")});
                break;
            }
        }
        if (chapterIds.contains(sources.at(index).chapterId.toString())) {
            result.errors.append({ProofreadingErrorCode::InvalidSource, path,
                                  QStringLiteral("Chapter IDs in one analysis must be unique.")});
        }
        chapterIds.insert(sources.at(index).chapterId.toString());
        if (sourceHash.has_value() && *sourceHash != sources.at(index).sourceHash) {
            result.errors.append(
                {ProofreadingErrorCode::InvalidSource, path,
                 QStringLiteral("Sources in one analysis must share one source hash.")});
        } else {
            sourceHash = sources.at(index).sourceHash;
        }
    }
    if (sources.isEmpty()) {
        result.errors.append({ProofreadingErrorCode::InvalidSource, QStringLiteral("$.sources"),
                              QStringLiteral("At least one exact chapter source is required.")});
    }
    if (!result.errors.isEmpty()) {
        return result;
    }

    ProtectedTermRegistry protectedTerms(policy.protectedTerms);
    ProofreadingReport report;
    report.sourceHash = *sourceHash;
    for (const auto& name : policy.knownNames) {
        int count = 0;
        for (const auto& source : sources) {
            const auto decoded = *detail::decodeSource(source);
            count += detail::literalMatches(decoded, name, Qt::CaseSensitive).size();
        }
        report.nameFrequencies.insert(name, count);
    }

    for (const auto& source : sources) {
        const auto decoded = *detail::decodeSource(source);
        detectDuplicateTokens(source, decoded, protectedTerms, report.candidates);
        detectRepeatedLines(source, decoded, protectedTerms, report.candidates);
        detectSpacing(source, decoded, protectedTerms, report.candidates);
        detectPunctuation(source, decoded, protectedTerms, report.candidates);
        detectTerminology(source, decoded, policy, protectedTerms, report.candidates);
    }
    detectNameVariants(sources, policy, report.nameFrequencies, protectedTerms, report.candidates);
    std::sort(report.candidates.begin(), report.candidates.end(), detail::candidateLess);
    report.candidates.erase(
        std::unique(report.candidates.begin(), report.candidates.end(),
                    [](const auto& left, const auto& right) { return left.id == right.id; }),
        report.candidates.end());
    result.report = std::move(report);
    return result;
}

} // namespace loreforge::proofreading
