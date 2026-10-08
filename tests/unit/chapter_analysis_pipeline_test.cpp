#include "chapter_analysis_controller.h"
#include "main_window.h"
#include "markdown_source_fixture.h"
#include "novel_analysis_workbench.h"

#include "loreforge/parser/markdown_source_parser.h"
#include "loreforge/storage/book_repository.h"
#include "loreforge/storage/project_repository.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFontDatabase>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTabWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeWidget>
#include <QtTest>

namespace {
using namespace loreforge;
using Controller = app::ChapterAnalysisController;
using Database = storage::ProjectDatabase;
struct Fixture {
    QTemporaryDir source;
    QTemporaryDir destination;
    document::Document book;
    app::ChapterPreview preview;
    core::ProjectId projectId = core::ProjectId::fromStableKey(QStringLiteral("pipeline-fixture"));
    QString path;
    bool initialize(const QByteArray& chapter = test::markdownFixtureChapter()) {
        if (!test::createMarkdownFixture(source.path())) {
            return false;
        }
        if (!test::writeMarkdownFixtureFile(source.path(), QStringLiteral("02/010.md"), chapter)) {
            return false;
        }
        auto parsed = parser::MarkdownSourceParser::parseDirectory(source.path());
        if (!std::holds_alternative<parser::MarkdownSourceImport>(parsed)) {
            return false;
        }
        book = std::get<parser::MarkdownSourceImport>(parsed).document;
        path = destination.filePath(QStringLiteral("pipeline.loreforge"));
        auto created = Database::create(path);
        if (!std::holds_alternative<std::unique_ptr<Database>>(created)) {
            return false;
        }
        auto database = std::get<std::unique_ptr<Database>>(std::move(created));
        storage::ProjectRepository projects(*database);
        storage::BookRepository books(*database);
        if (projects.create(
                {projectId, QStringLiteral("Pipeline fixture"), QDateTime::currentDateTimeUtc()}) ||
            books.saveDocument(projectId, book)) {
            return false;
        }
        auto built = app::ChapterPreviewBuilder::build(projectId, book, 0, 32768, 4096);
        if (!std::holds_alternative<app::ChapterPreview>(built)) {
            return false;
        }
        auto saved =
            app::ChapterPreviewBuilder::store(*database, std::get<app::ChapterPreview>(built));
        if (!std::holds_alternative<app::ChapterPreview>(saved)) {
            return false;
        }
        preview = std::get<app::ChapterPreview>(saved);
        return true;
    }
};

QJsonObject analysisFor(const QJsonObject& user) {
    const auto range = user.value(QStringLiteral("source_ranges")).toArray().at(1).toObject();
    const QJsonArray evidence{
        QJsonObject{{QStringLiteral("source_start"), range.value(QStringLiteral("source_start"))},
                    {QStringLiteral("source_end"), range.value(QStringLiteral("source_end"))}}};
    const QJsonObject support{{QStringLiteral("inferred"), false},
                              {QStringLiteral("confidence"), 0.9},
                              {QStringLiteral("evidence"), evidence}};
    auto summary = support;
    summary.insert(QStringLiteral("text"), QStringLiteral("她打开门，窗外有灯。"));
    auto character = support;
    character.insert(QStringLiteral("name"), QStringLiteral("她"));
    character.insert(QStringLiteral("aliases"), QJsonArray{});
    return {{QStringLiteral("chapter_id"), user.value(QStringLiteral("chapter_id"))},
            {QStringLiteral("characters"), QJsonArray{character}},
            {QStringLiteral("locations"), QJsonArray{}},
            {QStringLiteral("events"), QJsonArray{}},
            {QStringLiteral("summary"), summary},
            {QStringLiteral("important_facts"), QJsonArray{summary}},
            {QStringLiteral("open_threads"), QJsonArray{}}};
}
QJsonObject reviewFor(const QJsonObject& user, const QString& verdict = QStringLiteral("pass")) {
    QJsonArray issues;
    if (verdict != QStringLiteral("pass")) {
        const auto range = user.value(QStringLiteral("source_ranges")).toArray().at(1).toObject();
        issues.append(QJsonObject{
            {QStringLiteral("reason"), QStringLiteral("这项结论需再核对。")},
            {QStringLiteral("source_start"), range.value(QStringLiteral("source_start"))},
            {QStringLiteral("source_end"), range.value(QStringLiteral("source_end"))}});
    }
    return {{QStringLiteral("chapter_id"), user.value(QStringLiteral("chapter_id"))},
            {QStringLiteral("verdict"), verdict},
            {QStringLiteral("issues"), issues}};
}

class Server final : public QObject {
  public:
    Server() {
        const auto listening = server.listen(QHostAddress::LocalHost, 0);
        Q_ASSERT(listening);
        connect(&server, &QTcpServer::newConnection, this, [this] {
            while (auto* socket = server.nextPendingConnection()) {
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
                    auto& bytes = buffers[socket];
                    bytes += socket->readAll();
                    const auto boundary = bytes.indexOf("\r\n\r\n");
                    if (boundary < 0) {
                        return;
                    }
                    qsizetype length = 0;
                    for (const auto& line : bytes.first(boundary).split('\n')) {
                        if (line.toLower().startsWith("content-length:")) {
                            length = line.sliced(line.indexOf(':') + 1).trimmed().toLongLong();
                        }
                    }
                    if (bytes.size() < boundary + 4 + length) {
                        return;
                    }
                    disconnect(socket, &QTcpSocket::readyRead, this, nullptr);
                    rawRequests.append(bytes.mid(boundary + 4, length));
                    const auto request = QJsonDocument::fromJson(rawRequests.last()).object();
                    requests.append(request);
                    if (!respond) {
                        return;
                    }
                    const auto user =
                        QJsonDocument::fromJson(request.value(QStringLiteral("messages"))
                                                    .toArray()[1]
                                                    .toObject()
                                                    .value(QStringLiteral("content"))
                                                    .toString()
                                                    .toUtf8())
                            .object();
                    const auto index = requests.size() - 1;
                    const auto output = makeOutput ? makeOutput(user, static_cast<int>(index))
                                                   : (user.contains(QStringLiteral("analysis"))
                                                          ? reviewFor(user)
                                                          : analysisFor(user));
                    QJsonObject response{
                        {QStringLiteral("model"), QStringLiteral("fixture-model")},
                        {QStringLiteral("choices"),
                         QJsonArray{QJsonObject{
                             {QStringLiteral("message"),
                              QJsonObject{{QStringLiteral("content"),
                                           QString::fromUtf8(QJsonDocument(output).toJson(
                                               QJsonDocument::Compact))}}},
                             {QStringLiteral("finish_reason"), finishReason}}}}};
                    if (withUsage) {
                        response.insert(QStringLiteral("usage"),
                                        QJsonObject{{QStringLiteral("prompt_tokens"), 10},
                                                    {QStringLiteral("completion_tokens"), 4},
                                                    {QStringLiteral("total_tokens"), 14}});
                    }
                    const auto body =
                        status == 200
                            ? QJsonDocument(response).toJson(QJsonDocument::Compact)
                            : QByteArray(
                                  "{\"error\":{\"message\":\"credential-must-not-persist\"}}");
                    socket->write(QByteArray("HTTP/1.1 ") + QByteArray::number(status) +
                                  " Test\r\nContent-Type: application/json\r\nConnection: "
                                  "close\r\nContent-Length: " +
                                  QByteArray::number(body.size()) + "\r\n\r\n" + body);
                    socket->disconnectFromHost();
                });
            }
        });
    }
    ~Server() override {
        for (auto* socket : server.findChildren<QTcpSocket*>()) {
            disconnect(socket, nullptr, this, nullptr);
            socket->abort();
        }
    }
    app::AnalysisConnection connection() const {
        return {{QUrl(QStringLiteral("http://127.0.0.1:%1/v1/chat/completions")
                          .arg(server.serverPort())),
                 {},
                 QStringLiteral("fixture-model"),
                 true,
                 llm::CompletionTokenParameter::MaxTokens},
                QStringLiteral("json_object"),
                3000};
    }
    QTcpServer server;
    QHash<QTcpSocket*, QByteArray> buffers;
    QList<QJsonObject> requests;
    QList<QByteArray> rawRequests;
    std::function<QJsonObject(const QJsonObject&, int)> makeOutput;
    bool respond = true;
    bool withUsage = true;
    int status = 200;
    QString finishReason = QStringLiteral("stop");
};
QList<app::AnalysisHistoryEntry> history(const Fixture& fixture) {
    auto loaded = Controller::history(fixture.path, fixture.projectId);
    return std::holds_alternative<QList<app::AnalysisHistoryEntry>>(loaded)
               ? std::get<QList<app::AnalysisHistoryEntry>>(loaded)
               : QList<app::AnalysisHistoryEntry>{};
}
} // namespace

