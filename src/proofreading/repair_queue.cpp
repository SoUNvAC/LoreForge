#include "loreforge/proofreading/repair_queue.h"

#include <utility>

namespace loreforge::proofreading {
namespace {

bool validTime(const QDateTime& time) {
    return time.isValid();
}

bool validCandidate(const ProofreadingCandidate& candidate) {
    return candidate.id.isValid() && candidate.chapterId.isValid() &&
           candidate.sourceSpan.isValid() && !candidate.originalText.isEmpty() &&
           !candidate.suggestedText.isEmpty() &&
           candidate.originalText != candidate.suggestedText && candidate.confidence >= 0.0 &&
           candidate.confidence <= 1.0 && !candidate.evidence.trimmed().isEmpty() &&
           !candidate.detectorVersion.trimmed().isEmpty() && candidate.sourceHash.isValid();
}

RepairQueueError invalidItem(QString message) {
    return {RepairQueueErrorCode::InvalidItem, std::move(message)};
}

bool canResolve(CandidateStatus status) {
    return status == CandidateStatus::Detected || status == CandidateStatus::ReviewRequired;
}

} // namespace

QString candidateStatusName(CandidateStatus status) {
    switch (status) {
    case CandidateStatus::Detected:
        return QStringLiteral("DETECTED");
    case CandidateStatus::ReviewRequired:
        return QStringLiteral("REVIEW_REQUIRED");
    case CandidateStatus::Approved:
        return QStringLiteral("APPROVED");
    case CandidateStatus::Rejected:
        return QStringLiteral("REJECTED");
    case CandidateStatus::Patched:
        return QStringLiteral("PATCHED");
    case CandidateStatus::Submitted:
        return QStringLiteral("SUBMITTED");
    case CandidateStatus::Merged:
        return QStringLiteral("MERGED");
    case CandidateStatus::Stale:
        return QStringLiteral("STALE");
    }
    return {};
}

std::optional<CandidateStatus> candidateStatusFromName(QStringView name) {
    for (const auto status :
         {CandidateStatus::Detected, CandidateStatus::ReviewRequired, CandidateStatus::Approved,
          CandidateStatus::Rejected, CandidateStatus::Patched, CandidateStatus::Submitted,
          CandidateStatus::Merged, CandidateStatus::Stale}) {
        if (name == candidateStatusName(status)) {
            return status;
        }
    }
    return std::nullopt;
}

RepairQueueResult RepairQueueWorkflow::review(const core::ProjectId& projectId,
                                              const ProofreadingCandidate& candidate,
                                              QString sourceContext, const QDateTime& now) {
    if (!projectId.isValid() || !validCandidate(candidate) || !validTime(now)) {
        return invalidItem(
            QStringLiteral("A valid project, candidate, and review time are required."));
    }
    if (!sourceContext.contains(candidate.originalText)) {
        return invalidItem(
            QStringLiteral("Source context must contain the candidate's original text."));
    }
    return RepairQueueItem{projectId,
                           candidate,
                           candidate.suggestedText,
                           std::move(sourceContext),
                           CandidateStatus::ReviewRequired,
                           now.toUTC(),
                           now.toUTC()};
}

RepairQueueResult RepairQueueWorkflow::approve(const RepairQueueItem& item, const QDateTime& now) {
    if (!validTime(now)) {
        return invalidItem(QStringLiteral("A valid review time is required."));
    }
    if (!canResolve(item.status)) {
        return RepairQueueError{RepairQueueErrorCode::InvalidTransition,
                                QStringLiteral("Only pending candidates can be approved.")};
    }
    if (item.currentSuggestion.isEmpty() || item.currentSuggestion == item.candidate.originalText) {
        return RepairQueueError{
            RepairQueueErrorCode::EmptySuggestion,
            QStringLiteral("An approved candidate must contain a real replacement.")};
    }
    auto updated = item;
    updated.status = CandidateStatus::Approved;
    updated.updatedAt = now.toUTC();
    return updated;
}

RepairQueueResult RepairQueueWorkflow::reject(const RepairQueueItem& item, const QDateTime& now) {
    if (!validTime(now)) {
        return invalidItem(QStringLiteral("A valid review time is required."));
    }
    if (!canResolve(item.status)) {
        return RepairQueueError{RepairQueueErrorCode::InvalidTransition,
                                QStringLiteral("Only pending candidates can be rejected.")};
    }
    auto updated = item;
    updated.status = CandidateStatus::Rejected;
    updated.updatedAt = now.toUTC();
    return updated;
}

RepairQueueResult RepairQueueWorkflow::editSuggestion(const RepairQueueItem& item,
                                                      QString suggestion, const QDateTime& now) {
    suggestion = suggestion.trimmed();
    if (!validTime(now)) {
        return invalidItem(QStringLiteral("A valid review time is required."));
    }
    if (item.status == CandidateStatus::Patched || item.status == CandidateStatus::Submitted ||
        item.status == CandidateStatus::Merged || item.status == CandidateStatus::Stale) {
        return RepairQueueError{RepairQueueErrorCode::InvalidTransition,
                                QStringLiteral("This candidate can no longer be edited.")};
    }
    if (suggestion.isEmpty() || suggestion == item.candidate.originalText) {
        return RepairQueueError{RepairQueueErrorCode::EmptySuggestion,
                                QStringLiteral("The suggestion must be a non-empty replacement.")};
    }
    auto updated = item;
    updated.currentSuggestion = std::move(suggestion);
    updated.status = CandidateStatus::ReviewRequired;
    updated.updatedAt = now.toUTC();
    return updated;
}

PatchAuthorizationResult RepairGate::authorizeApprovedCandidate(const RepairQueueItem& item) {
    if (item.status != CandidateStatus::Approved) {
        return RepairQueueError{RepairQueueErrorCode::UnapprovedCandidate,
                                QStringLiteral("A patch requires an approved candidate.")};
    }
    return PatchAuthorization{
        PatchOrigin::ApprovedCandidate, item.candidate.id,
        item.candidate.chapterId,       item.candidate.sourceSpan,
        item.candidate.originalText,    item.currentSuggestion,
        item.candidate.sourceHash,      QStringLiteral("Approved proofreading candidate")};
}

PatchAuthorizationResult RepairGate::authorizeManualEdit(
    const core::ChapterId& chapterId, const core::SourceSpan& sourceSpan, QString originalText,
    QString replacementText, const core::ContentHash& sourceHash, QString auditReason) {
    auditReason = auditReason.trimmed();
    if (auditReason.isEmpty()) {
        return RepairQueueError{
            RepairQueueErrorCode::ManualReasonRequired,
            QStringLiteral("An explicit manual edit requires an audit reason.")};
    }
    if (!chapterId.isValid() || !sourceSpan.isValid() || originalText.isEmpty() ||
        replacementText == originalText || !sourceHash.isValid()) {
        return invalidItem(QStringLiteral("The manual edit authorization is invalid."));
    }
    return PatchAuthorization{PatchOrigin::ExplicitManualEdit,
                              std::nullopt,
                              chapterId,
                              sourceSpan,
                              std::move(originalText),
                              std::move(replacementText),
                              sourceHash,
                              std::move(auditReason)};
}

} // namespace loreforge::proofreading
