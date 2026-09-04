#include "document_fixture.h"

#include "loreforge/document/document_json_codec.h"
#include "loreforge/inference/inference_types.h"
#include "loreforge/inference/output_validator.h"
#include "loreforge/storage/book_repository.h"
#include "loreforge/storage/inference_repository.h"
#include "loreforge/storage/llm_run_repository.h"
#include "loreforge/storage/project_database.h"
#include "loreforge/storage/project_repository.h"

#include <QFile>
#include <QJsonArray>
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
    void migratesAnExistingVersionOneDatabase();
    void persistsLLMRunsAcrossReopen();
    void preservesInspectableInferenceSnapshotsAcrossReopen();
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
    QCOMPARE(database->schemaVersion(), 4);
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
    auto original = loreforge::test::handcraftedDocument();
    original.chapters[0].blocks[1].extractionConfidence = 0.84;

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
    QCOMPARE(database->schemaVersion(), 4);
    database.reset();

    QVERIFY(executeRawSql(databasePath,
                          QStringLiteral("INSERT INTO legacy_marker(value) VALUES('preserved')")));
}

void StorageIntegrationTest::migratesAnExistingVersionOneDatabase() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto databasePath = directory.filePath(QStringLiteral("version-one.loreforge"));
    const QStringList schema{
        QStringLiteral("CREATE TABLE schema_migrations(version INTEGER PRIMARY KEY NOT NULL, "
                       "name TEXT NOT NULL, applied_at TEXT NOT NULL)"),
        QStringLiteral("CREATE TABLE projects(id TEXT PRIMARY KEY NOT NULL, name TEXT NOT NULL, "
                       "created_at TEXT NOT NULL)"),
        QStringLiteral(
            "CREATE TABLE books(id TEXT PRIMARY KEY NOT NULL, project_id TEXT NOT NULL, "
            "title TEXT NOT NULL, authors_json TEXT NOT NULL, language TEXT NOT NULL, "
            "source_format TEXT NOT NULL, source_locator TEXT NOT NULL, source_hash TEXT "
            "NOT NULL, FOREIGN KEY(project_id) REFERENCES projects(id) ON DELETE CASCADE, "
            "UNIQUE(project_id, source_locator))"),
        QStringLiteral(
            "CREATE TABLE chapters(id TEXT PRIMARY KEY NOT NULL, book_id TEXT NOT NULL, "
            "chapter_index INTEGER NOT NULL CHECK(chapter_index >= 0), title TEXT NOT NULL, "
            "FOREIGN KEY(book_id) REFERENCES books(id) ON DELETE CASCADE, "
            "UNIQUE(book_id, chapter_index))"),
        QStringLiteral("CREATE TABLE blocks(chapter_id TEXT NOT NULL, block_index INTEGER NOT NULL "
                       "CHECK(block_index >= 0), block_type TEXT NOT NULL, text TEXT NOT NULL, "
                       "source_id TEXT NOT NULL, start_byte INTEGER NOT NULL, end_byte INTEGER NOT "
                       "NULL, PRIMARY KEY(chapter_id, block_index), FOREIGN KEY(chapter_id) "
                       "REFERENCES chapters(id) ON DELETE CASCADE)"),
        QStringLiteral(
            "CREATE INDEX blocks_source_span ON blocks(source_id, start_byte, end_byte)"),
        QStringLiteral("INSERT INTO schema_migrations VALUES(1, '001_initial', '2026-09-04')"),
        QStringLiteral("PRAGMA user_version = 1"),
    };
    for (const auto& statement : schema) {
        QVERIFY(executeRawSql(databasePath, statement));
    }

    auto opened = loreforge::storage::ProjectDatabase::open(databasePath);
    QVERIFY(std::holds_alternative<std::unique_ptr<loreforge::storage::ProjectDatabase>>(opened));
    auto database = takeDatabase(opened);
    QCOMPARE(database->schemaVersion(), 4);
    database.reset();
    QVERIFY(executeRawSql(databasePath,
                          QStringLiteral("SELECT extraction_confidence FROM blocks LIMIT 1")));
    QVERIFY(executeRawSql(databasePath, QStringLiteral("SELECT id FROM llm_runs LIMIT 1")));
    QVERIFY(executeRawSql(databasePath,
                          QStringLiteral("SELECT run_id FROM llm_run_artifacts LIMIT 1")));
}

