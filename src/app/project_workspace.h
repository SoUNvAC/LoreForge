#pragma once

#include "loreforge/document/document.h"
#include "loreforge/storage/project_repository.h"
#include "loreforge/storage/storage_error.h"

#include <QList>
#include <QString>

#include <variant>

namespace loreforge::storage {
class ProjectDatabase;
}

namespace loreforge::app {

struct StoredProject final {
    storage::ProjectRecord metadata;
    QList<document::Document> books;

    friend bool operator==(const StoredProject&, const StoredProject&) = default;
};

struct StoredWorkspace final {
    QString databasePath;
    QList<StoredProject> projects;

    friend bool operator==(const StoredWorkspace&, const StoredWorkspace&) = default;
};

using WorkspaceLoadResult = std::variant<StoredWorkspace, storage::StorageError>;

class ProjectWorkspaceLoader final {
  public:
    [[nodiscard]] static WorkspaceLoadResult load(storage::ProjectDatabase& database);
};

} // namespace loreforge::app
