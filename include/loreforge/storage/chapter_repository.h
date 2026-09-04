#pragma once

#include "loreforge/core/identifier.h"
#include "loreforge/document/document.h"
#include "loreforge/storage/storage_error.h"

#include <QList>

namespace loreforge::storage {

class BookRepository;
class ProjectDatabase;

class ChapterRepository final {
  public:
    explicit ChapterRepository(ProjectDatabase& database);

    [[nodiscard]] StorageStatus replaceForBook(const core::BookId& bookId,
                                               const QList<document::Chapter>& chapters);
    [[nodiscard]] StorageResult<QList<document::Chapter>>
    loadForBook(const core::BookId& bookId) const;

  private:
    [[nodiscard]] StorageStatus
    replaceInCurrentTransaction(const core::BookId& bookId,
                                const QList<document::Chapter>& chapters);

    friend class BookRepository;

    ProjectDatabase& database_;
};

} // namespace loreforge::storage
