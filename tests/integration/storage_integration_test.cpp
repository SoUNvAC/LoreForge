#include "document_fixture.h"

#include "loreforge/document/document_json_codec.h"
#include "loreforge/storage/book_repository.h"
#include "loreforge/storage/project_database.h"
#include "loreforge/storage/project_repository.h"

#include <QFile>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>
#include <stdexcept>
#include <variant>

using namespace Qt::StringLiterals;

class StorageIntegrationTest final : public QObject {
    Q_OBJECT

  private slots:
    void createsAndReopensAProject();
    void preservesDocumentBytesAcrossReopen();
    void failedBookSaveRollsBackAllTables();
    void migratesAnExistingVersionZeroDatabase();
    void rejectsANewerSchemaVersion();
    void rollsBackFailedTransactions();
    void rejectsNonSqliteInput();
    void rejectsCorruptStoredDocument();
};

namespace {

std::unique_ptr<loreforge::storage::ProjectDatabase> takeDatabase(
    loreforge::storage::StorageResult<std::unique_ptr<loreforge::storage::ProjectDatabase>>&
        result) {
    return std::get<std::unique_ptr<loreforge::storage::ProjectDatabase>>(std::move(result));
}

loreforge::storage::ProjectRecord projectRecord() {
    return {
        loreforge::core::ProjectId::fromStableKey(u"fixtures/project.loreforge"_s),
        u"Fixture Project"_s,
        QDateTime::fromString(u"2026-09-04T00:00:00.000Z"_s, Qt::ISODateWithMs),
    };
}

bool executeRawSql(const QString& databasePath, const QString& sql) {
    const auto connectionName = u"storage-test-raw"_s;
    bool succeeded = false;
    {
        auto database = QSqlDatabase::addDatabase(u"QSQLITE"_s, connectionName);
        database.setDatabaseName(databasePath);
        if (database.open()) {
            QSqlQuery query(database);
            succeeded = query.exec(sql);
        }
        database.close();
    }
    QSqlDatabase::removeDatabase(connectionName);
    return succeeded;
}

} // namespace

void StorageIntegrationTest::createsAndReopensAProject() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto databasePath = directory.filePath(QStringLiteral("project.loreforge"));

    auto created = loreforge::storage::ProjectDatabase::create(databasePath);
    QVERIFY(std::holds_alternative<std::unique_ptr<loreforge::storage::ProjectDatabase>>(created));
    auto database = takeDatabase(created);
    QCOMPARE(database->schemaVersion(), 1);
    loreforge::storage::ProjectRepository projects(*database);
    QVERIFY(!projects.create(projectRecord()).has_value());
    const auto projectList = projects.list();
    QVERIFY(std::holds_alternative<QList<loreforge::storage::ProjectRecord>>(projectList));
    QCOMPARE(std::get<QList<loreforge::storage::ProjectRecord>>(projectList),
             QList<loreforge::storage::ProjectRecord>{projectRecord()});

    database.reset();
    auto reopened = loreforge::storage::ProjectDatabase::open(databasePath);
    QVERIFY(std::holds_alternative<std::unique_ptr<loreforge::storage::ProjectDatabase>>(reopened));
    database = takeDatabase(reopened);
    loreforge::storage::ProjectRepository reopenedProjects(*database);
    const auto loaded = reopenedProjects.find(projectRecord().id);
    QVERIFY(std::holds_alternative<loreforge::storage::ProjectRecord>(loaded));
    QCOMPARE(std::get<loreforge::storage::ProjectRecord>(loaded), projectRecord());
}

