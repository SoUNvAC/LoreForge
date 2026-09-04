#pragma once

#include "loreforge/storage/storage_error.h"

#include <QString>
#include <QStringView>

#include <functional>
#include <memory>

class QSqlDatabase;

namespace loreforge::storage {

class BookRepository;
class ChapterRepository;
class LLMRunRepository;
class ProjectRepository;

class ProjectDatabase final {
  public:
    using TransactionWork = std::function<StorageStatus()>;

    [[nodiscard]] static StorageResult<std::unique_ptr<ProjectDatabase>>
    create(QStringView filePath);
    [[nodiscard]] static StorageResult<std::unique_ptr<ProjectDatabase>> open(QStringView filePath);

    ~ProjectDatabase();

    ProjectDatabase(const ProjectDatabase&) = delete;
    ProjectDatabase& operator=(const ProjectDatabase&) = delete;
    ProjectDatabase(ProjectDatabase&&) = delete;
    ProjectDatabase& operator=(ProjectDatabase&&) = delete;

    [[nodiscard]] const QString& filePath() const noexcept;
    [[nodiscard]] int schemaVersion() const noexcept;
    [[nodiscard]] StorageStatus runInTransaction(const TransactionWork& work);

  private:
    [[nodiscard]] static StorageResult<std::unique_ptr<ProjectDatabase>>
    openDatabase(QStringView filePath, bool createNew);
    ProjectDatabase(QString filePath, QString connectionName, int schemaVersion);

    [[nodiscard]] QSqlDatabase database() const;

    friend class BookRepository;
    friend class ChapterRepository;
    friend class LLMRunRepository;
    friend class ProjectRepository;

    QString filePath_;
    QString connectionName_;
    int schemaVersion_ = 0;
};

} // namespace loreforge::storage