void StorageIntegrationTest::persistsLLMRunsAcrossReopen() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto databasePath = directory.filePath(QStringLiteral("llm-runs.loreforge"));
    auto created = loreforge::storage::ProjectDatabase::create(databasePath);
    QVERIFY(std::holds_alternative<std::unique_ptr<loreforge::storage::ProjectDatabase>>(created));
    auto database = takeDatabase(created);
    loreforge::storage::ProjectRepository projects(*database);
    QVERIFY(!projects.create(projectRecord()).has_value());

    const QUuid requestId(QStringLiteral("{23627de9-5cf0-46aa-8f2e-a195e4473b19}"));
    const auto runId = loreforge::core::LLMRunId::fromStableKey(requestId.toString());
    loreforge::storage::LLMRunRepository runs(*database);
    const loreforge::storage::LLMRunRecord queued{
        runId,
        projectRecord().id,
        requestId,
        QStringLiteral("qwen"),
        QStringLiteral("qwen-fixture"),
        loreforge::storage::LLMRunStatus::Queued,
        0,
        0,
        0,
        0,
        QDateTime::fromString(QStringLiteral("2026-09-04T01:00:00.000Z"), Qt::ISODateWithMs),
        std::nullopt,
        std::nullopt,
        {},
        {}};
    const auto queuedSave = runs.save(queued);
    QVERIFY2(!queuedSave.has_value(),
             queuedSave.has_value()
                 ? qPrintable(queuedSave->message + QLatin1Char(' ') + queuedSave->technicalDetails)
                 : "");

    auto succeeded = queued;
    succeeded.status = loreforge::storage::LLMRunStatus::Succeeded;
    succeeded.attemptCount = 2;
    succeeded.promptTokens = 120;
    succeeded.completionTokens = 30;
    succeeded.totalTokens = 150;
    succeeded.completedAt =
        QDateTime::fromString(QStringLiteral("2026-09-04T01:00:01.250Z"), Qt::ISODateWithMs);
    succeeded.latencyMs = 1'250;
    const auto succeededSave = runs.save(succeeded);
    QVERIFY2(!succeededSave.has_value(),
             succeededSave.has_value() ? qPrintable(succeededSave->message + QLatin1Char(' ') +
                                                    succeededSave->technicalDetails)
                                       : "");
    database.reset();

    auto reopened = loreforge::storage::ProjectDatabase::open(databasePath);
    QVERIFY(std::holds_alternative<std::unique_ptr<loreforge::storage::ProjectDatabase>>(reopened));
    database = takeDatabase(reopened);
    loreforge::storage::LLMRunRepository reopenedRuns(*database);
    const auto loaded = reopenedRuns.find(runId);
    QVERIFY(std::holds_alternative<loreforge::storage::LLMRunRecord>(loaded));
    QCOMPARE(std::get<loreforge::storage::LLMRunRecord>(loaded), succeeded);
    const auto listed = reopenedRuns.listForProject(projectRecord().id);
    QVERIFY(std::holds_alternative<QList<loreforge::storage::LLMRunRecord>>(listed));
    QCOMPARE(std::get<QList<loreforge::storage::LLMRunRecord>>(listed),
             QList<loreforge::storage::LLMRunRecord>{succeeded});
}