class ChapterAnalysisPipelineTest final : public QObject {
    Q_OBJECT
  private slots:
    void runsThreeRolesAndPersistsHistory();
    void refusesInvalidEvidenceAndTruncation_data();
    void refusesInvalidEvidenceAndTruncation();
    void handlesDisagreementWithoutAutomaticApproval();
    void cancelsAndDoesNotRetryFailures();
    void rejectsSourceChangesBeforeAndDuringRequests();
    void refusesReviewInputAboveBudget();
    void requiresConfirmationAndRestoresTheAnalysisPage();
    void isolatesRapidCancelAndRestart();
    void rejectsBadReviewAndReflectedCredentials();
    void preservesUnknownUsageAndHandlesTimeout();
    void refusesTamperedSnapshots();
    void locatesRepeatedEvidenceByByteRange();
    void closingTheWindowCancelsThePendingRun();
};

void ChapterAnalysisPipelineTest::runsThreeRolesAndPersistsHistory() {
    Fixture fixture;
    QVERIFY(fixture.initialize());
    Server server;
    Controller controller;
    QSignalSpy finished(&controller, &Controller::finished);
    controller.start(fixture.path, fixture.book, 0, fixture.preview, server.connection());
    QVERIFY(controller.busy());
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
    QVERIFY2(finished[0][0].toString().contains(QStringLiteral("通过")),
             qPrintable(finished[0][0].toString()));
    QCOMPARE(server.requests.size(), 3);
    const auto entries = history(fixture);
    QCOMPARE(entries.size(), 3);
    for (qsizetype index = 0; index < entries.size(); ++index) {
        QCOMPARE(entries[index].run.status, storage::LLMRunStatus::Succeeded);
        QCOMPARE(entries[index].artifacts.rawRequest, server.rawRequests[index]);
        QCOMPARE(entries[index].run.totalTokens, 14);
        QVERIFY(entries[index].outcome.value(QStringLiteral("usage_known")).toBool());
        QCOMPARE(entries[index].context.content.value(QStringLiteral("role_index")).toInt(),
                 static_cast<int>(index));
        QCOMPARE(server.requests[index].value(QStringLiteral("model")).toString(),
                 QStringLiteral("fixture-model"));
        QVERIFY(server.requests[index].contains(QStringLiteral("max_tokens")));
        QVERIFY(!server.rawRequests[index].contains("Second file."));
        QVERIFY(!server.rawRequests[index].contains(fixture.source.path().toUtf8()));
    }
    auto user = QJsonDocument::fromJson(server.requests[2]
                                            .value(QStringLiteral("messages"))
                                            .toArray()[1]
                                            .toObject()
                                            .value(QStringLiteral("content"))
                                            .toString()
                                            .toUtf8())
                    .object();
    QVERIFY(user.contains(QStringLiteral("analysis")));
    QVERIFY(user.contains(QStringLiteral("checker")));
}

