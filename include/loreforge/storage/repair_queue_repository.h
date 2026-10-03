#pragma once

#include "loreforge/proofreading/repair_queue.h"
#include "loreforge/storage/storage_error.h"

#include <QDateTime>
#include <QList>
#include <QString>

namespace loreforge::storage {

class ProjectDatabase;

class RepairQueueRepository final {
  public:
    explicit RepairQueueRepository(ProjectDatabase& database);

    [[nodiscard]] StorageStatus enqueue(const core::ProjectId& projectId,
                                        const proofreading::ProofreadingCandidate& candidate,
                                        QString sourceContext, const QDateTime& now);
    [[nodiscard]] StorageResult<proofreading::RepairQueueItem>
    find(const core::ProofreadingCandidateId& candidateId) const;
    [[nodiscard]] StorageResult<QList<proofreading::RepairQueueItem>>
    list(const core::ProjectId& projectId) const;
    [[nodiscard]] StorageStatus approve(const core::ProofreadingCandidateId& candidateId,
                                        const QDateTime& now);
    [[nodiscard]] StorageStatus reject(const core::ProofreadingCandidateId& candidateId,
                                       const QDateTime& now);
    [[nodiscard]] StorageStatus editSuggestion(const core::ProofreadingCandidateId& candidateId,
                                               QString suggestion, const QDateTime& now);
    [[nodiscard]] StorageStatus ignoreAndProtect(const core::ProofreadingCandidateId& candidateId,
                                                 QString notes, const QDateTime& now);
    [[nodiscard]] StorageResult<QList<proofreading::ProtectedTerm>>
    protectedTerms(const core::ProjectId& projectId) const;

  private:
    ProjectDatabase& database_;
};

} // namespace loreforge::storage
