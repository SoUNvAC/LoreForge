#pragma once

#include "loreforge/proofreading/proofreading_types.h"

#include <QPair>

namespace loreforge::proofreading::detail {

[[nodiscard]] std::optional<QString> decodeSource(const ProofreadingSource& source);
void validateSource(const ProofreadingSource& source, QStringView path,
                    QList<ProofreadingError>& errors);
void validatePolicy(const ProofreadingPolicy& policy, QList<ProofreadingError>& errors);

[[nodiscard]] core::SourceSpan absoluteSpan(const ProofreadingSource& source,
                                            QStringView decodedText, qsizetype start,
                                            qsizetype end);
[[nodiscard]] std::optional<QString> sourceText(const ProofreadingSource& source,
                                                const core::SourceSpan& span);
[[nodiscard]] QList<QPair<qsizetype, qsizetype>> literalMatches(QStringView text, QStringView term,
                                                                Qt::CaseSensitivity sensitivity);
[[nodiscard]] ProofreadingCandidate makeCandidate(const ProofreadingSource& source,
                                                  core::SourceSpan span, QString originalText,
                                                  QString suggestedText, CandidateCategory category,
                                                  double confidence, QString evidence,
                                                  SemanticImpact semanticImpact,
                                                  CandidateOrigin origin, QString detectorVersion);
[[nodiscard]] bool candidateLess(const ProofreadingCandidate& left,
                                 const ProofreadingCandidate& right);

} // namespace loreforge::proofreading::detail