void ChapterAnalysisPipelineTest::refusesInvalidEvidenceAndTruncation_data() {
    QTest::addColumn<int>("mode");
    QTest::newRow("out-of-range") << 0;
    QTest::newRow("utf8-split") << 1;
    QTest::newRow("wrong-chapter") << 2;
    QTest::newRow("truncated") << 3;
    QTest::newRow("bad-schema") << 4;
}
void ChapterAnalysisPipelineTest::refusesInvalidEvidenceAndTruncation() {
    QFETCH(int, mode);
    Fixture fixture;
    QVERIFY(fixture.initialize());
    Server server;
    server.makeOutput = [mode](const QJsonObject& user, int) {
        auto output = analysisFor(user);
        auto summary = output.value(QStringLiteral("summary")).toObject();
        auto evidence = summary.value(QStringLiteral("evidence")).toArray();
        auto range = evidence[0].toObject();
        if (mode == 0) {
            range.insert(QStringLiteral("source_end"), 999999);
        }
        if (mode == 1) {
            range.insert(QStringLiteral("source_start"),
                         range.value(QStringLiteral("source_start")).toInteger() + 1);
        }
        evidence[0] = range;
        summary.insert(QStringLiteral("evidence"), evidence);
        output.insert(QStringLiteral("summary"), summary);
        if (mode == 2) {
            output.insert(QStringLiteral("chapter_id"), QStringLiteral("wrong"));
        }
        if (mode == 4) {
            output.insert(QStringLiteral("extra"), true);
        }
        return output;
    };
    if (mode == 3) {
        server.finishReason = QStringLiteral("length");
    }
    Controller controller;
    QSignalSpy finished(&controller, &Controller::finished);
    controller.start(fixture.path, fixture.book, 0, fixture.preview, server.connection());
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
    QCOMPARE(server.requests.size(), 1);
    const auto entries = history(fixture);
    QCOMPARE(entries.size(), 1);
    QCOMPARE(entries[0].run.status, storage::LLMRunStatus::Failed);
    QVERIFY(!entries[0].outcome.value(QStringLiteral("domain_errors")).toArray().isEmpty());
}

