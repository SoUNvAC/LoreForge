#pragma once

#include "loreforge/core/identifier.h"
#include "loreforge/document/document.h"
#include "loreforge/storage/storage_error.h"

#include <QList>

namespace loreforge::storage {

class ProjectDatabase;

class BookRepository final {
  public:
    explicit BookRepository(ProjectDatabase& database);

    [[nodiscard]] StorageStatus saveDocument(const core::ProjectId& projectId,
                                             const document::Document& document);
    [[nodiscard]] StorageResult<document::Document> loadDocument(const core::BookId& bookId) const;
    [[nodiscard]] StorageResult<QList<core::BookId>>
    listBookIds(const core::ProjectId& projectId) const;

  private:
    ProjectDatabase& database_;
};

} // namespace loreforge::storage
