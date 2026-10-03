#pragma once

#include "loreforge/proofreading/proofreading_types.h"

#include <QDateTime>
#include <QString>
#include <QStringView>

#include <optional>
#include <variant>

namespace loreforge::proofreading {

enum class CandidateStatus {
    Detected,
    ReviewRequired,
    Approved,
    Rejected,
    Patched,
    Submitted,
    Merged,
    Stale,
};

[[nodiscard]] QString candidateStatusName(CandidateStatus status);
[[nodiscard]] std::optional<CandidateStatus> candidateStatusFromName(QStringView name);

struct RepairQueueItem final {
    core::ProjectId projectId;
    ProofreadingCandidate candidate;
    QString currentSuggestion;
    QString sourceContext;
    CandidateStatus status = CandidateStatus::ReviewRequired;
    QDateTime createdAt;
    QDateTime updatedAt;

    friend bool operator==(const RepairQueueItem&, const RepairQueueItem&) = default;
};

enum class RepairQueueErrorCode {
    InvalidItem,
    InvalidTransition,
    EmptySuggestion,
    UnapprovedCandidate,
    ManualReasonRequired,
};

struct RepairQueueError final {
    RepairQueueErrorCode code;
    QString message;

    friend bool operator==(const RepairQueueError&, const RepairQueueError&) = default;
};

using RepairQueueResult = std::variant<RepairQueueItem, RepairQueueError>;

class RepairQueueWorkflow final {
  public:
    [[nodiscard]] static RepairQueueResult review(const core::ProjectId& projectId,
                                                  const ProofreadingCandidate& candidate,
                                                  QString sourceContext, const QDateTime& now);
    [[nodiscard]] static RepairQueueResult approve(const RepairQueueItem& item,
                                                   const QDateTime& now);
    [[nodiscard]] static RepairQueueResult reject(const RepairQueueItem& item,
                                                  const QDateTime& now);
    [[nodiscard]] static RepairQueueResult editSuggestion(const RepairQueueItem& item,
                                                          QString suggestion, const QDateTime& now);
};

enum class PatchOrigin {
    ApprovedCandidate,
    ExplicitManualEdit,
};

struct PatchAuthorization final {
    PatchOrigin origin = PatchOrigin::ApprovedCandidate;
    std::optional<core::ProofreadingCandidateId> candidateId;
    core::ChapterId chapterId;
    core::SourceSpan sourceSpan;
    QString originalText;
    QString replacementText;
    core::ContentHash sourceHash;
    QString auditReason;

    friend bool operator==(const PatchAuthorization&, const PatchAuthorization&) = default;
};

using PatchAuthorizationResult = std::variant<PatchAuthorization, RepairQueueError>;

class RepairGate final {
  public:
    [[nodiscard]] static PatchAuthorizationResult
    authorizeApprovedCandidate(const RepairQueueItem& item);
    [[nodiscard]] static PatchAuthorizationResult
    authorizeManualEdit(const core::ChapterId& chapterId, const core::SourceSpan& sourceSpan,
                        QString originalText, QString replacementText,
                        const core::ContentHash& sourceHash, QString auditReason);
};

} // namespace loreforge::proofreading
