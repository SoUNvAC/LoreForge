#include "llm_workbench.h"
#include "loreforge/storage/project_database.h"
#include "loreforge/storage/project_repository.h"
#include "main_window.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDockWidget>
#include <QFontDatabase>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTabWidget>
#include <QTableWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

namespace {
class ProbeServer final : public QObject {
  public:
    ProbeServer() {
        const auto listening = server.listen(QHostAddress::LocalHost, 0);
        Q_ASSERT(listening);
        connect(&server, &QTcpServer::newConnection, this, [this] {
            while (auto* socket = server.nextPendingConnection()) {
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
                    auto& bytes = buffers[socket];
                    bytes += socket->readAll();
                    const auto headerEnd = bytes.indexOf("\r\n\r\n");
                    if (headerEnd < 0) {
                        return;
                    }
                    qsizetype length = 0;
                    for (const auto& header : bytes.first(headerEnd).split('\n')) {
                        if (header.toLower().startsWith("content-length:")) {
                            length = header.sliced(header.indexOf(':') + 1).trimmed().toLongLong();
                        }
                    }
                    if (bytes.size() < headerEnd + 4 + length) {
                        return;
                    }
                    requests.append(bytes);
                    disconnect(socket, &QTcpSocket::readyRead, this, nullptr);
                    if (!respond) {
                        return;
                    }
                    const auto reply = QByteArray("HTTP/1.1 ") + QByteArray::number(status) +
                                       " Test\r\nContent-Type: application/json\r\nConnection: "
                                       "close\r\nContent-Length: " +
                                       QByteArray::number(body.size()) + "\r\n\r\n" + body;
                    socket->write(reply);
                    socket->disconnectFromHost();
                });
            }
        });
        setResponse(true);
    }
    ~ProbeServer() override {
        for (auto* socket : server.findChildren<QTcpSocket*>()) {
            disconnect(socket, nullptr, this, nullptr);
            socket->abort();
        }
    }
    QString endpoint() const {
        return QStringLiteral("http://127.0.0.1:%1/v1/chat/completions").arg(server.serverPort());
    }
    void setResponse(bool usage) {
        QJsonObject response{
            {QStringLiteral("model"), QStringLiteral("fixture-model")},
            {QStringLiteral("choices"),
             QJsonArray{QJsonObject{
                 {QStringLiteral("message"),
                  QJsonObject{{QStringLiteral("content"), QStringLiteral("{\"ok\":true}")}}},
                 {QStringLiteral("finish_reason"), QStringLiteral("stop")}}}}};
        if (usage) {
            response.insert(QStringLiteral("usage"),
                            QJsonObject{{QStringLiteral("prompt_tokens"), 10},
                                        {QStringLiteral("completion_tokens"), 4},
                                        {QStringLiteral("total_tokens"), 14}});
        }
        body = QJsonDocument(response).toJson(QJsonDocument::Compact);
    }
    QTcpServer server;
    QHash<QTcpSocket*, QByteArray> buffers;
    QList<QByteArray> requests;
    QByteArray body;
    int status = 200;
    bool respond = true;
};

template <typename Widget> Widget* child(QWidget& page, const char* name) {
    return page.findChild<Widget*>(QString::fromLatin1(name));
}
void configure(loreforge::app::LlmWorkbench& page, const QString& endpoint) {
    child<QLineEdit>(page, "llmEndpoint")->setText(endpoint);
    child<QLineEdit>(page, "llmModelId")->setText(QStringLiteral("fixture-model"));
    child<QCheckBox>(page, "llmAllowHttp")->setChecked(true);
    child<QComboBox>(page, "llmResponseFormat")->setCurrentText(QStringLiteral("json_object"));
}
bool createProfile(const QString& path, const loreforge::core::ProjectId& id) {
    auto created = loreforge::storage::ProjectDatabase::create(path);
    if (!std::holds_alternative<std::unique_ptr<loreforge::storage::ProjectDatabase>>(created))
        return false;
    auto& database = std::get<std::unique_ptr<loreforge::storage::ProjectDatabase>>(created);
    loreforge::storage::ProjectRepository projects(*database);
    return !projects.create({id, QStringLiteral("Test profile"), QDateTime::currentDateTimeUtc()})
                .has_value();
}
QJsonObject profile(const QString& path, const loreforge::core::ProjectId& id) {
    auto opened = loreforge::storage::ProjectDatabase::open(path);
    if (!std::holds_alternative<std::unique_ptr<loreforge::storage::ProjectDatabase>>(opened))
        return {};
    auto& database = std::get<std::unique_ptr<loreforge::storage::ProjectDatabase>>(opened);
    loreforge::storage::ProjectRepository projects(*database);
    auto result = projects.apiSettings(id);
    return std::holds_alternative<QJsonObject>(result) ? std::get<QJsonObject>(result)
                                                       : QJsonObject{};
}
} // namespace

