#include "loreforge/storage/project_database.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

#include <exception>
#include <utility>

namespace loreforge::storage {
namespace {

constexpr int latestSchemaVersion = 1;

StorageError makeError(StorageErrorCode code, QString message, QString details = {},
                       bool recoverable = true) {
    return {code, std::move(message), std::move(details), recoverable};
}

StorageError sqlError(StorageErrorCode fallbackCode, const QString& message,
                      const QSqlError& error) {
    const auto details = error.databaseText() + QStringLiteral(" ") + error.driverText();
    const auto lowerDetails = details.toLower();
    const bool corrupt = lowerDetails.contains(QStringLiteral("not a database")) ||
                         lowerDetails.contains(QStringLiteral("malformed"));
    return makeError(corrupt ? StorageErrorCode::CorruptDatabase : fallbackCode, message,
                     details.trimmed(), !corrupt);
}

StorageStatus executeSql(QSqlDatabase& database, QStringView sql, StorageErrorCode errorCode,
                         QStringView message) {
    QSqlQuery query(database);
    if (!query.exec(sql.toString())) {
        return sqlError(errorCode, message.toString(), query.lastError());
    }
    return std::nullopt;
}

StorageResult<int> recordedSchemaVersion(QSqlDatabase& database) {
    QSqlQuery query(database);
    if (!query.exec(QStringLiteral("SELECT COALESCE(MAX(version), 0) FROM schema_migrations"))) {
        return sqlError(StorageErrorCode::MigrationFailed,
                        QStringLiteral("Could not read the database schema version."),
                        query.lastError());
    }
    if (!query.next()) {
        return makeError(StorageErrorCode::MigrationFailed,
                         QStringLiteral("The database schema version is missing."));
    }
    return query.value(0).toInt();
}

StorageStatus applyInitialMigration(QSqlDatabase& database) {
    QFile migration(QStringLiteral(":/loreforge/migrations/001_initial.sql"));
    if (!migration.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return makeError(StorageErrorCode::MigrationFailed,
                         QStringLiteral("The initial database migration is unavailable."),
                         migration.errorString(), false);
    }

    if (!database.transaction()) {
        return sqlError(StorageErrorCode::TransactionFailed,
                        QStringLiteral("Could not start the schema migration transaction."),
                        database.lastError());
    }

    const auto statements = QString::fromUtf8(migration.readAll()).split(QLatin1Char(';'));
    for (const auto& statement : statements) {
        if (statement.trimmed().isEmpty()) {
            continue;
        }
        if (const auto status = executeSql(database, statement, StorageErrorCode::MigrationFailed,
                                           QStringLiteral("The initial schema migration failed."));
            status.has_value()) {
            database.rollback();
            return status;
        }
    }

    QSqlQuery recordMigration(database);
    recordMigration.prepare(
        QStringLiteral("INSERT INTO schema_migrations(version, name, applied_at) VALUES(1, ?, ?)"));
    recordMigration.addBindValue(QStringLiteral("001_initial"));
    recordMigration.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    if (!recordMigration.exec()) {
        const auto migrationError =
            sqlError(StorageErrorCode::MigrationFailed,
                     QStringLiteral("Could not record the initial schema migration."),
                     recordMigration.lastError());
        database.rollback();
        return migrationError;
    }

    if (const auto status = executeSql(database, QStringLiteral("PRAGMA user_version = 1"),
                                       StorageErrorCode::MigrationFailed,
                                       QStringLiteral("Could not update the schema version."));
        status.has_value()) {
        database.rollback();
        return status;
    }

    if (!database.commit()) {
        const auto commitError = sqlError(StorageErrorCode::TransactionFailed,
                                          QStringLiteral("Could not commit the schema migration."),
                                          database.lastError());
        database.rollback();
        return commitError;
    }
    return std::nullopt;
}

StorageStatus verifySchema(QSqlDatabase& database) {
    QSqlQuery integrity(database);
    if (!integrity.exec(QStringLiteral("PRAGMA quick_check")) || !integrity.next()) {
        return sqlError(StorageErrorCode::CorruptDatabase,
                        QStringLiteral("The project database integrity check failed."),
                        integrity.lastError());
    }
    if (integrity.value(0).toString() != QStringLiteral("ok")) {
        return makeError(StorageErrorCode::CorruptDatabase,
                         QStringLiteral("The project database is damaged."),
                         integrity.value(0).toString(), false);
    }

    QSqlQuery foreignKeyCheck(database);
    if (!foreignKeyCheck.exec(QStringLiteral("PRAGMA foreign_key_check"))) {
        return sqlError(StorageErrorCode::CorruptDatabase,
                        QStringLiteral("The project relationships could not be verified."),
                        foreignKeyCheck.lastError());
    }
    if (foreignKeyCheck.next()) {
        return makeError(
            StorageErrorCode::CorruptDatabase,
            QStringLiteral("The project database contains broken relationships."),
            QStringLiteral("Table %1 contains a broken reference at row %2.")
                .arg(foreignKeyCheck.value(0).toString(), foreignKeyCheck.value(1).toString()),
            false);
    }

    QSqlQuery foreignKeys(database);
    if (!foreignKeys.exec(QStringLiteral("PRAGMA foreign_keys")) || !foreignKeys.next() ||
        foreignKeys.value(0).toInt() != 1) {
        return makeError(StorageErrorCode::CannotOpen,
                         QStringLiteral("SQLite foreign-key enforcement is unavailable."), {},
                         false);
    }

    QSqlQuery tables(database);
    if (!tables.exec(
            QStringLiteral("SELECT name FROM sqlite_master WHERE type = 'table' AND name IN "
                           "('schema_migrations', 'projects', 'books', 'chapters', 'blocks')"))) {
        return sqlError(StorageErrorCode::CorruptDatabase,
                        QStringLiteral("The project database schema could not be inspected."),
                        tables.lastError());
    }
    QSet<QString> presentTables;
    while (tables.next()) {
        presentTables.insert(tables.value(0).toString());
    }
    const QSet<QString> requiredTables{
        QStringLiteral("schema_migrations"), QStringLiteral("projects"), QStringLiteral("books"),
        QStringLiteral("chapters"),          QStringLiteral("blocks"),
    };
    if (presentTables != requiredTables) {
        return makeError(StorageErrorCode::CorruptDatabase,
                         QStringLiteral("The project database schema is incomplete."), {}, false);
    }
    return std::nullopt;
}

StorageResult<int> migrate(QSqlDatabase& database) {
    if (const auto status = executeSql(
            database, QStringLiteral("PRAGMA foreign_keys = ON"), StorageErrorCode::CannotOpen,
            QStringLiteral("Could not enable SQLite foreign-key enforcement."));
        status.has_value()) {
        return *status;
    }

    if (const auto status = executeSql(
            database,
            QStringLiteral(
                "CREATE TABLE IF NOT EXISTS schema_migrations (version INTEGER PRIMARY KEY NOT "
                "NULL, name TEXT NOT NULL, applied_at TEXT NOT NULL)"),
            StorageErrorCode::MigrationFailed,
            QStringLiteral("Could not initialize database migration tracking."));
        status.has_value()) {
        return *status;
    }

    const auto recorded = recordedSchemaVersion(database);
    if (std::holds_alternative<StorageError>(recorded)) {
        return std::get<StorageError>(recorded);
    }
    const auto version = std::get<int>(recorded);
    if (version > latestSchemaVersion) {
        return makeError(StorageErrorCode::UnsupportedSchema,
                         QStringLiteral("This project uses a newer database schema."),
                         QStringLiteral("Schema version %1 is newer than supported version %2.")
                             .arg(version)
                             .arg(latestSchemaVersion),
                         false);
    }
    if (version == 0) {
        if (const auto status = applyInitialMigration(database); status.has_value()) {
            return *status;
        }
    }

    QSqlQuery userVersion(database);
    if (!userVersion.exec(QStringLiteral("PRAGMA user_version")) || !userVersion.next()) {
        return sqlError(StorageErrorCode::MigrationFailed,
                        QStringLiteral("Could not verify the SQLite user version."),
                        userVersion.lastError());
    }
    const auto sqliteVersion = userVersion.value(0).toInt();
    if (sqliteVersion != latestSchemaVersion) {
        return makeError(StorageErrorCode::CorruptDatabase,
                         QStringLiteral("The database contains inconsistent schema metadata."),
                         QStringLiteral("Migration version is %1 but SQLite user_version is %2.")
                             .arg(latestSchemaVersion)
                             .arg(sqliteVersion),
                         false);
    }
    if (const auto status = verifySchema(database); status.has_value()) {
        return *status;
    }
    return latestSchemaVersion;
}

} // namespace

StorageResult<std::unique_ptr<ProjectDatabase>>
ProjectDatabase::openDatabase(QStringView requestedPath, bool createNew) {
    const auto trimmedPath = requestedPath.trimmed().toString();
    if (trimmedPath.isEmpty()) {
        return makeError(StorageErrorCode::InvalidPath,
                         QStringLiteral("A project database path is required."));
    }

    const QFileInfo fileInfo(trimmedPath);
    if (createNew && fileInfo.exists()) {
        return makeError(StorageErrorCode::FileAlreadyExists,
                         QStringLiteral("A file already exists at the project path."));
    }
    if (!createNew && (!fileInfo.exists() || !fileInfo.isFile())) {
        return makeError(StorageErrorCode::FileNotFound,
                         QStringLiteral("The project database does not exist."));
    }
    if (!QSqlDatabase::isDriverAvailable(QStringLiteral("QSQLITE"))) {
        return makeError(StorageErrorCode::DriverUnavailable,
                         QStringLiteral("The SQLite driver is unavailable."), {}, false);
    }

    const auto absolutePath = QDir::cleanPath(fileInfo.absoluteFilePath());
    const auto connectionName =
        QStringLiteral("loreforge-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    {
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
        database.setDatabaseName(absolutePath);
        if (!database.open()) {
            const auto openError = sqlError(
                StorageErrorCode::CannotOpen,
                QStringLiteral("The project database could not be opened."), database.lastError());
            database = {};
            QSqlDatabase::removeDatabase(connectionName);
            return openError;
        }

        const auto migrationResult = migrate(database);
        if (std::holds_alternative<StorageError>(migrationResult)) {
            const auto migrationError = std::get<StorageError>(migrationResult);
            database.close();
            database = {};
            QSqlDatabase::removeDatabase(connectionName);
            return migrationError;
        }
    }

    return std::unique_ptr<ProjectDatabase>(
        new ProjectDatabase(absolutePath, connectionName, latestSchemaVersion));
}

StorageResult<std::unique_ptr<ProjectDatabase>> ProjectDatabase::create(QStringView filePath) {
    return openDatabase(filePath, true);
}

StorageResult<std::unique_ptr<ProjectDatabase>> ProjectDatabase::open(QStringView filePath) {
    return openDatabase(filePath, false);
}

ProjectDatabase::ProjectDatabase(QString filePath, QString connectionName, int schemaVersion)
    : filePath_(std::move(filePath)), connectionName_(std::move(connectionName)),
      schemaVersion_(schemaVersion) {}

ProjectDatabase::~ProjectDatabase() {
    {
        auto connection = QSqlDatabase::database(connectionName_, false);
        if (connection.isValid()) {
            connection.close();
        }
    }
    QSqlDatabase::removeDatabase(connectionName_);
}

const QString& ProjectDatabase::filePath() const noexcept {
    return filePath_;
}

int ProjectDatabase::schemaVersion() const noexcept {
    return schemaVersion_;
}

StorageStatus ProjectDatabase::runInTransaction(const TransactionWork& work) {
    auto connection = database();
    if (!connection.transaction()) {
        return sqlError(StorageErrorCode::TransactionFailed,
                        QStringLiteral("Could not start a database transaction."),
                        connection.lastError());
    }

    StorageStatus workStatus;
    try {
        workStatus = work();
    } catch (const std::exception& exception) {
        if (!connection.rollback()) {
            return sqlError(StorageErrorCode::TransactionFailed,
                            QStringLiteral("The operation threw an exception and rollback failed."),
                            connection.lastError());
        }
        return makeError(StorageErrorCode::TransactionFailed,
                         QStringLiteral("The database transaction was cancelled."),
                         QString::fromUtf8(exception.what()), true);
    } catch (...) {
        if (!connection.rollback()) {
            return sqlError(StorageErrorCode::TransactionFailed,
                            QStringLiteral("The operation threw an exception and rollback failed."),
                            connection.lastError());
        }
        return makeError(StorageErrorCode::TransactionFailed,
                         QStringLiteral("The database transaction was cancelled."), {}, true);
    }

    if (workStatus.has_value()) {
        if (!connection.rollback()) {
            return sqlError(StorageErrorCode::TransactionFailed,
                            QStringLiteral("The operation failed and rollback also failed."),
                            connection.lastError());
        }
        return workStatus;
    }

    if (!connection.commit()) {
        const auto commitError = sqlError(
            StorageErrorCode::TransactionFailed,
            QStringLiteral("Could not commit the database transaction."), connection.lastError());
        connection.rollback();
        return commitError;
    }
    return std::nullopt;
}

QSqlDatabase ProjectDatabase::database() const {
    return QSqlDatabase::database(connectionName_);
}

} // namespace loreforge::storage