void StorageIntegrationTest::preservesInspectableInferenceSnapshotsAcrossReopen() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto databasePath = directory.filePath(QStringLiteral("inference-snapshot.loreforge"));
    auto created = loreforge::storage::ProjectDatabase::create(databasePath);
    QVERIFY(std::holds_alternative<std::unique_ptr<loreforge::storage::ProjectDatabase>>(created));
    auto database = takeDatabase(created);
    loreforge::storage::ProjectRepository projects(*database);
    QVERIFY(!projects.create(projectRecord()).has_value());

    const auto createdAt = QDateTime::fromString(u"2026-09-04T02:00:00.000Z"_s, Qt::ISODateWithMs);
    const QUuid requestId(u"{63a3ebf3-638b-4bf4-b00a-cbddbe4cf403}"_s);
    const auto runId = loreforge::core::LLMRunId::fromStableKey(requestId.toString());
    loreforge::storage::LLMRunRepository runs(*database);
    const loreforge::storage::LLMRunRecord run{runId,
                                               projectRecord().id,
                                               requestId,
                                               u"qwen"_s,
                                               u"qwen-fixture"_s,
                                               loreforge::storage::LLMRunStatus::Succeeded,
                                               1,
                                               11,
                                               4,
                                               15,
                                               createdAt,
                                               createdAt.addMSecs(320),
                                               320,
                                               {},
                                               {}};
    QVERIFY(!runs.save(run).has_value());

    const auto prompt = loreforge::inference::makePromptVersion(
        {loreforge::core::PromptTemplateId::fromStableKey(u"fixture-extraction"_s),
         u"Fixture extraction"_s},
        1, u"Return one named fixture."_s, createdAt);
    const QJsonObject schemaJson{
        {u"$schema"_s, u"https://json-schema.org/draft/2020-12/schema"_s},
        {u"type"_s, u"object"_s},
        {u"required"_s, QJsonArray{u"name"_s}},
        {u"properties"_s, QJsonObject{{u"name"_s, QJsonObject{{u"type"_s, u"string"_s}}}}},
        {u"additionalProperties"_s, false},
    };
    const auto outputSchema = loreforge::inference::makeOutputSchema(
        loreforge::core::OutputSchemaId::fromStableKey(u"fixture-output"_s), u"Fixture output"_s, 1,
        schemaJson, createdAt);
    const auto contextSnapshot = loreforge::inference::makeContextSnapshot(
        loreforge::core::ContextSnapshotId::fromStableKey(u"fixture-context"_s), projectRecord().id,
        QJsonObject{{u"source"_s, u"chapter-1"_s}, {u"text"_s, u"Lin entered."_s}}, createdAt);
    const QByteArray rawRequest = R"({"model":"qwen-fixture","messages":[]})";
    const QByteArray rawResponse = R"({"choices":[{"message":{"content":"{\"name\":\"Lin\"}"}}]})";
    const QJsonDocument parsedResponse(QJsonObject{{u"name"_s, u"Lin"_s}});
    const auto validation = loreforge::inference::OutputValidator::validate(
        outputSchema.schema, parsedResponse.object());
    QVERIFY(validation.isValid());

    loreforge::storage::InferenceRepository inference(*database);
    QVERIFY(!inference.savePromptVersion(prompt).has_value());
    QVERIFY(!inference.saveOutputSchema(outputSchema).has_value());
    QVERIFY(!inference.saveContextSnapshot(contextSnapshot).has_value());
    const loreforge::inference::LLMRunArtifacts pending{
        runId,
        prompt.prompt.id,
        prompt.version,
        outputSchema.id,
        outputSchema.version,
        contextSnapshot.id,
        rawRequest,
        std::nullopt,
        std::nullopt,
        {loreforge::inference::ValidationStatus::Pending, {}},
    };
    QVERIFY(!inference.createRunArtifacts(pending).has_value());
    QVERIFY(!inference.finalizeRunArtifacts(runId, rawResponse, parsedResponse, validation)
                 .has_value());
    const auto duplicatePrompt = inference.savePromptVersion(prompt);
    QVERIFY(duplicatePrompt.has_value());
    QCOMPARE(duplicatePrompt->code, loreforge::storage::StorageErrorCode::Conflict);
    database.reset();

    auto reopened = loreforge::storage::ProjectDatabase::open(databasePath);
    QVERIFY(std::holds_alternative<std::unique_ptr<loreforge::storage::ProjectDatabase>>(reopened));
    database = takeDatabase(reopened);
    loreforge::storage::InferenceRepository reopenedInference(*database);
    const auto inspected = reopenedInference.inspectRun(runId);
    QVERIFY(std::holds_alternative<loreforge::storage::StoredInferenceSnapshot>(inspected));
    const auto& snapshot = std::get<loreforge::storage::StoredInferenceSnapshot>(inspected);
    QVERIFY(snapshot.promptVersion == prompt);
    QVERIFY(snapshot.outputSchema == outputSchema);
    QVERIFY(snapshot.contextSnapshot == contextSnapshot);
    QCOMPARE(snapshot.runArtifacts.rawRequest, rawRequest);
    QVERIFY(snapshot.runArtifacts.rawResponse.has_value());
    QCOMPARE(*snapshot.runArtifacts.rawResponse, rawResponse);
    QVERIFY(snapshot.runArtifacts.parsedResponse.has_value());
    QCOMPARE(*snapshot.runArtifacts.parsedResponse, parsedResponse);
    QCOMPARE(snapshot.runArtifacts.validation, validation);
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