class LlmWorkbenchTest final : public QObject {
    Q_OBJECT
  private slots:
    void addsSecondPageWithoutDiscardingReadingState();
    void savesProjectApiAndTemporaryKey();
    void testsPrivateApiWithoutSendingSource();
    void rejectsUnsafeConfigurationAndRedactsFailures();
    void supportsCancellationAndMissingUsage();
    void resetsTokenCountsOnlyAfterConfirmation();
    void acknowledgesFailureWithoutChangingTaskHistory();
    void keepsNewerTasksSafeFromLateCallbacks();
    void reportsConnectionTaskStates();
    void placesTaskIndicatorBeforeWorkspaceStatus();
    void displaysInvalidResponseDiagnosticsAndCountsKnownUsage();
    void appliesAndPersistsThinkingMode();
    void preservesEditsWhenProjectSaveFails();
};

void LlmWorkbenchTest::acknowledgesFailureWithoutChangingTaskHistory() {
    using namespace loreforge::app;
    TaskIndicator indicator;
    auto* lamp = indicator.findChild<QPushButton*>(QStringLiteral("taskStatusLamp"));
    auto* label = indicator.findChild<QLabel*>(QStringLiteral("taskStatusText"));
    QCOMPARE(indicator.state(), TaskState::Idle);
    const auto task = QUuid::createUuid();
    indicator.report(task, TaskState::Waiting, QStringLiteral("连接测试"));
    QCOMPARE(indicator.state(), TaskState::Waiting);
    QVERIFY(!lamp->isEnabled());
    indicator.report(task, TaskState::Failed, QStringLiteral("HTTP 404"));
    QVERIFY(lamp->isEnabled());
    QVERIFY(label->text().contains(QStringLiteral("404")));
    QVERIFY(lamp->styleSheet().contains(QStringLiteral("#d32f2f")));
    lamp->click();
    QCOMPARE(indicator.state(), TaskState::Idle);
    QCOMPARE(label->text(), QStringLiteral("就绪"));
    QVERIFY(lamp->styleSheet().contains(QStringLiteral("#808080")));
    indicator.report(task, TaskState::Failed, QStringLiteral("过时的失败回调"));
    QCOMPARE(indicator.state(), TaskState::Idle);
    const auto next = QUuid::createUuid();
    indicator.report(next, TaskState::Local, QStringLiteral("预览"));
    indicator.setAnimationEnabled(false);
    indicator.acknowledgeFailure();
    QCOMPARE(indicator.state(), TaskState::Local);
    indicator.report(next, TaskState::Cancelled, QStringLiteral("已取消"));
    QCOMPARE(indicator.state(), TaskState::Cancelled);
    QVERIFY(!lamp->isEnabled());
}