void StorageIntegrationTest::preservesDocumentBytesAcrossReopen() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto databasePath = directory.filePath(QStringLiteral("round-trip.loreforge"));
    const auto original = loreforge::test::handcraftedDocument();

    auto created = loreforge::storage::ProjectDatabase::create(databasePath);
    QVERIFY(std::holds_alternative<std::unique_ptr<loreforge::storage::ProjectDatabase>>(created));
    auto database = takeDatabase(created);
    loreforge::storage::ProjectRepository projects(*database);
    QVERIFY(!projects.create(projectRecord()).has_value());
    loreforge::storage::BookRepository books(*database);
    QVERIFY(!books.saveDocument(projectRecord().id, original).has_value());
    const auto bookIds = books.listBookIds(projectRecord().id);
    QVERIFY(std::holds_alternative<QList<loreforge::core::BookId>>(bookIds));
    QCOMPARE(std::get<QList<loreforge::core::BookId>>(bookIds),
             QList<loreforge::core::BookId>{original.id});
    database.reset();

    auto reopened = loreforge::storage::ProjectDatabase::open(databasePath);
    QVERIFY(std::holds_alternative<std::unique_ptr<loreforge::storage::ProjectDatabase>>(reopened));
    database = takeDatabase(reopened);
    loreforge::storage::BookRepository reopenedBooks(*database);
    const auto loaded = reopenedBooks.loadDocument(original.id);
    QVERIFY(std::holds_alternative<loreforge::document::Document>(loaded));
    const auto& loadedDocument = std::get<loreforge::document::Document>(loaded);
    QCOMPARE(loadedDocument, original);

    const auto originalJson = loreforge::document::DocumentJsonCodec::encode(original);
    const auto loadedJson = loreforge::document::DocumentJsonCodec::encode(loadedDocument);
    QVERIFY(std::holds_alternative<QByteArray>(originalJson));
    QCOMPARE(std::get<QByteArray>(originalJson), std::get<QByteArray>(loadedJson));
}

void StorageIntegrationTest::failedBookSaveRollsBackAllTables() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    auto created = loreforge::storage::ProjectDatabase::create(
        directory.filePath(QStringLiteral("book-rollback.loreforge")));
    QVERIFY(std::holds_alternative<std::unique_ptr<loreforge::storage::ProjectDatabase>>(created));
    auto database = takeDatabase(created);
    loreforge::storage::ProjectRepository projects(*database);
    QVERIFY(!projects.create(projectRecord()).has_value());
    loreforge::storage::BookRepository books(*database);

    const auto original = loreforge::test::handcraftedDocument();
    QVERIFY(!books.saveDocument(projectRecord().id, original).has_value());
    auto conflicting = original;
    conflicting.id = loreforge::core::BookId::fromStableKey(u"fixtures/conflicting.txt"_s);
    conflicting.metadata.sourceLocator = u"fixtures/conflicting.txt"_s;
    const auto failedSave = books.saveDocument(projectRecord().id, conflicting);
    QVERIFY(failedSave.has_value());

    const auto missing = books.loadDocument(conflicting.id);
    QVERIFY(std::holds_alternative<loreforge::storage::StorageError>(missing));
    QCOMPARE(std::get<loreforge::storage::StorageError>(missing).code,
             loreforge::storage::StorageErrorCode::NotFound);
    const auto preserved = books.loadDocument(original.id);
    QVERIFY(std::holds_alternative<loreforge::document::Document>(preserved));
    QCOMPARE(std::get<loreforge::document::Document>(preserved), original);
}

void StorageIntegrationTest::migratesAnExistingVersionZeroDatabase() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto databasePath = directory.filePath(QStringLiteral("version-zero.loreforge"));
    QVERIFY(executeRawSql(databasePath,
                          QStringLiteral("CREATE TABLE legacy_marker(value TEXT NOT NULL)")));

    auto opened = loreforge::storage::ProjectDatabase::open(databasePath);
    QVERIFY(std::holds_alternative<std::unique_ptr<loreforge::storage::ProjectDatabase>>(opened));
    auto database = takeDatabase(opened);
    QCOMPARE(database->schemaVersion(), 1);
    database.reset();

    QVERIFY(executeRawSql(databasePath,
                          QStringLiteral("INSERT INTO legacy_marker(value) VALUES('preserved')")));
}

void StorageIntegrationTest::rejectsANewerSchemaVersion() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto databasePath = directory.filePath(QStringLiteral("future-schema.loreforge"));
    QVERIFY(executeRawSql(
        databasePath,
        QStringLiteral("CREATE TABLE schema_migrations(version INTEGER PRIMARY KEY NOT NULL, "
                       "name TEXT NOT NULL, applied_at TEXT NOT NULL)")));
    QVERIFY(executeRawSql(
        databasePath,
        QStringLiteral("INSERT INTO schema_migrations VALUES(99, 'future', '2026-09-04')")));

    const auto opened = loreforge::storage::ProjectDatabase::open(databasePath);
    QVERIFY(std::holds_alternative<loreforge::storage::StorageError>(opened));
    QCOMPARE(std::get<loreforge::storage::StorageError>(opened).code,
             loreforge::storage::StorageErrorCode::UnsupportedSchema);
}