void ChapterAnalysisPipelineTest::handlesDisagreementWithoutAutomaticApproval() {
    Fixture fixture;
    QVERIFY(fixture.initialize());
    Server server;
    server.makeOutput = [](const QJsonObject& user, int index) {
        return index == 0
                   ? analysisFor(user)
                   : reviewFor(user, index == 1 ? QStringLiteral("redo") : QStringLiteral("pass"));
    };
    Controller controller;
    QSignalSpy finished(&controller, &Controller::finished);
    controller.start(fixture.path, fixture.book, 0, fixture.preview, server.connection());
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
    QCOMPARE(server.requests.size(), 3);
    QCOMPARE(finished[0][0].toString(), QStringLiteral("待人工确认"));
}

void ChapterAnalysisPipelineTest::cancelsAndDoesNotRetryFailures() {
    Fixture fixture;
    QVERIFY(fixture.initialize());
    Server server;
    server.respond = false;
    Controller controller;
    QSignalSpy finished(&controller, &Controller::finished);
    controller.start(fixture.path, fixture.book, 0, fixture.preview, server.connection());
    QTRY_COMPARE_WITH_TIMEOUT(server.requests.size(), 1, 3000);
    controller.cancel();
    QTRY_COMPARE(finished.size(), 1);
    auto entries = history(fixture);
    QCOMPARE(entries.size(), 1);
    QCOMPARE(entries[0].run.status, storage::LLMRunStatus::Cancelled);
    QVERIFY(!entries[0].outcome.value(QStringLiteral("usage_known")).toBool());
    QVERIFY(!entries[0].artifacts.rawResponse);
    server.respond = true;
    server.status = 500;
    auto connection = server.connection();
    connection.options.apiKey = QByteArray("credential-must-not-persist");
    controller.start(fixture.path, fixture.book, 0, fixture.preview, connection);
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 2, 5000);
    QCOMPARE(server.requests.size(), 2);
    entries = history(fixture);
    QCOMPARE(entries.size(), 2);
    QCOMPARE(entries.last().run.status, storage::LLMRunStatus::Failed);
    QCOMPARE(entries.last().run.attemptCount, 1);
    QVERIFY(!entries.last().artifacts.rawResponse);
    QVERIFY(
        !entries.last().run.errorMessage.contains(QStringLiteral("credential-must-not-persist")));
}