void LlmWorkbenchTest::keepsNewerTasksSafeFromLateCallbacks() {
    using namespace loreforge::app;
    TaskIndicator indicator;
    const auto older = QUuid::createUuid();
    const auto newer = QUuid::createUuid();
    indicator.report(older, TaskState::Waiting, QStringLiteral("分析"));
    indicator.report(newer, TaskState::Local, QStringLiteral("导入"));
    indicator.report(older, TaskState::Failed, QStringLiteral("旧任务失败"));
    QCOMPARE(indicator.state(), TaskState::Local);
    indicator.report(newer, TaskState::Attention, QStringLiteral("待人工确认"));
    QCOMPARE(indicator.state(), TaskState::Attention);
    indicator.acknowledgeFailure();
    QCOMPARE(indicator.state(), TaskState::Attention);
    const auto background = QUuid::createUuid();
    const auto foreground = QUuid::createUuid();
    indicator.report(background, TaskState::Waiting, QStringLiteral("等待响应"));
    indicator.report(foreground, TaskState::Local, QStringLiteral("准备预览"));
    indicator.report(foreground, TaskState::Success, QStringLiteral("预览完成"));
    QCOMPARE(indicator.state(), TaskState::Waiting);
    indicator.report(background, TaskState::Success, QStringLiteral("分析完成"));
    QCOMPARE(indicator.state(), TaskState::Success);
}

void LlmWorkbenchTest::reportsConnectionTaskStates() {
    using namespace loreforge::app;
    QTemporaryDir dir;
    ProbeServer server;
    server.status = 404;
    LlmWorkbench page(nullptr, dir.filePath(QStringLiteral("settings.ini")));
    TaskIndicator indicator;
    connect(&page, &LlmWorkbench::taskStatus, &indicator, &TaskIndicator::report);
    configure(page, server.endpoint());
    page.findChild<QPushButton*>(QStringLiteral("llmTestConnection"))->click();
    QCOMPARE(indicator.state(), TaskState::Waiting);
    QTRY_COMPARE(indicator.state(), TaskState::Failed);
    const auto log = page.findChild<QPlainTextEdit*>(QStringLiteral("llmConnectionLog"));
    const auto before = log->toPlainText();
    indicator.acknowledgeFailure();
    QCOMPARE(indicator.state(), TaskState::Idle);
    QCOMPARE(log->toPlainText(), before);
    QCOMPARE(server.requests.size(), 1);
    configure(page, QStringLiteral("not-a-url"));
    page.findChild<QPushButton*>(QStringLiteral("llmTestConnection"))->click();
    QCOMPARE(indicator.state(), TaskState::Failed);
    QCOMPARE(server.requests.size(), 1);
}

void LlmWorkbenchTest::placesTaskIndicatorBeforeWorkspaceStatus() {
    loreforge::app::MainWindow window;
    window.show();
    auto* indicator = child<loreforge::app::TaskIndicator>(window, "taskIndicator");
    auto* status = child<QLabel>(window, "workspaceStatusMessage");
    auto* tabs = child<QTabWidget>(window, "workspacePages");
    QTRY_VERIFY(indicator->isVisible());
    QCOMPARE(status->text(), QStringLiteral("Ready"));
    QVERIFY(indicator->mapTo(&window, QPoint()).x() < 20);
    QVERIFY(status->mapTo(&window, QPoint()).x() >=
            indicator->mapTo(&window, QPoint()).x() + indicator->width());
    tabs->setCurrentIndex(1);
    QVERIFY(indicator->isVisible());
    tabs->setCurrentIndex(2);
    QVERIFY(indicator->isVisible());
    const auto task = QUuid::createUuid();
    indicator->report(task, loreforge::app::TaskState::Local, QStringLiteral("任务"));
    indicator->report(task, loreforge::app::TaskState::Failed, QStringLiteral("失败"));
    indicator->acknowledgeFailure();
    QCOMPARE(status->text(), QStringLiteral("Ready"));
}

