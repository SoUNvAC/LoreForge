#include "loreforge/proofreading/proofreading_types.h"

namespace loreforge::proofreading {

QString candidateCategoryName(CandidateCategory category) {
    switch (category) {
    case CandidateCategory::Typo:
        return QStringLiteral("TYPO");
    case CandidateCategory::MissingWord:
        return QStringLiteral("MISSING_WORD");
    case CandidateCategory::ExtraWord:
        return QStringLiteral("EXTRA_WORD");
    case CandidateCategory::DuplicatedText:
        return QStringLiteral("DUPLICATED_TEXT");
    case CandidateCategory::Spacing:
        return QStringLiteral("SPACING");
    case CandidateCategory::Punctuation:
        return QStringLiteral("PUNCTUATION");
    case CandidateCategory::NameInconsistency:
        return QStringLiteral("NAME_INCONSISTENCY");
    case CandidateCategory::TerminologyInconsistency:
        return QStringLiteral("TERMINOLOGY_INCONSISTENCY");
    case CandidateCategory::PossibleSourceDamage:
        return QStringLiteral("POSSIBLE_SOURCE_DAMAGE");
    case CandidateCategory::Ambiguous:
        return QStringLiteral("AMBIGUOUS");
    }
    return {};
}

std::optional<CandidateCategory> candidateCategoryFromName(QStringView name) {
    for (const auto category :
         {CandidateCategory::Typo, CandidateCategory::MissingWord, CandidateCategory::ExtraWord,
          CandidateCategory::DuplicatedText, CandidateCategory::Spacing,
          CandidateCategory::Punctuation, CandidateCategory::NameInconsistency,
          CandidateCategory::TerminologyInconsistency, CandidateCategory::PossibleSourceDamage,
          CandidateCategory::Ambiguous}) {
        if (name == candidateCategoryName(category)) {
            return category;
        }
    }
    return std::nullopt;
}

QString semanticImpactName(SemanticImpact impact) {
    switch (impact) {
    case SemanticImpact::TextOnly:
        return QStringLiteral("TEXT_ONLY");
    case SemanticImpact::PunctuationOnly:
        return QStringLiteral("PUNCTUATION_ONLY");
    case SemanticImpact::EntityChange:
        return QStringLiteral("ENTITY_CHANGE");
    case SemanticImpact::DialogueChange:
        return QStringLiteral("DIALOGUE_CHANGE");
    case SemanticImpact::ActionChange:
        return QStringLiteral("ACTION_CHANGE");
    case SemanticImpact::TimelineChange:
        return QStringLiteral("TIMELINE_CHANGE");
    case SemanticImpact::StoryStateChange:
        return QStringLiteral("STORY_STATE_CHANGE");
    case SemanticImpact::Unknown:
        return QStringLiteral("UNKNOWN");
    }
    return {};
}

std::optional<SemanticImpact> semanticImpactFromName(QStringView name) {
    for (const auto impact : {SemanticImpact::TextOnly, SemanticImpact::PunctuationOnly,
                              SemanticImpact::EntityChange, SemanticImpact::DialogueChange,
                              SemanticImpact::ActionChange, SemanticImpact::TimelineChange,
                              SemanticImpact::StoryStateChange, SemanticImpact::Unknown}) {
        if (name == semanticImpactName(impact)) {
            return impact;
        }
    }
    return std::nullopt;
}

bool ProofreadingResult::isValid() const noexcept {
    return report.has_value() && errors.isEmpty();
}

} // namespace loreforge::proofreading