void ChapterAnalysisPipelineTest::rejectsSourceChangesBeforeAndDuringRequests() {
    Fixture fixture;
    QVERIFY(fixture.initialize());
    Server server;
    Controller controller;
    QSignalSpy finished(&controller, &Controller::finished);
    QVERIFY(test::writeMarkdownFixtureFile(fixture.source.path(), QStringLiteral("02/010.md"),
                                           QByteArray("# Changed\n\nDifferent text.\n")));
    controller.start(fixture.path, fixture.book, 0, fixture.preview, server.connection());
    QTRY_COMPARE(finished.size(), 1);
    QCOMPARE(server.requests.size(), 0);
    QVERIFY(test::writeMarkdownFixtureFile(fixture.source.path(), QStringLiteral("02/010.md"),
                                           test::markdownFixtureChapter()));
    server.makeOutput = [&fixture](const QJsonObject& user, int) {
        const auto saved =
            test::writeMarkdownFixtureFile(fixture.source.path(), QStringLiteral("02/010.md"),
                                           QByteArray("# Changed\n\nDifferent text.\n"));
        Q_ASSERT(saved);
        return analysisFor(user);
    };
    controller.start(fixture.path, fixture.book, 0, fixture.preview, server.connection());
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 2, 5000);
    QCOMPARE(server.requests.size(), 1);
    const auto entries = history(fixture);
    QCOMPARE(entries.size(), 1);
    QCOMPARE(entries[0].run.status, storage::LLMRunStatus::Failed);
    QVERIFY(entries[0].run.errorMessage.contains(QStringLiteral("过时")));
}

void ChapterAnalysisPipelineTest::refusesReviewInputAboveBudget() {
    Fixture fixture;
    QVERIFY(fixture.initialize());
    Server server;
    server.makeOutput = [](const QJsonObject& user, int) {
        auto output = analysisFor(user);
        auto summary = output.value(QStringLiteral("summary")).toObject();
        summary.insert(QStringLiteral("text"), QString(40000, QChar(0x4e2d)));
        output.insert(QStringLiteral("summary"), summary);
        return output;
    };
    Controller controller;
    QSignalSpy finished(&controller, &Controller::finished);
    controller.start(fixture.path, fixture.book, 0, fixture.preview, server.connection());
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
    QVERIFY(finished[0][0].toString().contains(QStringLiteral("超出预算")));
    QCOMPARE(server.requests.size(), 1);
}