void LlmWorkbenchTest::displaysInvalidResponseDiagnosticsAndCountsKnownUsage() {
    QTemporaryDir directory;
    ProbeServer server;
    server.body =
        R"({"choices":[{"message":{"content":"","reasoning_content":"private-thinking"},"finish_reason":"length"}],"usage":{"prompt_tokens":10,"completion_tokens":4096,"total_tokens":4106}})";
    loreforge::app::LlmWorkbench page(nullptr, directory.filePath(QStringLiteral("settings.ini")));
    configure(page, server.endpoint());
    child<QComboBox>(page, "llmResponseFormat")->setCurrentText(QStringLiteral("json_object"));
    child<QPushButton>(page, "llmTestConnection")->click();
    QTRY_VERIFY(child<QPushButton>(page, "llmTestConnection")->isEnabled());
    QVERIFY(
        child<QLabel>(page, "llmConnectionStatus")->text().contains(QStringLiteral("正文为空")));
    QVERIFY(child<QLabel>(page, "llmConnectionStatus")->text().contains(QStringLiteral("length")));
    QVERIFY(child<QLabel>(page, "llmTestTotals")->text().contains(QStringLiteral("总计 4106")));
    QVERIFY(child<QLabel>(page, "llmTestTotals")->text().contains(QStringLiteral("0 次用量未知")));
    QVERIFY(!child<QPlainTextEdit>(page, "llmConnectionLog")
                 ->toPlainText()
                 .contains(QStringLiteral("private-thinking")));
    QCOMPARE(server.requests.size(), 1);
}

void LlmWorkbenchTest::appliesAndPersistsThinkingMode() {
    QTemporaryDir directory;
    const auto path = directory.filePath(QStringLiteral("thinking.loreforge"));
    const auto id = loreforge::core::ProjectId::fromStableKey(QStringLiteral("thinking-profile"));
    QVERIFY(createProfile(path, id));
    ProbeServer server;
    loreforge::app::LlmWorkbench page(nullptr, path);
    QVERIFY(page.setProject(path, id));
    configure(page, server.endpoint());
    auto* mode = child<QComboBox>(page, "llmThinkingMode");
    auto* test = child<QPushButton>(page, "llmTestConnection");
    QCOMPARE(mode->currentData().toInt(), -1);
    for (int value : {-1, 0, 1}) {
        mode->setCurrentIndex(mode->findData(value));
        const auto count = server.requests.size();
        test->click();
        QVERIFY(!mode->isEnabled());
        QTRY_VERIFY(test->isEnabled());
        QCOMPARE(server.requests.size(), count + 1);
        const auto raw = server.requests.last();
        const auto payload = QJsonDocument::fromJson(raw.mid(raw.indexOf("\r\n\r\n") + 4)).object();
        if (value < 0) {
            QVERIFY(!payload.contains(QStringLiteral("enable_thinking")));
        } else {
            QVERIFY(payload.value(QStringLiteral("enable_thinking")).isBool());
            QCOMPARE(payload.value(QStringLiteral("enable_thinking")).toBool(), value == 1);
        }
    }
    mode->setCurrentIndex(mode->findData(0));
    child<QPushButton>(page, "llmSaveConfiguration")->click();
    loreforge::app::LlmWorkbench restored(nullptr, path);
    QVERIFY(restored.setProject(path, id));
    QCOMPARE(child<QComboBox>(restored, "llmThinkingMode")->currentData().toInt(), 0);
    configure(restored, server.endpoint());
    child<QComboBox>(restored, "llmResponseFormat")->setCurrentText(QStringLiteral("json_object"));
    const auto configuration = restored.analysisConnection();
    QVERIFY(std::holds_alternative<loreforge::app::AnalysisConnection>(configuration));
    const auto thinking =
        std::get<loreforge::app::AnalysisConnection>(configuration).options.enableThinking;
    QVERIFY(thinking.has_value());
    QVERIFY(!*thinking);
}

