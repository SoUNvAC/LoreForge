#include "loreforge/storage/project_repository.h"

#include "loreforge/storage/project_database.h"

#include <QSqlError>
#include <QSqlQuery>

namespace loreforge::storage {
namespace {

StorageError queryError(QString message, const QSqlError& error) {
    const auto details = (error.databaseText() + QLatin1Char(' ') + error.driverText()).trimmed();
    const auto conflict =
        details.contains(QStringLiteral("UNIQUE constraint failed"), Qt::CaseInsensitive);
    return {conflict ? StorageErrorCode::Conflict : StorageErrorCode::SqlError, std::move(message),
            details, true};
}

StorageResult<ProjectRecord> recordFromQuery(const QSqlQuery& query) {
    const auto id = core::ProjectId::fromString(query.value(0).toString());
    const auto createdAt = QDateTime::fromString(query.value(2).toString(), Qt::ISODateWithMs);
    if (!id.has_value() || !createdAt.isValid()) {
        return StorageError{StorageErrorCode::CorruptData,
                            QStringLiteral("Stored project metadata is invalid."),
                            {},
                            false};
    }
    return ProjectRecord{*id, query.value(1).toString(), createdAt};
}

} // namespace

ProjectRepository::ProjectRepository(ProjectDatabase& database) : database_(database) {}

StorageStatus ProjectRepository::create(const ProjectRecord& project) {
    if (!project.id.isValid() || project.name.trimmed().isEmpty() || !project.createdAt.isValid()) {
        return StorageError{StorageErrorCode::InvalidArgument,
                            QStringLiteral("Project ID, name, and creation time are required."),
                            {},
                            true};
    }

    auto connection = database_.database();
    QSqlQuery query(connection);
    query.prepare(QStringLiteral("INSERT INTO projects(id, name, created_at) VALUES(?, ?, ?)"));
    query.addBindValue(project.id.toString());
    query.addBindValue(project.name);
    query.addBindValue(project.createdAt.toUTC().toString(Qt::ISODateWithMs));
    if (!query.exec()) {
        return queryError(QStringLiteral("The project could not be created."), query.lastError());
    }
    return std::nullopt;
}

StorageResult<ProjectRecord> ProjectRepository::find(const core::ProjectId& projectId) const {
    auto connection = database_.database();
    QSqlQuery query(connection);
    query.prepare(QStringLiteral("SELECT id, name, created_at FROM projects WHERE id = ?"));
    query.addBindValue(projectId.toString());
    if (!query.exec()) {
        return queryError(QStringLiteral("The project could not be read."), query.lastError());
    }
    if (!query.next()) {
        return StorageError{
            StorageErrorCode::NotFound, QStringLiteral("The project was not found."), {}, true};
    }
    return recordFromQuery(query);
}

StorageResult<QList<ProjectRecord>> ProjectRepository::list() const {
    auto connection = database_.database();
    QSqlQuery query(connection);
    if (!query.exec(
            QStringLiteral("SELECT id, name, created_at FROM projects ORDER BY created_at, id"))) {
        return queryError(QStringLiteral("Projects could not be listed."), query.lastError());
    }

    QList<ProjectRecord> projects;
    while (query.next()) {
        auto project = recordFromQuery(query);
        if (std::holds_alternative<StorageError>(project)) {
            return std::get<StorageError>(project);
        }
        projects.append(std::get<ProjectRecord>(std::move(project)));
    }
    return projects;
}

} // namespace loreforge::storage