void ChapterAnalysisPipelineTest::requiresConfirmationAndRestoresTheAnalysisPage() {
    Fixture fixture;
    QVERIFY(fixture.initialize());
    Server server;
    {
        app::MainWindow window;
        QVERIFY(window.openProjectFile(fixture.path));
        auto* llm = window.findChild<QWidget*>(QStringLiteral("llmWorkbench"));
        llm->findChild<QLineEdit*>(QStringLiteral("llmEndpoint"))
            ->setText(server.connection().options.endpoint.toString());
        llm->findChild<QLineEdit*>(QStringLiteral("llmModelId"))
            ->setText(QStringLiteral("fixture-model"));
        llm->findChild<QCheckBox*>(QStringLiteral("llmAllowHttp"))->setChecked(true);
        llm->findChild<QComboBox*>(QStringLiteral("llmResponseFormat"))
            ->setCurrentText(QStringLiteral("json_object"));
        auto* prepare = window.findChild<QPushButton*>(QStringLiteral("llmPrepareChapterPreview"));
        auto* analyze = window.findChild<QPushButton*>(QStringLiteral("llmAnalyzeChapter"));
        QVERIFY(!analyze->isEnabled());
        prepare->click();
        QVERIFY(analyze->isEnabled());
        QTimer::singleShot(0, &window, [&window] {
            auto* dialog =
                window.findChild<QMessageBox*>(QStringLiteral("analysisSendConfirmation"));
            QVERIFY(dialog);
            dialog->reject();
        });
        analyze->click();
        QCOMPARE(server.requests.size(), 0);
        QTimer::singleShot(0, &window, [&window] {
            auto* dialog =
                window.findChild<QMessageBox*>(QStringLiteral("analysisSendConfirmation"));
            QVERIFY(dialog);
            dialog->findChild<QPushButton*>(QStringLiteral("analysisConfirmSend"))->click();
        });
        analyze->click();
        auto* status = window.findChild<QLabel*>(QStringLiteral("analysisTaskStatus"));
        QTRY_VERIFY_WITH_TIMEOUT(status->text().contains(QStringLiteral("通过")), 5000);
        QVERIFY(!window.findChild<QLabel*>(QStringLiteral("llmChapterPreviewStatus"))
                     ->text()
                     .contains(QStringLiteral("准备失败")));
        QCOMPARE(server.requests.size(), 3);
        QCOMPARE(window.findChild<QListWidget*>(QStringLiteral("analysisHistory"))->count(), 3);
        QVERIFY(window.findChild<QLabel*>(QStringLiteral("analysisTokenTotals"))
                    ->text()
                    .contains(QStringLiteral("42 token")));
        auto* selector = window.findChild<QComboBox*>(QStringLiteral("analysisChapterSelector"));
        selector->setCurrentIndex(1);
        QCOMPARE(window.findChild<QListWidget*>(QStringLiteral("analysisHistory"))->count(), 0);
        QVERIFY(!analyze->isEnabled());
        selector->setCurrentIndex(0);
        QVERIFY(!analyze->isEnabled());
        if (const auto path = qEnvironmentVariable("LOREFORGE_ANALYSIS_PREVIEW_IMAGE");
            !path.isEmpty()) {
            const auto id =
                QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/msyh.ttc"));
            const auto families = QFontDatabase::applicationFontFamilies(id);
            if (!families.isEmpty()) {
                window.setFont(QFont(families.first(), 10));
            }
            window.findChild<QTabWidget*>(QStringLiteral("workspacePages"))->setCurrentIndex(2);
            window.resize(1400, 1200);
            window.show();
            QTest::qWait(30);
            QVERIFY(window.grab().save(path));
        }
    }
    app::MainWindow reopened;
    QVERIFY(reopened.openProjectFile(fixture.path));
    QCOMPARE(reopened.findChild<QListWidget*>(QStringLiteral("analysisHistory"))->count(), 3);
    QVERIFY(reopened.findChild<QLabel*>(QStringLiteral("analysisTaskStatus"))
                ->text()
                .contains(QStringLiteral("通过")));
    QVERIFY(!reopened.findChild<QPushButton*>(QStringLiteral("llmAnalyzeChapter"))->isEnabled());
    QCOMPARE(server.requests.size(), 3);
}

void ChapterAnalysisPipelineTest::isolatesRapidCancelAndRestart() {
    Fixture fixture;
    QVERIFY(fixture.initialize());
    Server server;
    Controller controller;
    QSignalSpy finished(&controller, &Controller::finished);
    controller.start(fixture.path, fixture.book, 0, fixture.preview, server.connection());
    controller.cancel();
    QCOMPARE(finished.size(), 1);
    controller.start(fixture.path, fixture.book, 0, fixture.preview, server.connection());
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 2, 5000);
    QVERIFY(finished[1][0].toString().contains(QStringLiteral("通过")));
    QCOMPARE(server.requests.size(), 3);
    QCOMPARE(history(fixture).size(), 3);
}