void LlmWorkbenchTest::preservesEditsWhenProjectSaveFails() {
    QTemporaryDir directory;
    const auto path = directory.filePath(QStringLiteral("failed-save.loreforge"));
    const auto other = directory.filePath(QStringLiteral("other.loreforge"));
    const auto id = loreforge::core::ProjectId::fromStableKey(QStringLiteral("save-test"));
    QVERIFY(createProfile(path, id));
    QVERIFY(createProfile(other, id));
    const auto execute = [&](const QString& sql) {
        const auto name = QUuid::createUuid().toString();
        bool ok = false;
        {
            auto connection = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
            connection.setDatabaseName(path);
            if (connection.open()) {
                QSqlQuery query(connection);
                ok = query.exec(sql);
            }
            connection.close();
        }
        QSqlDatabase::removeDatabase(name);
        return ok;
    };
    loreforge::app::LlmWorkbench page;
    QVERIFY(page.setProject(path, id));
    child<QLineEdit>(page, "llmApiKey")->setText(QStringLiteral("before-fixture-key"));
    QVERIFY(page.flushConfiguration());
    QVERIFY(
        execute(QStringLiteral("CREATE TRIGGER fail_api_save BEFORE INSERT ON project_api_settings "
                               "BEGIN SELECT RAISE(FAIL, 'fixture'); END")));
    child<QLineEdit>(page, "llmApiKey")->setText(QStringLiteral("after-fixture-key"));
    QVERIFY(!page.flushConfiguration());
    QVERIFY(!page.setProject(other, id));
    QCOMPARE(child<QLineEdit>(page, "llmApiKey")->text(), QStringLiteral("after-fixture-key"));
    QCOMPARE(profile(path, id).value(QStringLiteral("api_key")).toString(),
             QStringLiteral("before-fixture-key"));
    QVERIFY(execute(QStringLiteral("DROP TRIGGER fail_api_save")));
    QVERIFY(page.flushConfiguration());
    QVERIFY(page.setProject(other, id));
    QVERIFY(child<QLineEdit>(page, "llmApiKey")->text().isEmpty());
    QCOMPARE(profile(path, id).value(QStringLiteral("api_key")).toString(),
             QStringLiteral("after-fixture-key"));
}

void LlmWorkbenchTest::addsSecondPageWithoutDiscardingReadingState() {
    loreforge::app::MainWindow window;
    auto* tabs = child<QTabWidget>(window, "workspacePages");
    QVERIFY(tabs);
    QCOMPARE(tabs->count(), 3);
    QCOMPARE(tabs->tabText(0), QStringLiteral("阅读工作台"));
    QCOMPARE(tabs->tabText(1), QStringLiteral("LLM 工作台"));
    QCOMPARE(tabs->tabText(2), QStringLiteral("小说分析"));
    auto* reader = child<QWidget>(window, "chapterReader");
    auto* context = child<QDockWidget>(window, "contextInspectorDock");
    auto* repairs = child<QDockWidget>(window, "repairQueueDock");
    context->hide();
    repairs->show();
    tabs->setCurrentIndex(1);
    QVERIFY(context->isHidden());
    QVERIFY(repairs->isHidden());
    tabs->setCurrentIndex(2);
    QVERIFY(context->isHidden());
    QVERIFY(repairs->isHidden());
    QVERIFY(!child<QPushButton>(window, "llmAnalyzeChapter")->isEnabled());
    auto* roles = child<QTableWidget>(window, "llmRoleStatus");
    QCOMPARE(roles->rowCount(), 3);
    QCOMPARE(roles->item(2, 0)->text(), QStringLiteral("审查员（LLM 角色）"));
    QVERIFY(!child<QWidget>(window, "llmWorkbench")
                 ->findChild<QPushButton*>(QStringLiteral("llmPrepareChapterPreview")));
    QVERIFY(child<QWidget>(window, "novelAnalysisWorkbench")
                ->findChild<QPushButton*>(QStringLiteral("llmPrepareChapterPreview")));
    tabs->setCurrentIndex(0);
    QCOMPARE(child<QWidget>(window, "chapterReader"), reader);
    QVERIFY(context->isHidden());
    QVERIFY(!repairs->isHidden());
}

