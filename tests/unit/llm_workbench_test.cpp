#include "llm_workbench.h"
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
} // namespace

class LlmWorkbenchTest final : public QObject {
    Q_OBJECT
  private slots:
    void addsSecondPageWithoutDiscardingReadingState();
    void savesOnlyNonSecretConfiguration();
    void testsPrivateApiWithoutSendingSource();
    void rejectsUnsafeConfigurationAndRedactsFailures();
    void supportsCancellationAndMissingUsage();
    void resetsTokenCountsOnlyAfterConfirmation();
};

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

void LlmWorkbenchTest::savesOnlyNonSecretConfiguration() {
    QTemporaryDir directory;
    const auto path = directory.filePath(QStringLiteral("settings.ini"));
    {
        loreforge::app::LlmWorkbench page(nullptr, path);
        configure(page, QStringLiteral("http://192.168.1.20:8000/v1/chat/completions"));
        child<QLineEdit>(page, "llmApiKey")->setText(QStringLiteral("secret-not-for-disk"));
        child<QComboBox>(page, "llmModelSize")->setCurrentText(QStringLiteral("30B"));
        child<QPushButton>(page, "llmSaveConfiguration")->click();
        QVERIFY(
            child<QLabel>(page, "llmConnectionStatus")->text().contains(QStringLiteral("已保存")));
    }
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const auto bytes = file.readAll();
    QVERIFY(!bytes.contains("secret-not-for-disk"));
    QVERIFY(!bytes.contains("apiKey"));
    loreforge::app::LlmWorkbench restored(nullptr, path);
    QCOMPARE(child<QComboBox>(restored, "llmModelSize")->currentText(), QStringLiteral("30B"));
    QVERIFY(child<QLineEdit>(restored, "llmApiKey")->text().isEmpty());
    QVERIFY(!child<QCheckBox>(restored, "llmAllowHttp")->isChecked());
    QVERIFY(!child<QPushButton>(restored, "llmCancelTest")->isEnabled());
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
    QVERIFY(
        child<QLabel>(page, "llmConnectionStatus")->text().contains(QStringLiteral("重新测试")));
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
