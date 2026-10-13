#pragma once

#include "loreforge/core/identifier.h"
#include "loreforge/storage/storage_error.h"

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QString>

namespace loreforge::storage {

class ProjectDatabase;

struct ProjectRecord final {
    core::ProjectId id;
    QString name;
    QDateTime createdAt;

    friend bool operator==(const ProjectRecord&, const ProjectRecord&) = default;
};

class ProjectRepository final {
  public:
    explicit ProjectRepository(ProjectDatabase& database);

    [[nodiscard]] StorageStatus create(const ProjectRecord& project);
    [[nodiscard]] StorageResult<ProjectRecord> find(const core::ProjectId& projectId) const;
    [[nodiscard]] StorageResult<QList<ProjectRecord>> list() const;
    [[nodiscard]] StorageStatus saveApiSettings(const core::ProjectId& projectId,
                                                const QJsonObject& settings);
    [[nodiscard]] StorageResult<QJsonObject> apiSettings(const core::ProjectId& projectId) const;

  private:
    ProjectDatabase& database_;
};

} // namespace loreforge::storage