void StorageIntegrationTest::rollsBackFailedTransactions() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    auto created = loreforge::storage::ProjectDatabase::create(
        directory.filePath(QStringLiteral("rollback.loreforge")));
    QVERIFY(std::holds_alternative<std::unique_ptr<loreforge::storage::ProjectDatabase>>(created));
    auto database = takeDatabase(created);
    loreforge::storage::ProjectRepository projects(*database);

    const auto status = database->runInTransaction([&projects] {
        if (const auto createStatus = projects.create(projectRecord()); createStatus.has_value()) {
            return createStatus;
        }
        return loreforge::storage::StorageStatus(loreforge::storage::StorageError{
            loreforge::storage::StorageErrorCode::TransactionFailed,
            QStringLiteral("Intentional test failure."),
            {},
            true});
    });
    QVERIFY(status.has_value());

    const auto missing = projects.find(projectRecord().id);
    QVERIFY(std::holds_alternative<loreforge::storage::StorageError>(missing));
    QCOMPARE(std::get<loreforge::storage::StorageError>(missing).code,
             loreforge::storage::StorageErrorCode::NotFound);

    const auto exceptionStatus =
        database->runInTransaction([&projects]() -> loreforge::storage::StorageStatus {
            if (const auto createStatus = projects.create(projectRecord());
                createStatus.has_value()) {
                return createStatus;
            }
            throw std::runtime_error("intentional test exception");
        });
    QVERIFY(exceptionStatus.has_value());
    QCOMPARE(exceptionStatus->code, loreforge::storage::StorageErrorCode::TransactionFailed);
    const auto stillMissing = projects.find(projectRecord().id);
    QVERIFY(std::holds_alternative<loreforge::storage::StorageError>(stillMissing));
    QCOMPARE(std::get<loreforge::storage::StorageError>(stillMissing).code,
             loreforge::storage::StorageErrorCode::NotFound);
}

void StorageIntegrationTest::rejectsNonSqliteInput() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto databasePath = directory.filePath(QStringLiteral("corrupt.loreforge"));
    QFile corruptFile(databasePath);
    QVERIFY(corruptFile.open(QIODevice::WriteOnly));
    QCOMPARE(corruptFile.write("this is not sqlite"), 18);
    corruptFile.close();

    const auto opened = loreforge::storage::ProjectDatabase::open(databasePath);
    QVERIFY(std::holds_alternative<loreforge::storage::StorageError>(opened));
    QCOMPARE(std::get<loreforge::storage::StorageError>(opened).code,
             loreforge::storage::StorageErrorCode::CorruptDatabase);
}

void StorageIntegrationTest::rejectsCorruptStoredDocument() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto databasePath = directory.filePath(QStringLiteral("corrupt-document.loreforge"));
    const auto original = loreforge::test::handcraftedDocument();

    auto created = loreforge::storage::ProjectDatabase::create(databasePath);
    QVERIFY(std::holds_alternative<std::unique_ptr<loreforge::storage::ProjectDatabase>>(created));
    auto database = takeDatabase(created);
    loreforge::storage::ProjectRepository projects(*database);
    QVERIFY(!projects.create(projectRecord()).has_value());
    loreforge::storage::BookRepository books(*database);
    QVERIFY(!books.saveDocument(projectRecord().id, original).has_value());
    database.reset();

    QVERIFY(executeRawSql(
        databasePath,
        QStringLiteral("UPDATE chapters SET chapter_index = 9 WHERE chapter_index = 1")));
    auto reopened = loreforge::storage::ProjectDatabase::open(databasePath);
    QVERIFY(std::holds_alternative<std::unique_ptr<loreforge::storage::ProjectDatabase>>(reopened));
    database = takeDatabase(reopened);
    loreforge::storage::BookRepository reopenedBooks(*database);
    const auto loaded = reopenedBooks.loadDocument(original.id);
    QVERIFY(std::holds_alternative<loreforge::storage::StorageError>(loaded));
    QCOMPARE(std::get<loreforge::storage::StorageError>(loaded).code,
             loreforge::storage::StorageErrorCode::CorruptData);
}

QTEST_GUILESS_MAIN(StorageIntegrationTest)

#include "storage_integration_test.moc"