void ChapterAnalysisPipelineTest::rejectsBadReviewAndReflectedCredentials() {
    Fixture fixture;
    QVERIFY(fixture.initialize());
    Server server;
    server.makeOutput = [](const QJsonObject& user, int index) {
        if (index == 0) {
            return analysisFor(user);
        }
        auto output = reviewFor(user, QStringLiteral("redo"));
        auto issues = output.value(QStringLiteral("issues")).toArray();
        auto issue = issues[0].toObject();
        issue.insert(QStringLiteral("source_end"), 999999);
        issues[0] = issue;
        output.insert(QStringLiteral("issues"), issues);
        return output;
    };
    Controller controller;
    QSignalSpy finished(&controller, &Controller::finished);
    controller.start(fixture.path, fixture.book, 0, fixture.preview, server.connection());
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
    QCOMPARE(server.requests.size(), 2);
    QCOMPARE(history(fixture).last().run.status, storage::LLMRunStatus::Failed);
    server.makeOutput = [](const QJsonObject& user, int) {
        auto output = analysisFor(user);
        auto summary = output.value(QStringLiteral("summary")).toObject();
        summary.insert(QStringLiteral("text"), QStringLiteral("credential-must-not-persist"));
        output.insert(QStringLiteral("summary"), summary);
        return output;
    };
    auto connection = server.connection();
    connection.options.apiKey = QByteArray("credential-must-not-persist");
    controller.start(fixture.path, fixture.book, 0, fixture.preview, connection);
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 2, 5000);
    QCOMPARE(server.requests.size(), 3);
    const auto entries = history(fixture);
    QCOMPARE(entries.size(), 3);
    QVERIFY(!entries.last().artifacts.rawResponse);
    QVERIFY(!entries.last().artifacts.parsedResponse);
    QVERIFY(
        !entries.last().run.errorMessage.contains(QStringLiteral("credential-must-not-persist")));
}

void ChapterAnalysisPipelineTest::preservesUnknownUsageAndHandlesTimeout() {
    Fixture fixture;
    QVERIFY(fixture.initialize());
    Server server;
    server.withUsage = false;
    Controller controller;
    QSignalSpy finished(&controller, &Controller::finished);
    controller.start(fixture.path, fixture.book, 0, fixture.preview, server.connection());
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
    auto entries = history(fixture);
    QCOMPARE(entries.size(), 3);
    for (const auto& entry : entries) {
        QVERIFY(!entry.outcome.value(QStringLiteral("usage_known")).toBool());
    }
    app::NovelAnalysisWorkbench page;
    page.setSelectedBook(&fixture.book, 0);
    page.showHistory(entries, fixture.book.chapters[0].id.toString());
    QVERIFY(page.findChild<QLabel*>(QStringLiteral("analysisTokenTotals"))
                ->text()
                .contains(QStringLiteral("3 次用量未知")));
    server.respond = false;
    auto connection = server.connection();
    connection.timeoutMs = 80;
    controller.start(fixture.path, fixture.book, 0, fixture.preview, connection);
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 2, 5000);
    QCOMPARE(server.requests.size(), 4);
    entries = history(fixture);
    QCOMPARE(entries.size(), 4);
    QCOMPARE(entries.last().run.status, storage::LLMRunStatus::Failed);
    QCOMPARE(entries.last().run.errorCode, QStringLiteral("timeout"));
}

void ChapterAnalysisPipelineTest::refusesTamperedSnapshots() {
    Fixture fixture;
    QVERIFY(fixture.initialize());
    Server server;
    Controller controller;
    QSignalSpy finished(&controller, &Controller::finished);
    auto tampered = fixture.preview;
    tampered.snapshot.content.insert(QStringLiteral("sent"), true);
    controller.start(fixture.path, fixture.book, 0, tampered, server.connection());
    QCOMPARE(finished.size(), 1);
    QVERIFY(!controller.busy());
    QCOMPARE(server.requests.size(), 0);
}