void LlmWorkbenchTest::savesProjectApiAndTemporaryKey() {
    QTemporaryDir directory;
    const auto path = directory.filePath(QStringLiteral("profile.loreforge"));
    const auto otherPath = directory.filePath(QStringLiteral("other.loreforge"));
    const auto id = loreforge::core::ProjectId::fromStableKey(QStringLiteral("profile-a"));
    const auto otherId = loreforge::core::ProjectId::fromStableKey(QStringLiteral("profile-b"));
    QVERIFY(createProfile(path, id));
    QVERIFY(createProfile(otherPath, otherId));
    {
        loreforge::app::LlmWorkbench page(nullptr, path);
        QVERIFY(page.setProject(path, id));
        configure(page, QStringLiteral("http://192.168.1.20:8000/v1/chat/completions"));
        child<QLineEdit>(page, "llmApiKey")->setText(QStringLiteral("temporary-fixture-key"));
        QVERIFY(!page.findChild<QComboBox*>(QStringLiteral("llmModelSize")));
        page.setAnalysisBudgets(65536, 16384);
        child<QPushButton>(page, "llmSaveConfiguration")->click();
        QVERIFY(child<QLabel>(page, "llmConnectionStatus")
                    ->text()
                    .contains(QStringLiteral("已自动保存")));
    }
    QCOMPARE(profile(path, id).value(QStringLiteral("api_key")).toString(),
             QStringLiteral("temporary-fixture-key"));
    loreforge::app::LlmWorkbench restored(nullptr, path);
    QVERIFY(restored.setProject(path, id));
    QCOMPARE(child<QLineEdit>(restored, "llmApiKey")->text(),
             QStringLiteral("temporary-fixture-key"));
    QVERIFY(child<QCheckBox>(restored, "llmAllowHttp")->isChecked());
    QVERIFY(restored.setProject(otherPath, otherId));
    QVERIFY(child<QLineEdit>(restored, "llmApiKey")->text().isEmpty());
    child<QLineEdit>(restored, "llmApiKey")->setText(QStringLiteral("other-fixture-key"));
    QTRY_COMPARE(profile(otherPath, otherId).value(QStringLiteral("api_key")).toString(),
                 QStringLiteral("other-fixture-key"));
    QVERIFY(restored.setProject(path, id));
    QCOMPARE(child<QLineEdit>(restored, "llmApiKey")->text(),
             QStringLiteral("temporary-fixture-key"));
    child<QPushButton>(restored, "llmClearSavedKey")->click();
    QVERIFY(profile(path, id).value(QStringLiteral("api_key")).toString().isEmpty());
    QCOMPARE(profile(otherPath, otherId).value(QStringLiteral("api_key")).toString(),
             QStringLiteral("other-fixture-key"));
    QVERIFY(!child<QPushButton>(restored, "llmCancelTest")->isEnabled());
    loreforge::app::LlmWorkbench session;
    configure(session, QStringLiteral("https://example.invalid/chat/completions"));
    child<QLineEdit>(session, "llmApiKey")->setText(QStringLiteral("session-temporary-key"));
    // Existing project settings always win; session data must not overwrite them.
    QVERIFY(session.setProject(otherPath, otherId));
    QCOMPARE(child<QLineEdit>(session, "llmApiKey")->text(), QStringLiteral("other-fixture-key"));
    QVERIFY(session.setProject({}));
    child<QLineEdit>(session, "llmApiKey")->setText(QStringLiteral("new-session-key"));
    const auto freshPath = directory.filePath(QStringLiteral("fresh.loreforge"));
    QVERIFY(createProfile(freshPath, id));
    QVERIFY(session.setProject(freshPath, id));
    QCOMPARE(profile(freshPath, id).value(QStringLiteral("api_key")).toString(),
             QStringLiteral("new-session-key"));
}

