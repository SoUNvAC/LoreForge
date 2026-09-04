#include "project_workspace.h"

#include "loreforge/storage/book_repository.h"
#include "loreforge/storage/project_database.h"

#include <utility>

namespace loreforge::app {

WorkspaceLoadResult ProjectWorkspaceLoader::load(storage::ProjectDatabase& database) {
    storage::ProjectRepository projectRepository(database);
    auto projectResult = projectRepository.list();
    if (std::holds_alternative<storage::StorageError>(projectResult)) {
        return std::get<storage::StorageError>(std::move(projectResult));
    }

    StoredWorkspace workspace{database.filePath(), {}};
    storage::BookRepository bookRepository(database);
    for (auto& project : std::get<QList<storage::ProjectRecord>>(projectResult)) {
        auto bookIdsResult = bookRepository.listBookIds(project.id);
        if (std::holds_alternative<storage::StorageError>(bookIdsResult)) {
            return std::get<storage::StorageError>(std::move(bookIdsResult));
        }

        StoredProject storedProject{std::move(project), {}};
        for (const auto& bookId : std::get<QList<core::BookId>>(bookIdsResult)) {
            auto documentResult = bookRepository.loadDocument(bookId);
            if (std::holds_alternative<storage::StorageError>(documentResult)) {
                return std::get<storage::StorageError>(std::move(documentResult));
            }
            storedProject.books.append(std::get<document::Document>(std::move(documentResult)));
        }
        workspace.projects.append(std::move(storedProject));
    }
    return workspace;
}

} // namespace loreforge::app
