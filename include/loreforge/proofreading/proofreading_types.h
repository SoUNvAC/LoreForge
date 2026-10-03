#pragma once

#include "loreforge/core/content_hash.h"
#include "loreforge/core/identifier.h"
#include "loreforge/core/source_span.h"

#include <QByteArray>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QStringView>

#include <optional>

namespace loreforge::proofreading {

enum class CandidateCategory {
    Typo,
    MissingWord,
    ExtraWord,
    DuplicatedText,
    Spacing,
    Punctuation,
    NameInconsistency,
    TerminologyInconsistency,
    PossibleSourceDamage,
    Ambiguous,
};

enum class SemanticImpact {
    TextOnly,
    PunctuationOnly,
    EntityChange,
    DialogueChange,
    ActionChange,
    TimelineChange,
    StoryStateChange,
    Unknown,
};

enum class CandidateOrigin {
    Deterministic,
    Semantic,
};

[[nodiscard]] QString candidateCategoryName(CandidateCategory category);
[[nodiscard]] std::optional<CandidateCategory> candidateCategoryFromName(QStringView name);
[[nodiscard]] QString semanticImpactName(SemanticImpact impact);
[[nodiscard]] std::optional<SemanticImpact> semanticImpactFromName(QStringView name);

struct ProofreadingSource final {
    core::ChapterId chapterId;
    core::SourceSpan sourceSpan;
    QByteArray utf8;
    core::ContentHash sourceHash;

    friend bool operator==(const ProofreadingSource&, const ProofreadingSource&) = default;
};

struct TerminologyRule final {
    QString canonicalSpelling;
    QStringList knownVariants;
    bool caseSensitive = true;

    friend bool operator==(const TerminologyRule&, const TerminologyRule&) = default;
};

struct ProtectedTerm final {
    QString canonicalSpelling;
    QStringList allowedVariants;
    QString notes;
    std::optional<core::ChapterId> chapterScope;

    friend bool operator==(const ProtectedTerm&, const ProtectedTerm&) = default;
};

struct ProofreadingPolicy final {
    QList<TerminologyRule> terminology;
    QList<ProtectedTerm> protectedTerms;
    QStringList knownNames;
    int minimumNameOccurrences = 2;

    friend bool operator==(const ProofreadingPolicy&, const ProofreadingPolicy&) = default;
};

struct ProofreadingCandidate final {
    core::ProofreadingCandidateId id;
    core::ChapterId chapterId;
    core::SourceSpan sourceSpan;
    QString originalText;
    QString suggestedText;
    CandidateCategory category = CandidateCategory::Ambiguous;
    double confidence = 0.0;
    QString evidence;
    SemanticImpact semanticImpact = SemanticImpact::Unknown;
    CandidateOrigin origin = CandidateOrigin::Deterministic;
    QString detectorVersion;
    core::ContentHash sourceHash;

    friend bool operator==(const ProofreadingCandidate&, const ProofreadingCandidate&) = default;
};

struct ProofreadingReport final {
    core::ContentHash sourceHash;
    QList<ProofreadingCandidate> candidates;
    QMap<QString, int> nameFrequencies;

    friend bool operator==(const ProofreadingReport&, const ProofreadingReport&) = default;
};

enum class ProofreadingErrorCode {
    InvalidSource,
    InvalidPolicy,
    SchemaViolation,
    ChapterMismatch,
    InvalidCandidate,
    ProtectedTerm,
    DuplicateCandidate,
};

struct ProofreadingError final {
    ProofreadingErrorCode code;
    QString path;
    QString message;

    friend bool operator==(const ProofreadingError&, const ProofreadingError&) = default;
};

struct ProofreadingResult final {
    std::optional<ProofreadingReport> report;
    QList<ProofreadingError> errors;

    [[nodiscard]] bool isValid() const noexcept;
};

} // namespace loreforge::proofreading