void LlmWorkbenchTest::testsPrivateApiWithoutSendingSource() {
    QTemporaryDir directory;
    ProbeServer server;
    loreforge::app::LlmWorkbench page(nullptr, directory.filePath(QStringLiteral("profile.ini")));
    configure(page, server.endpoint());
    child<QComboBox>(page, "llmTokenField")->setCurrentText(QStringLiteral("max_tokens"));
    auto* test = child<QPushButton>(page, "llmTestConnection");
    test->click();
    QVERIFY(!test->isEnabled());
    QTRY_VERIFY_WITH_TIMEOUT(test->isEnabled(), 3000);
    QCOMPARE(server.requests.size(), 1);
    QVERIFY(!server.requests.first().toLower().contains("authorization:"));
    const auto& raw = server.requests.first();
    const auto body = QJsonDocument::fromJson(raw.sliced(raw.indexOf("\r\n\r\n") + 4)).object();
    QCOMPARE(body.value(QStringLiteral("model")).toString(), QStringLiteral("fixture-model"));
    QVERIFY(body.contains(QStringLiteral("max_tokens")));
    QVERIFY(!body.contains(QStringLiteral("max_completion_tokens")));
    QCOMPARE(body.value(QStringLiteral("messages")).toArray().size(), 2);
    QVERIFY(child<QLabel>(page, "llmConnectionStatus")
                ->text()
                .contains(QStringLiteral("短样本测试通过")));
    QVERIFY(child<QLabel>(page, "llmTestMetrics")->text().contains(QStringLiteral("输入 10")));
    QVERIFY(child<QLabel>(page, "llmTestTotals")->text().contains(QStringLiteral("总计 14")));
    if (const auto preview = qEnvironmentVariable("LOREFORGE_LLM_PREVIEW"); !preview.isEmpty()) {
        const auto fontId =
            QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/msyh.ttc"));
        const auto families = QFontDatabase::applicationFontFamilies(fontId);
        if (!families.isEmpty()) {
            page.setFont(QFont(families.first(), 10));
        }
        page.resize(1100, 900);
        page.show();
        QTest::qWait(30);
        QVERIFY(page.grab().save(preview));
    }
    child<QLineEdit>(page, "llmModelId")->setText(QStringLiteral("another-model"));
    QVERIFY(child<QLabel>(page, "llmConnectionStatus")->text().contains(QStringLiteral("本会话")));
}

void LlmWorkbenchTest::rejectsUnsafeConfigurationAndRedactsFailures() {
    QTemporaryDir directory;
    ProbeServer server;
    loreforge::app::LlmWorkbench page(nullptr, directory.filePath(QStringLiteral("profile.ini")));
    configure(page, QStringLiteral("http://example.com/v1/chat/completions"));
    child<QPushButton>(page, "llmTestConnection")->click();
    QVERIFY(child<QLabel>(page, "llmConnectionStatus")->text().contains(QStringLiteral("公网")));
    QCOMPARE(server.requests.size(), 0);
    configure(page, server.endpoint() + QStringLiteral("?token=secret"));
    child<QPushButton>(page, "llmTestConnection")->click();
    QCOMPARE(server.requests.size(), 0);
    configure(page, server.endpoint());
    child<QLineEdit>(page, "llmApiKey")->setText(QStringLiteral("secret-not-for-log"));
    server.status = 401;
    server.body = QByteArray("{\"error\":{\"message\":\"secret-not-for-log\"}}");
    child<QPushButton>(page, "llmTestConnection")->click();
    QTRY_VERIFY_WITH_TIMEOUT(child<QPushButton>(page, "llmTestConnection")->isEnabled(), 3000);
    QCOMPARE(server.requests.size(), 1);
    QVERIFY(child<QLabel>(page, "llmConnectionStatus")->text().contains(QStringLiteral("401")));
    QVERIFY(!child<QPlainTextEdit>(page, "llmConnectionLog")
                 ->toPlainText()
                 .contains(QStringLiteral("secret-not-for-log")));
}

void LlmWorkbenchTest::supportsCancellationAndMissingUsage() {
    QTemporaryDir directory;
    ProbeServer server;
    loreforge::app::LlmWorkbench page(nullptr, directory.filePath(QStringLiteral("profile.ini")));
    configure(page, server.endpoint());
    server.setResponse(false);
    child<QPushButton>(page, "llmTestConnection")->click();
    QTRY_VERIFY_WITH_TIMEOUT(child<QPushButton>(page, "llmTestConnection")->isEnabled(), 3000);
    QVERIFY(child<QLabel>(page, "llmTestMetrics")->text().contains(QStringLiteral("服务未提供")));
    QVERIFY(child<QLabel>(page, "llmTestTotals")->text().contains(QStringLiteral("1 次用量未知")));
    server.respond = false;
    child<QPushButton>(page, "llmTestConnection")->click();
    QTRY_COMPARE_WITH_TIMEOUT(server.requests.size(), 2, 3000);
    child<QPushButton>(page, "llmCancelTest")->click();
    QTRY_VERIFY(child<QPushButton>(page, "llmTestConnection")->isEnabled());
    QVERIFY(child<QLabel>(page, "llmConnectionStatus")->text().contains(QStringLiteral("已取消")));
    QCOMPARE(server.requests.size(), 2);
}