void ChapterAnalysisPipelineTest::locatesRepeatedEvidenceByByteRange() {
    Fixture fixture;
    QVERIFY(fixture.initialize(QStringLiteral("# 重复\r\n\r\n重复。\r\n\r\n重复。\r\n").toUtf8()));
    Server server;
    server.makeOutput = [](const QJsonObject& user, int index) {
        if (index > 0) {
            return reviewFor(user);
        }
        auto output = analysisFor(user);
        auto summary = output.value(QStringLiteral("summary")).toObject();
        const auto range = user.value(QStringLiteral("source_ranges")).toArray().at(2).toObject();
        summary.insert(
            QStringLiteral("evidence"),
            QJsonArray{QJsonObject{
                {QStringLiteral("source_start"), range.value(QStringLiteral("source_start"))},
                {QStringLiteral("source_end"), range.value(QStringLiteral("source_end"))}}});
        output.insert(QStringLiteral("summary"), summary);
        return output;
    };
    Controller controller;
    QSignalSpy finished(&controller, &Controller::finished);
    controller.start(fixture.path, fixture.book, 0, fixture.preview, server.connection());
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
    QVERIFY(finished[0][0].toString().contains(QStringLiteral("通过")));
    app::NovelAnalysisWorkbench page;
    page.setSelectedBook(&fixture.book, 0);
    page.showHistory(history(fixture), fixture.book.chapters[0].id.toString());
    auto* tabs = page.findChild<QTabWidget*>(QStringLiteral("analysisResultTabs"));
    auto* tree = qobject_cast<QTreeWidget*>(tabs->widget(1));
    QVERIFY(tree);
    QVERIFY(tree->topLevelItem(0)->childCount() > 0);
    tree->setCurrentItem(tree->topLevelItem(0)->child(0));
    auto* source = page.findChild<QPlainTextEdit*>(QStringLiteral("llmChapterSourcePreview"));
    QCOMPARE(source->textCursor().selectionStart(),
             static_cast<int>(source->toPlainText().lastIndexOf(QStringLiteral("重复。"))));
    QCOMPARE(page.findChild<QPlainTextEdit*>(QStringLiteral("analysisEvidence"))->toPlainText(),
             QStringLiteral("重复。"));
}

void ChapterAnalysisPipelineTest::closingTheWindowCancelsThePendingRun() {
    Fixture fixture;
    QVERIFY(fixture.initialize());
    Server server;
    server.respond = false;
    auto window = std::make_unique<app::MainWindow>();
    QVERIFY(window->openProjectFile(fixture.path));
    auto* llm = window->findChild<QWidget*>(QStringLiteral("llmWorkbench"));
    llm->findChild<QLineEdit*>(QStringLiteral("llmEndpoint"))
        ->setText(server.connection().options.endpoint.toString());
    llm->findChild<QLineEdit*>(QStringLiteral("llmModelId"))
        ->setText(QStringLiteral("fixture-model"));
    llm->findChild<QCheckBox*>(QStringLiteral("llmAllowHttp"))->setChecked(true);
    llm->findChild<QComboBox*>(QStringLiteral("llmResponseFormat"))
        ->setCurrentText(QStringLiteral("json_object"));
    window->findChild<QPushButton*>(QStringLiteral("llmPrepareChapterPreview"))->click();
    QTimer::singleShot(0, window.get(), [&window] {
        auto* dialog = window->findChild<QMessageBox*>(QStringLiteral("analysisSendConfirmation"));
        QVERIFY(dialog);
        dialog->findChild<QPushButton*>(QStringLiteral("analysisConfirmSend"))->click();
    });
    window->findChild<QPushButton*>(QStringLiteral("llmAnalyzeChapter"))->click();
    QTRY_COMPARE_WITH_TIMEOUT(server.requests.size(), 1, 3000);
    QVERIFY(!llm->findChild<QPushButton*>(QStringLiteral("llmTestConnection"))->isEnabled());
    window.reset();
    const auto entries = history(fixture);
    QCOMPARE(entries.size(), 1);
    QCOMPARE(entries[0].run.status, storage::LLMRunStatus::Cancelled);
}

QTEST_MAIN(ChapterAnalysisPipelineTest)
#include "chapter_analysis_pipeline_test.moc"