void LlmWorkbenchTest::resetsTokenCountsOnlyAfterConfirmation() {
    QTemporaryDir directory;
    ProbeServer server;
    const auto settingsPath = directory.filePath(QStringLiteral("profile.ini"));
    loreforge::app::LlmWorkbench page(nullptr, settingsPath);
    configure(page, server.endpoint());
    auto* test = child<QPushButton>(page, "llmTestConnection");
    auto* reset = child<QPushButton>(page, "llmResetTokenCounts");
    auto* totals = child<QLabel>(page, "llmTestTotals");
    test->click();
    QVERIFY(!reset->isEnabled());
    QTRY_VERIFY_WITH_TIMEOUT(test->isEnabled(), 3000);
    QVERIFY(reset->isEnabled());
    QVERIFY(totals->text().contains(QStringLiteral("总计 14")));
    server.setResponse(false);
    test->click();
    QTRY_VERIFY_WITH_TIMEOUT(test->isEnabled(), 3000);
    QVERIFY(totals->text().contains(QStringLiteral("1 次用量未知")));
    const auto previousTotals = totals->text();
    const auto previousMetrics = child<QLabel>(page, "llmTestMetrics")->text();
    const auto previousStatus = child<QLabel>(page, "llmConnectionStatus")->text();
    QTimer::singleShot(0, &page, [&page] {
        auto* dialog = child<QMessageBox>(page, "llmResetTokenConfirmation");
        QVERIFY(dialog);
        auto* cancel = child<QPushButton>(*dialog, "llmCancelTokenReset");
        QCOMPARE(dialog->defaultButton(), cancel);
        QCOMPARE(cancel->text(), QStringLiteral("取消"));
        cancel->click();
    });
    reset->click();
    QCOMPARE(totals->text(), previousTotals);
    QTimer::singleShot(0, &page, [&page] {
        auto* dialog = child<QMessageBox>(page, "llmResetTokenConfirmation");
        QVERIFY(dialog);
        dialog->reject();
    });
    reset->click();
    QCOMPARE(totals->text(), previousTotals);
    QTimer::singleShot(0, &page, [&page] {
        auto* dialog = child<QMessageBox>(page, "llmResetTokenConfirmation");
        QVERIFY(dialog);
        auto* confirm = child<QPushButton>(*dialog, "llmConfirmTokenReset");
        QCOMPARE(confirm->text(), QStringLiteral("确认"));
        confirm->click();
    });
    reset->click();
    QVERIFY(totals->text().contains(QStringLiteral("输入 0 · 输出 0 · 总计 0")));
    QVERIFY(totals->text().contains(QStringLiteral("0 次用量未知")));
    QCOMPARE(child<QLabel>(page, "llmTestMetrics")->text(), previousMetrics);
    QCOMPARE(child<QLabel>(page, "llmConnectionStatus")->text(), previousStatus);
    QCOMPARE(child<QLineEdit>(page, "llmEndpoint")->text(), server.endpoint());
    QCOMPARE(server.requests.size(), 2);
    server.setResponse(true);
    test->click();
    QTRY_VERIFY_WITH_TIMEOUT(test->isEnabled(), 3000);
    QVERIFY(totals->text().contains(QStringLiteral("总计 14")));
    loreforge::app::LlmWorkbench restored(nullptr, settingsPath);
    QVERIFY(child<QLabel>(restored, "llmTestTotals")->text().contains(QStringLiteral("尚无响应")));
}

QTEST_MAIN(LlmWorkbenchTest)
#include "llm_workbench_test.moc"
