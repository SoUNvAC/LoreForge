#include "chapter_analysis_controller.h"
#include "chapter_evidence.h"
#include "character_highlight_preview.h"
#include "dialogue_segmentation.h"
#include "main_window.h"
#include "markdown_source_fixture.h"
#include "novel_analysis_workbench.h"
#include "stream_monitor_widget.h"

#include "loreforge/parser/markdown_source_parser.h"
#include "loreforge/storage/book_repository.h"
#include "loreforge/storage/inference_repository.h"
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
#include <QSpinBox>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTabWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeWidget>
#include <QtTest>
#include <algorithm>

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
    bool initialize(const QByteArray& chapter = test::markdownFixtureChapter(),
                    bool dialogue = false) {
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
        auto built = app::ChapterPreviewBuilder::build(projectId, book, 0, 32768, 4096, dialogue);
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
    if (user.contains(QStringLiteral("source_units"))) {
        QJsonArray segments;
        for (const auto& value : user.value(QStringLiteral("source_units")).toArray()) {
            const auto unit = value.toObject();
            const auto quoted =
                unit.value(QStringLiteral("text")).toString().startsWith(QChar(0x201c));
            if (!quoted)
                continue;
            segments.append(
                QJsonObject{{QStringLiteral("first_unit"), unit.value(QStringLiteral("unit_id"))},
                            {QStringLiteral("last_unit"), unit.value(QStringLiteral("unit_id"))},
                            {QStringLiteral("speaker"), QStringLiteral("她")}});
        }
        return {{QStringLiteral("chapter_id"), user.value(QStringLiteral("chapter_id"))},
                {QStringLiteral("characters"),
                 QJsonArray{
                     QJsonObject{{QStringLiteral("name"), QStringLiteral("她")},
                                 {QStringLiteral("aliases"), QJsonArray{QStringLiteral("小晴")}}}}},
                {QStringLiteral("segments"), segments},
                {QStringLiteral("proofreading"), QJsonArray{}}};
    }
    const auto range = user.value(QStringLiteral("source_blocks")).toArray().at(1).toObject();
    const QJsonArray evidence{
        QJsonObject{{QStringLiteral("block_id"), range.value(QStringLiteral("block_id"))}}};
    const QJsonObject support{{QStringLiteral("inferred"), false},
                              {QStringLiteral("confidence"), 0.9},
                              {QStringLiteral("evidence"), evidence}};
    auto summary = support;
    summary.insert(QStringLiteral("text"), QStringLiteral("她打开门，窗外有灯。"));
    auto character = support;
    character.insert(QStringLiteral("name"), QStringLiteral("她"));
    character.insert(QStringLiteral("aliases"), QJsonArray{});
    const auto raw =
        user.value(QStringLiteral("source")).toObject().value(QStringLiteral("utf8")).toString();
    QJsonArray segments;
    const auto add = [&](qsizetype start, qsizetype end) {
        if (end > start)
            segments.append(QJsonObject{{QStringLiteral("text"), raw.mid(start, end - start)},
                                        {QStringLiteral("kind"), QStringLiteral("narration")},
                                        {QStringLiteral("speaker"), QString{}}});
    };
    qsizetype previous = 0;
    for (const auto& quote : app::quotationRanges(raw)) {
        add(previous, quote.first);
        add(quote.first, quote.second);
        previous = quote.second;
    }
    add(previous, raw.size());
    return {{QStringLiteral("chapter_id"), user.value(QStringLiteral("chapter_id"))},
            {QStringLiteral("characters"), QJsonArray{character}},
            {QStringLiteral("locations"), QJsonArray{}},
            {QStringLiteral("events"), QJsonArray{}},
            {QStringLiteral("summary"), summary},
            {QStringLiteral("segments"), segments},
            {QStringLiteral("important_facts"), QJsonArray{summary}},
            {QStringLiteral("open_threads"), QJsonArray{}}};
}
QJsonObject reviewFor(const QJsonObject& user, const QString& verdict = QStringLiteral("pass")) {
    QJsonArray issues;
    if (verdict != QStringLiteral("pass")) {
        const auto range = user.value(QStringLiteral("source_blocks")).toArray().at(1).toObject();
        issues.append(
            QJsonObject{{QStringLiteral("reason"), QStringLiteral("这项结论需再核对。")},
                        {QStringLiteral("block_id"), range.value(QStringLiteral("block_id"))}});
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
                    if (withReasoning) {
                        auto choices = response.value(QStringLiteral("choices")).toArray();
                        auto choice = choices[0].toObject();
                        auto message = choice.value(QStringLiteral("message")).toObject();
                        message.insert(QStringLiteral("reasoning_content"),
                                       QStringLiteral("private-thinking-must-not-persist"));
                        choice.insert(QStringLiteral("message"), message);
                        choices[0] = choice;
                        response.insert(QStringLiteral("choices"), choices);
                    }
                    auto body =
                        !responseOverride.isNull() ? responseOverride
                        : status == 200
                            ? QJsonDocument(response).toJson(QJsonDocument::Compact)
                            : QByteArray(
                                  "{\"error\":{\"message\":\"credential-must-not-persist\"}}");
                    const bool sse = streaming && status == 200 && responseOverride.isNull() &&
                                     request.value(QStringLiteral("stream")).toBool();
                    if (sse) {
                        const auto delta = [](const QJsonObject& value,
                                              QJsonValue finish = QJsonValue()) {
                            return QByteArray("data: ") +
                                   QJsonDocument(QJsonObject{{QStringLiteral("choices"),
                                                              QJsonArray{QJsonObject{
                                                                  {QStringLiteral("index"), 0},
                                                                  {QStringLiteral("delta"), value},
                                                                  {QStringLiteral("finish_reason"),
                                                                   finish}}}}})
                                       .toJson(QJsonDocument::Compact) +
                                   "\n\n";
                        };
                        const auto content =
                            QString::fromUtf8(QJsonDocument(output).toJson(QJsonDocument::Compact));
                        body =
                            delta({{QStringLiteral("content"), content.first(content.size() / 2)}});
                        if (withReasoning)
                            body += delta({{QStringLiteral("reasoning_content"),
                                            QStringLiteral("private-thinking-must-not-persist")}});
                        body += delta(
                            {{QStringLiteral("content"), content.sliced(content.size() / 2)}});
                        body += delta({}, finishReason);
                        if (withUsage)
                            body += "data: "
                                    "{\"choices\":[],\"usage\":{\"prompt_tokens\":10,\"completion_"
                                    "tokens\":4,\"total_tokens\":14}}\n\n";
                        body += "data: [DONE]\n\n";
                    }
                    socket->write(QByteArray("HTTP/1.1 ") + QByteArray::number(status) +
                                  " Test\r\nContent-Type: " +
                                  (sse ? "text/event-stream" : "application/json") +
                                  "\r\nConnection: "
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
    bool streaming = true;
    bool withUsage = true;
    bool withReasoning = false;
    QByteArray responseOverride;
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
    void initTestCase() {
        qputenv("LOREFORGE_HIGHLIGHT_SETTINGS",
                highlightSettings_.filePath(QStringLiteral("highlight.ini")).toUtf8());
        qputenv("LOREFORGE_NAVIGATION_SETTINGS",
                highlightSettings_.filePath(QStringLiteral("navigation.ini")).toUtf8());
    }
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
    void persistsSafeDiagnosticsAndKnownUsageForInvalidResponses();
    void doesNotStoreReasoningInSuccessfulArtifacts();
    void continuesOnlyUnfinishedRoles_data();
    void continuesOnlyUnfinishedRoles();
    void refusesIncompatibleContinuation();
    void confirmsContinuationAndShowsReviewReasons();
    void validatesLegacyAndCorruptPrerequisites();
    void carriesAliasRulesAndPreservesSemanticVerdicts_data();
    void carriesAliasRulesAndPreservesSemanticVerdicts();
    void monitorShowsOnlyTheCurrentRequest();
    void acceptsJsonFallbackWithoutPretendingItIsSse();
    void resolvesAllClaimsAndAuditsRawIds();
    void refusesInvalidBlockReferences_data();
    void refusesInvalidBlockReferences();
    void refusesInvalidMappingAndRendersLegacyHistory();
    void validatesCompleteNovelSegmentation();
    void refusesCoarseNarrationWithoutForcingQuotesToDialogue();
    void independentlySegmentsAndContinuesWithoutRepeatingExtraction();
    void confirmsIndependentDialogueFromMainWindow();
    void preservesPartialExtractionWithoutPretendingItFullyPassed();
    void automaticallyRunsNextChapter_data();
    void automaticallyRunsNextChapter();
    void showsChapterIndicatorsAndPersistedProofreading();
    void switchesChaptersWithoutReloadingUnrelatedArtifacts();
    void opensProjectInBackgroundAndDiscardsStaleResults();
    void loadsLargeHistoryInBackgroundWhileReading();

  private:
    QTemporaryDir highlightSettings_;
};

void ChapterAnalysisPipelineTest::automaticallyRunsNextChapter_data() {
    QTest::addColumn<int>("mode");
    const QStringList cases{QStringLiteral("all-chapters"),
                            QStringLiteral("uncheck-during-current"),
                            QStringLiteral("enable-during-current"),
                            QStringLiteral("http-failure"),
                            QStringLiteral("needs-human"),
                            QStringLiteral("partial-extraction"),
                            QStringLiteral("uncheck-before-queued-next"),
                            QStringLiteral("manual-chapter-change"),
                            QStringLiteral("cancel-current"),
                            QStringLiteral("reject-auto-consent"),
                            QStringLiteral("api-configuration-change"),
                            QStringLiteral("next-source-changed"),
                            QStringLiteral("next-chapter-fails"),
                            QStringLiteral("keeps-comprehensive-mode"),
                            QStringLiteral("starts-at-last-chapter"),
                            QStringLiteral("project-switch"),
                            QStringLiteral("close-before-queued-next"),
                            QStringLiteral("close-current-window"),
                            QStringLiteral("budget-change"),
                            QStringLiteral("checker-http-failure"),
                            QStringLiteral("reviewer-http-failure"),
                            QStringLiteral("invalid-review-output"),
                            QStringLiteral("redo-verdict"),
                            QStringLiteral("current-source-changed"),
                            QStringLiteral("credential-reflection")};
    for (int mode = 0; mode < cases.size(); ++mode)
        QTest::newRow(qPrintable(cases[mode])) << mode;
}

void ChapterAnalysisPipelineTest::automaticallyRunsNextChapter() {
    QFETCH(int, mode);
    Fixture fixture;
    QVERIFY(fixture.initialize());
    const auto otherPath = fixture.destination.filePath(QStringLiteral("other-auto.loreforge"));
    QVERIFY(QFile::copy(fixture.path, otherPath));
    Server server;
    app::MainWindow window;
    QVERIFY(window.openProjectFile(fixture.path));
    auto* llm = window.findChild<QWidget*>(QStringLiteral("llmWorkbench"));
    llm->findChild<QLineEdit*>(QStringLiteral("llmEndpoint"))
        ->setText(server.connection().options.endpoint.toString());
    llm->findChild<QLineEdit*>(QStringLiteral("llmModelId"))
        ->setText(QStringLiteral("fixture-model"));
    llm->findChild<QLineEdit*>(QStringLiteral("llmApiKey"))
        ->setText(QStringLiteral("auto-chapter-fixture-key"));
    llm->findChild<QCheckBox*>(QStringLiteral("llmAllowHttp"))->setChecked(true);
    llm->findChild<QComboBox*>(QStringLiteral("llmResponseFormat"))
        ->setCurrentText(QStringLiteral("json_object"));
    auto* flag = window.findChild<QCheckBox*>(QStringLiteral("analysisAutoNextChapter"));
    auto* controller = window.findChild<Controller*>();
    auto* selector = window.findChild<QComboBox*>(QStringLiteral("analysisChapterSelector"));
    QVERIFY(flag);
    QVERIFY(!flag->isChecked());
    if (mode == 14)
        selector->setCurrentIndex(2);
    const auto enable = [&](bool accept) {
        QTimer::singleShot(0, &window, [&window, accept] {
            auto* dialog =
                window.findChild<QMessageBox*>(QStringLiteral("analysisAutoNextConfirmation"));
            QVERIFY(dialog);
            if (accept)
                dialog->findChild<QPushButton*>(QStringLiteral("analysisAutoNextConfirm"))->click();
            else
                dialog->reject();
        });
        flag->setChecked(true);
    };
    if (mode != 2)
        enable(mode != 9);
    QCOMPARE(server.requests.size(), 0);
    bool enabledWhileBusy = false;
    bool sourceRewritten = false;
    server.makeOutput = [&](const QJsonObject& user, int index) -> QJsonObject {
        if (index == 0) {
            if (mode == 1)
                flag->setChecked(false);
            if (mode == 2) {
                enabledWhileBusy = controller->busy() && flag->isEnabled();
                enable(true);
            }
            if (mode == 8)
                window.findChild<QPushButton*>(QStringLiteral("analysisCancelTask"))->click();
            if (mode == 17)
                window.close();
            if (mode == 18)
                window.findChild<QSpinBox*>(QStringLiteral("llmChapterOutputBudget"))
                    ->setValue(4352);
            if (mode == 10)
                llm->findChild<QLineEdit*>(QStringLiteral("llmModelId"))
                    ->setText(QStringLiteral("changed-fixture-model"));
        }
        if (mode == 12 && index == 3)
            server.status = 500;
        if (mode == 23 && index == 0)
            sourceRewritten =
                test::writeMarkdownFixtureFile(fixture.source.path(), QStringLiteral("02/010.md"),
                                               QByteArray("# Changed\n\nNot the imported text.\n"));
        if (mode == 24)
            return QJsonObject{
                {QStringLiteral("secret"), QStringLiteral("auto-chapter-fixture-key")}};
        if (mode == 19 || mode == 20)
            server.status =
                user.contains(mode == 19 ? QStringLiteral("analysis") : QStringLiteral("checker"))
                    ? 500
                    : 200;
        if (user.contains(QStringLiteral("analysis"))) {
            if (mode == 21)
                return QJsonObject{};
            return reviewFor(user, mode == 4    ? QStringLiteral("human")
                                   : mode == 22 ? QStringLiteral("redo")
                                                : QStringLiteral("pass"));
        }
        auto output = analysisFor(user);
        if (mode == 5) {
            auto parts = output.value(QStringLiteral("segments")).toArray();
            parts.append(QJsonObject{{QStringLiteral("first_unit"), QStringLiteral("u9999")},
                                     {QStringLiteral("last_unit"), QStringLiteral("u9999")},
                                     {QStringLiteral("speaker"), QStringLiteral("她")}});
            output.insert(QStringLiteral("segments"), parts);
        }
        return output;
    };
    if (mode == 3)
        server.status = 500;
    QSignalSpy finished(controller, &Controller::finished);
    connect(controller, &Controller::finished, &window, [&] {
        if (finished.size() != 1)
            return;
        if (mode == 6)
            flag->setChecked(false);
        if (mode == 7)
            selector->setCurrentIndex(1);
        if (mode == 11)
            QVERIFY(test::writeMarkdownFixtureFile(
                fixture.source.path(), QStringLiteral("02/002.md"),
                QByteArray("# Changed\n\nNot the imported text.\n")));
        if (mode == 15)
            QVERIFY(window.openProjectFile(otherPath));
        if (mode == 16)
            window.close();
    });
    QTimer::singleShot(0, &window, [&window] {
        auto* dialog = window.findChild<QMessageBox*>(QStringLiteral("analysisSendConfirmation"));
        QVERIFY(dialog);
        dialog->findChild<QPushButton*>(QStringLiteral("analysisConfirmSend"))->click();
    });
    window
        .findChild<QPushButton*>(mode == 13 ? QStringLiteral("llmAnalyzeChapter")
                                            : QStringLiteral("dialogueSegmentChapter"))
        ->click();
    const bool completesBook = mode == 0 || mode == 2 || mode == 3 || mode == 4 || mode == 5 ||
                               mode == 12 || mode == 13 || (mode >= 19 && mode <= 22);
    const int expectedChapters = completesBook ? 3 : 1;
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), expectedChapters, 10000);
    QTRY_VERIFY(!flag->isChecked());
    QVERIFY(!controller->busy());
    QTest::qWait(30);
    const int expectedRequests = mode == 3                                               ? 3
                                 : mode == 12                                            ? 5
                                 : (mode == 19 || mode == 21)                            ? 6
                                 : completesBook                                         ? 9
                                 : (mode == 8 || mode == 17 || mode == 23 || mode == 24) ? 1
                                                                                         : 3;
    QCOMPARE(server.requests.size(), expectedRequests);
    if (mode == 23)
        QVERIFY(sourceRewritten);
    if (mode == 2)
        QVERIFY(enabledWhileBusy);
    if (completesBook || mode == 14)
        QCOMPARE(selector->currentIndex(), 2);
    if (mode == 7 || mode == 11)
        QCOMPARE(selector->currentIndex(), 1);
    if (mode != 15) {
        const auto entries = history(fixture);
        QCOMPARE(entries.size(), expectedRequests);
        const int expectedFailures = mode == 3 || mode == 19 || mode == 20 || mode == 21   ? 3
                                     : mode == 12                                          ? 2
                                     : mode == 8 || mode == 17 || mode == 23 || mode == 24 ? 1
                                                                                           : 0;
        int failures = 0;
        for (const auto& entry : entries) {
            if (entry.run.status != storage::LLMRunStatus::Succeeded)
                ++failures;
            const bool dialogue =
                entry.context.content.value(QStringLiteral("task_kind")).toString() ==
                QStringLiteral("dialogue");
            QCOMPARE(dialogue, mode != 13);
        }
        QCOMPARE(failures, expectedFailures);
    }
    auto* chapterTree = window.findChild<QTreeWidget*>(QStringLiteral("chapterTree"));
    QCOMPARE(chapterTree->columnCount(), 6);
    QCOMPARE(chapterTree->headerItem()->text(4), QStringLiteral("分析"));
    QCOMPARE(chapterTree->headerItem()->text(5), QStringLiteral("错字"));
    if (mode == 0 || mode == 2 || mode == 3 || mode == 4 || mode == 5 ||
        (mode >= 19 && mode <= 22)) {
        for (int i = 0; i < 3; ++i) {
            auto* row = chapterTree->topLevelItem(i);
            QVERIFY(!row->icon(4).isNull());
            QCOMPARE(row->text(4),
                     mode == 3 || mode == 19 || mode == 20 || mode == 21 ? QStringLiteral("失败")
                     : mode == 4 || mode == 5 || mode == 22              ? QStringLiteral("待确认")
                                                            : QStringLiteral("已分析"));
            QCOMPARE(row->text(5), mode == 3 ? QStringLiteral("待检查") : QStringLiteral("未发现"));
        }
    }
    app::MainWindow reopened;
    QVERIFY(reopened.openProjectFile(fixture.path));
    QVERIFY(
        !reopened.findChild<QCheckBox*>(QStringLiteral("analysisAutoNextChapter"))->isChecked());
}

void ChapterAnalysisPipelineTest::opensProjectInBackgroundAndDiscardsStaleResults() {
    Fixture first, second;
    QVERIFY(first.initialize(test::markdownFixtureChapter(), true));
    QVERIFY(second.initialize(test::markdownFixtureChapter(), true));
    app::MainWindow window;
    bool heartbeat = false;
    QTimer::singleShot(0, &window, [&] { heartbeat = true; });
    QVERIFY(window.openProjectFileAsync(first.path));
    QVERIFY(window.projectOpening());
    QTRY_VERIFY(!window.projectOpening());
    QVERIFY(heartbeat);
    auto* metadata = window.findChild<QLabel*>(QStringLiteral("bookMetadata"));
    QVERIFY(metadata);
    QVERIFY(metadata->text().contains(first.book.id.toString()));
    QVERIFY(window.openProjectFileAsync(first.path));
    QVERIFY(window.openProjectFileAsync(second.path));
    QTRY_VERIFY(!window.projectOpening());
    QVERIFY(metadata->text().contains(second.book.id.toString()));
    QTest::qWait(30);
    QVERIFY(metadata->text().contains(second.book.id.toString()));
    QVERIFY(window.openProjectFileAsync(first.path));
    QVERIFY(window.openProjectFile(second.path)); // Synchronous caller also supersedes old workers.
    QTest::qWait(30);
    QVERIFY(metadata->text().contains(second.book.id.toString()));
    QVERIFY(window.openProjectFileAsync(first.path));
    window.close();
    QVERIFY(!window.projectOpening());
    QTest::qWait(30);
    QVERIFY(metadata->text().contains(second.book.id.toString()));
    app::MainWindow missing;
    QVERIFY(missing.openProjectFileAsync(
        first.destination.filePath(QStringLiteral("missing.loreforge"))));
    QTRY_VERIFY(!missing.projectOpening());
    QVERIFY(missing.findChild<QLabel*>(QStringLiteral("workspaceStatus"))
                ->text()
                .contains(QStringLiteral("Open failed")));
}

void ChapterAnalysisPipelineTest::loadsLargeHistoryInBackgroundWhileReading() {
    Fixture fixture, other;
    QVERIFY(fixture.initialize(test::markdownFixtureChapter(), true));
    QVERIFY(other.initialize(test::markdownFixtureChapter(), true));
    Server server;
    server.makeOutput = [](const QJsonObject& user, int) {
        return user.contains(QStringLiteral("analysis")) ? reviewFor(user) : analysisFor(user);
    };
    Controller controller;
    QSignalSpy finished(&controller, &Controller::finished);
    for (int i = 0; i < 8; ++i) {
        controller.start(fixture.path, fixture.book, 0, fixture.preview, server.connection());
        QTRY_COMPARE(finished.size(), i + 1);
    }
    app::MainWindow window;
    QVERIFY(window.openProjectFile(fixture.path));
    QVERIFY(window.analysisHistoryLoading());
    auto* selector = window.findChild<QComboBox*>(QStringLiteral("analysisChapterSelector"));
    auto* tree = window.findChild<QTreeWidget*>(QStringLiteral("chapterTree"));
    auto* analyze = window.findChild<QPushButton*>(QStringLiteral("dialogueSegmentChapter"));
    QCOMPARE(tree->topLevelItem(0)->text(4), QStringLiteral("加载中"));
    QVERIFY(!analyze->isEnabled());
    QVERIFY(selector->isEnabled());
    selector->setCurrentIndex(
        1); // Reading and selection work before the background result arrives.
    QCOMPARE(tree->currentItem(), tree->topLevelItem(1));
    QTRY_VERIFY(!window.analysisHistoryLoading());
    QVERIFY(analyze->isEnabled());
    QCOMPARE(tree->currentItem(), tree->topLevelItem(1));
    QCOMPARE(tree->topLevelItem(0)->text(4), QStringLiteral("已分析"));
    QCOMPARE(tree->topLevelItem(1)->text(4), QStringLiteral("未分析"));
    QVERIFY(window.findChild<QLabel*>(QStringLiteral("analysisTokenTotals"))
                ->text()
                .contains(QStringLiteral("项目已知 336 token")));
    QVERIFY(window.openProjectFile(fixture.path));
    QVERIFY(window.analysisHistoryLoading());
    QVERIFY(window.openProjectFile(other.path));
    QTest::qWait(50);
    QVERIFY(!window.analysisHistoryLoading());
    QCOMPARE(tree->topLevelItem(0)->text(4), QStringLiteral("未分析"));
    QVERIFY(window.findChild<QLabel*>(QStringLiteral("bookMetadata"))
                ->text()
                .contains(other.book.id.toString()));
    QVERIFY(window.openProjectFile(fixture.path));
    QVERIFY(window.analysisHistoryLoading());
    window.close();
    QVERIFY(!window.analysisHistoryLoading());
    QTest::qWait(20);
    QCOMPARE(server.requests.size(), 24); // Background loading never sends novel text.
}

void ChapterAnalysisPipelineTest::switchesChaptersWithoutReloadingUnrelatedArtifacts() {
    Fixture fixture;
    QVERIFY(fixture.initialize(test::markdownFixtureChapter(), true));
    Server server;
    server.makeOutput = [](const QJsonObject& user, int) {
        return user.contains(QStringLiteral("analysis")) ? reviewFor(user) : analysisFor(user);
    };
    Controller controller;
    QSignalSpy finished(&controller, &Controller::finished);
    controller.start(fixture.path, fixture.book, 0, fixture.preview, server.connection());
    QTRY_COMPARE(finished.size(), 1);
    {
        auto opened = Database::open(fixture.path);
        auto database = std::get<std::unique_ptr<Database>>(std::move(opened));
        auto built = app::ChapterPreviewBuilder::build(fixture.projectId, fixture.book, 1, 32768,
                                                       4096, true);
        QVERIFY(std::holds_alternative<app::ChapterPreview>(built));
        auto stored =
            app::ChapterPreviewBuilder::store(*database, std::get<app::ChapterPreview>(built));
        QVERIFY(std::holds_alternative<app::ChapterPreview>(stored));
        controller.start(fixture.path, fixture.book, 1, std::get<app::ChapterPreview>(stored),
                         server.connection());
    }
    QTRY_COMPARE(finished.size(), 2);
    QHash<QString, QString> runChapters;
    const auto all = Controller::history(fixture.path, fixture.projectId, {}, &runChapters);
    QVERIFY(std::holds_alternative<QList<app::AnalysisHistoryEntry>>(all));
    QCOMPARE(runChapters.size(), 6);
    const auto entries = std::get<QList<app::AnalysisHistoryEntry>>(all);
    app::MainWindow window;
    QVERIFY(window.openProjectFile(fixture.path));
    auto* selector = window.findChild<QComboBox*>(QStringLiteral("analysisChapterSelector"));
    auto* tree = window.findChild<QTreeWidget*>(QStringLiteral("chapterTree"));
    auto* taskStatus = window.findChild<QLabel*>(QStringLiteral("analysisTaskStatus"));
    auto* totals = window.findChild<QLabel*>(QStringLiteral("analysisTokenTotals"));
    const auto expectedTotals = totals->text();
    QVERIFY(expectedTotals.contains(QStringLiteral("项目已知 84 token")));
    const auto connectionName = QUuid::createUuid().toString();
    {
        auto sql = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
        sql.setDatabaseName(fixture.path);
        QVERIFY(sql.open());
        QSqlQuery query(sql);
        const auto corrupt = [&](const QString& id) {
            query.prepare(QStringLiteral(
                "UPDATE llm_run_artifacts SET parsed_response_json='invalid' WHERE run_id=?"));
            query.addBindValue(id);
            return query.exec();
        };
        // Simulate a broken unrelated artifact AFTER the initial validated cache is built.
        QVERIFY(corrupt(entries.first().run.id.toString()));
        const auto chapter = fixture.book.chapters[1].id.toString();
        const auto selected =
            Controller::history(fixture.path, fixture.projectId, chapter, &runChapters);
        QVERIFY(std::holds_alternative<QList<app::AnalysisHistoryEntry>>(selected));
        QCOMPARE(std::get<QList<app::AnalysisHistoryEntry>>(selected).size(), 3);
        QVERIFY(std::holds_alternative<storage::StorageError>(
            Controller::history(fixture.path, fixture.projectId)));
        selector->setCurrentIndex(1); // First visit: only chapter 2 artifacts may be loaded.
        QVERIFY(taskStatus->text().contains(QStringLiteral("通过")));
        QCOMPARE(tree->topLevelItem(0)->text(4), QStringLiteral("已分析"));
        QCOMPARE(totals->text(), expectedTotals);
        QVERIFY(corrupt(entries.last().run.id.toString()));
        QElapsedTimer elapsed;
        elapsed.start();
        for (int i = 0; i < 10; ++i) {
            selector->setCurrentIndex(0);
            selector->setCurrentIndex(1); // Warm chapter history must not be re-read either.
            QVERIFY(taskStatus->text().contains(QStringLiteral("通过")));
        }
        qInfo() << "20 cached chapter switches, ms:" << elapsed.elapsed();
        QCOMPARE(totals->text(), expectedTotals);
        QVERIFY(window.openProjectFile(fixture.path)); // Reopening invalidates every cache.
        QCOMPARE(tree->topLevelItem(0)->text(4), QStringLiteral("读取失败"));
    }
    QSqlDatabase::removeDatabase(connectionName);
    QCOMPARE(server.requests.size(), 6);
}

void ChapterAnalysisPipelineTest::showsChapterIndicatorsAndPersistedProofreading() {
    Fixture fixture;
    QVERIFY(
        fixture.initialize(QStringLiteral("# 校对测试\n\n她说：“这是错自。”\n").toUtf8(), true));
    Server server;
    server.makeOutput = [](const QJsonObject& user, int index) {
        if (index == 2)
            return QJsonObject{}; // Failed reviewer must not hide analyst suggestions.
        if (index == 1)
            return reviewFor(user);
        auto output = analysisFor(user);
        for (const auto& value : user.value(QStringLiteral("source_units")).toArray()) {
            const auto unit = value.toObject();
            if (unit.value(QStringLiteral("text")).toString().contains(QStringLiteral("错自"))) {
                output.insert(
                    QStringLiteral("proofreading"),
                    QJsonArray{QJsonObject{
                        {QStringLiteral("unit_id"), unit.value(QStringLiteral("unit_id"))},
                        {QStringLiteral("original"), QStringLiteral("错自")},
                        {QStringLiteral("suggestion"), QStringLiteral("错字")},
                        {QStringLiteral("reason"), QStringLiteral("疑似同音字误用")}}});
            }
        }
        return output;
    };
    Controller controller;
    QSignalSpy finished(&controller, &Controller::finished);
    controller.start(fixture.path, fixture.book, 0, fixture.preview, server.connection());
    QTRY_COMPARE(finished.size(), 1);
    for (int attempt = 0; attempt < 2; ++attempt) {
        app::MainWindow window;
        QVERIFY(window.openProjectFile(fixture.path));
        auto* tree = window.findChild<QTreeWidget*>(QStringLiteral("chapterTree"));
        QCOMPARE(tree->topLevelItem(0)->text(4), QStringLiteral("失败"));
        QCOMPARE(tree->topLevelItem(0)->text(5), QStringLiteral("疑似 1 处"));
        QCOMPARE(tree->topLevelItem(1)->text(4), QStringLiteral("未分析"));
        QCOMPARE(tree->topLevelItem(1)->text(5), QStringLiteral("待检查"));
        const auto imagePath = qEnvironmentVariable("LOREFORGE_CHAPTER_TREE_IMAGE");
        if (!imagePath.isEmpty()) {
            const auto font =
                QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/msyh.ttc"));
            const auto families = QFontDatabase::applicationFontFamilies(font);
            if (!families.isEmpty())
                window.setFont(QFont(families.first(), 10));
            window.resize(1500, 950);
            window.show();
            QTest::qWait(30);
            QVERIFY(window.grab().save(imagePath + QStringLiteral("-tree.png")));
        }
        QTimer::singleShot(0, &window, [&] {
            auto* dialog = window.findChild<QDialog*>(QStringLiteral("chapterProofreadingDialog"));
            QVERIFY(dialog);
            auto* source =
                dialog->findChild<QPlainTextEdit*>(QStringLiteral("chapterProofreadingSource"));
            QCOMPARE(source->textCursor().selectedText(), QStringLiteral("错自"));
            QVERIFY(source->isReadOnly());
            auto* issues =
                dialog->findChild<QTreeWidget*>(QStringLiteral("chapterProofreadingIssues"));
            QCOMPARE(issues->topLevelItem(0)->text(1), QStringLiteral("错字"));
            if (!imagePath.isEmpty())
                QVERIFY(dialog->grab().save(imagePath + QStringLiteral("-issues.png")));
            dialog->reject();
        });
        tree->itemClicked(tree->topLevelItem(0), 5);
    }
    QCOMPARE(server.requests.size(), 3); // Viewing and reopening never sends novel text.
}

void ChapterAnalysisPipelineTest::preservesPartialExtractionWithoutPretendingItFullyPassed() {
    Fixture fixture;
    QVERIFY(fixture.initialize(
        QStringLiteral("# 部分提取\n\n“别动。”她停下。\n“……”她低头。\n……").toUtf8(), true));
    Server server;
    server.makeOutput = [](const QJsonObject& user, int) {
        if (user.contains(QStringLiteral("analysis")))
            return reviewFor(user);
        auto wire = analysisFor(user);
        auto parts = wire.value(QStringLiteral("segments")).toArray();
        parts.append(QJsonObject{{QStringLiteral("first_unit"), QStringLiteral("u9999")},
                                 {QStringLiteral("last_unit"), QStringLiteral("u9999")},
                                 {QStringLiteral("speaker"), QStringLiteral("她")}});
        wire.insert(QStringLiteral("segments"), parts);
        return wire;
    };
    Controller controller;
    QSignalSpy finished(&controller, &Controller::finished);
    QSignalSpy taskStatus(&controller, &Controller::taskStatus);
    controller.start(fixture.path, fixture.book, 0, fixture.preview, server.connection());
    QTRY_COMPARE(finished.size(), 1);
    QCOMPARE(server.requests.size(), 3);
    QVERIFY(finished[0][0].toString().contains(QStringLiteral("部分提取可查看")));
    QCOMPARE(qvariant_cast<app::TaskState>(taskStatus.last()[0]), app::TaskState::Attention);
    const auto entries = history(fixture);
    QCOMPARE(entries[0].run.status, storage::LLMRunStatus::Succeeded);
    const auto mapped = entries[0].outcome.value(QStringLiteral("normalized_output")).toObject();
    QCOMPARE(mapped.value(QStringLiteral("extraction_issues")).toArray().size(), 1);
    int turns = 0;
    for (const auto& value : mapped.value(QStringLiteral("segments")).toArray())
        if (value.toObject().value(QStringLiteral("kind")).toString() == QStringLiteral("dialogue"))
            ++turns;
    QCOMPARE(turns, 2);
    app::NovelAnalysisWorkbench page;
    page.setSelectedBook(&fixture.book, 0);
    page.showHistory(entries, fixture.book.chapters[0].id.toString());
    QVERIFY(page.findChild<QLabel*>(QStringLiteral("analysisTaskStatus"))
                ->text()
                .contains(QStringLiteral("部分提取")));
    const auto* people = qobject_cast<QTreeWidget*>(
        page.findChild<QTabWidget*>(QStringLiteral("analysisResultTabs"))->widget(0));
    QCOMPARE(people->topLevelItemCount(), 2);
    QCOMPARE(people->topLevelItem(1)->text(0), QStringLiteral("待确认"));
    QVERIFY(people->topLevelItem(1)->childCount() > 0);
}

void ChapterAnalysisPipelineTest::confirmsIndependentDialogueFromMainWindow() {
    Fixture fixture;
    QVERIFY(fixture.initialize(
        QStringLiteral("# 门边\n\n“……”她低头。\n“我……我不……”她退开。\n").toUtf8()));
    Server server;
    app::MainWindow window;
    QVERIFY(window.openProjectFile(fixture.path));
    auto* llm = window.findChild<QWidget*>(QStringLiteral("llmWorkbench"));
    llm->findChild<QLineEdit*>(QStringLiteral("llmEndpoint"))
        ->setText(server.connection().options.endpoint.toString());
    llm->findChild<QLineEdit*>(QStringLiteral("llmModelId"))
        ->setText(QStringLiteral("fixture-model"));
    llm->findChild<QLineEdit*>(QStringLiteral("llmApiKey"))
        ->setText(QStringLiteral("dialogue-test-key"));
    llm->findChild<QCheckBox*>(QStringLiteral("llmAllowHttp"))->setChecked(true);
    llm->findChild<QComboBox*>(QStringLiteral("llmResponseFormat"))
        ->setCurrentText(QStringLiteral("json_object"));
    auto* start = window.findChild<QPushButton*>(QStringLiteral("dialogueSegmentChapter"));
    QVERIFY(start->isEnabled());
    QTimer::singleShot(0, &window, [&window] {
        auto* dialog = window.findChild<QMessageBox*>(QStringLiteral("analysisSendConfirmation"));
        QVERIFY(dialog);
        QVERIFY(dialog->text().contains(QStringLiteral("独立人物与对白分割")));
        dialog->reject();
    });
    start->click();
    QCOMPARE(server.requests.size(), 0);
    QTimer::singleShot(0, &window, [&window] {
        auto* dialog = window.findChild<QMessageBox*>(QStringLiteral("analysisSendConfirmation"));
        QVERIFY(dialog);
        dialog->findChild<QPushButton*>(QStringLiteral("analysisConfirmSend"))->click();
    });
    start->click();
    QTRY_VERIFY(window.findChild<QLabel*>(QStringLiteral("analysisTaskStatus"))
                    ->text()
                    .startsWith(QStringLiteral("通过（")));
    QCOMPARE(server.requests.size(), 3);
    auto entries = history(fixture);
    QVERIFY(entries.last().context.content.value(QStringLiteral("task_kind")).toString() ==
            QStringLiteral("dialogue"));
    auto* highlight = window.findChild<app::CharacterHighlightText*>();
    QVERIFY(!highlight->spans().isEmpty());
    const auto* people = qobject_cast<QTreeWidget*>(
        window.findChild<QTabWidget*>(QStringLiteral("analysisResultTabs"))->widget(0));
    QCOMPARE(people->topLevelItem(0)->childCount(), 2);
    if (const auto path = qEnvironmentVariable("LOREFORGE_DIALOGUE_PAGE_IMAGE"); !path.isEmpty()) {
        const auto font =
            QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/msyh.ttc"));
        const auto families = QFontDatabase::applicationFontFamilies(font);
        if (!families.isEmpty())
            window.setFont(QFont(families.first(), 10));
        window.findChild<QTabWidget*>(QStringLiteral("workspacePages"))->setCurrentIndex(2);
        window.resize(1400, 1200);
        window.show();
        QTest::qWait(30);
        QVERIFY(window.grab().save(path));
    }
}

void ChapterAnalysisPipelineTest::independentlySegmentsAndContinuesWithoutRepeatingExtraction() {
    Fixture fixture;
    QVERIFY(fixture.initialize(QStringLiteral("# "
                                              "独立分割\r\n\r\n“……”她低下头。\r\n“呜……呜……”她哭了。"
                                              "\r\n“……”她又沉默。\r\n“我……我不……”她退开。\r\n")
                                   .toUtf8(),
                               true));
    Server server;
    server.makeOutput = [](const QJsonObject& user, int index) {
        if (!user.contains(QStringLiteral("analysis")))
            return analysisFor(user);
        auto review = reviewFor(user);
        if (index == 1)
            review.insert(QStringLiteral("chapter_id"), QStringLiteral("wrong-chapter"));
        return review;
    };
    Controller controller;
    QSignalSpy finished(&controller, &Controller::finished);
    controller.start(fixture.path, fixture.book, 0, fixture.preview, server.connection());
    QTRY_COMPARE(finished.size(), 1);
    QCOMPARE(server.requests.size(), 2);
    auto entries = history(fixture);
    QCOMPARE(entries[0].run.status, storage::LLMRunStatus::Succeeded);
    QCOMPARE(entries[1].run.status, storage::LLMRunStatus::Failed);
    const auto normalized =
        entries[0].outcome.value(QStringLiteral("normalized_output")).toObject();
    int utterances = 0, silence = 0;
    QSet<qint64> repeatedPositions;
    for (const auto& value : normalized.value(QStringLiteral("segments")).toArray()) {
        const auto part = value.toObject();
        if (part.value(QStringLiteral("kind")).toString() == QStringLiteral("dialogue"))
            ++utterances;
        if (part.value(QStringLiteral("text")).toString() == QStringLiteral("“……”")) {
            ++silence;
            repeatedPositions.insert(part.value(QStringLiteral("source_start")).toInteger());
        }
    }
    QCOMPARE(utterances, 4);
    QCOMPARE(silence, 2);
    QCOMPARE(repeatedPositions.size(), 2);
    QVERIFY(!normalized.contains(QStringLiteral("events")));
    const auto user = QJsonDocument::fromJson(server.requests[0]
                                                  .value(QStringLiteral("messages"))
                                                  .toArray()[1]
                                                  .toObject()
                                                  .value(QStringLiteral("content"))
                                                  .toString()
                                                  .toUtf8())
                          .object();
    QVERIFY(user.contains(QStringLiteral("source_units")));
    QVERIFY(!user.contains(QStringLiteral("source")));
    QVERIFY(!user.contains(QStringLiteral("source_blocks")));
    const auto raw = entries[0].artifacts.parsedResponse->object();
    QVERIFY(!raw.value(QStringLiteral("segments"))
                 .toArray()[0]
                 .toObject()
                 .contains(QStringLiteral("text")));
    const auto pipeline =
        entries[0].context.content.value(QStringLiteral("pipeline_id")).toString();
    controller.start(fixture.path, fixture.book, 0, fixture.preview, server.connection(), pipeline);
    QTRY_COMPARE(finished.size(), 2);
    QCOMPARE(server.requests.size(), 4);
    entries = history(fixture);
    QCOMPARE(entries.last().run.status, storage::LLMRunStatus::Succeeded);
    QCOMPARE(app::completedRolePrefix(app::latestPipelineRoles(entries, pipeline)), 3);
    app::NovelAnalysisWorkbench page;
    page.setSelectedBook(&fixture.book, 0);
    QSignalSpy preview(&page, &app::NovelAnalysisWorkbench::highlightSourceChanged);
    page.showHistory(entries, fixture.book.chapters[0].id.toString());
    QVERIFY(!preview.isEmpty());
    QVERIFY(!qvariant_cast<QJsonObject>(preview.last()[2])
                 .value(QStringLiteral("segments"))
                 .toArray()
                 .isEmpty());
    QVERIFY(page.findChild<QPushButton*>(QStringLiteral("dialogueSegmentChapter"))->isEnabled());
    const auto* people = qobject_cast<QTreeWidget*>(
        page.findChild<QTabWidget*>(QStringLiteral("analysisResultTabs"))->widget(0));
    QCOMPARE(people->topLevelItem(0)->childCount(), 4);
    QVERIFY(people->topLevelItem(0)->child(0)->text(0).contains(QStringLiteral("……")));
    QCOMPARE(people->topLevelItemCount(), 1); // No empty pending group.
    auto unknownEntries = entries;
    auto wire = unknownEntries[0].artifacts.parsedResponse->object();
    auto parts = wire.value(QStringLiteral("segments")).toArray();
    auto first = parts[0].toObject();
    first.insert(QStringLiteral("speaker"), QString{});
    parts[0] = first;
    wire.insert(QStringLiteral("segments"), parts);
    unknownEntries[0].artifacts.parsedResponse = QJsonDocument(wire);
    page.showHistory(unknownEntries, fixture.book.chapters[0].id.toString());
    QCOMPARE(people->topLevelItemCount(), 2);
    QCOMPARE(people->topLevelItem(1)->text(0), QStringLiteral("待确认"));
    QCOMPARE(people->topLevelItem(1)->childCount(), 1);
    page.showHistory(entries, fixture.book.chapters[0].id.toString());
    QCOMPARE(people->topLevelItemCount(), 1);
}

void ChapterAnalysisPipelineTest::refusesCoarseNarrationWithoutForcingQuotesToDialogue() {
    Fixture fixture;
    QVERIFY(fixture.initialize(
        QStringLiteral(
            "# 边界测试\n\n“这是什么药？”她的眼神变得严肃。\n\n她把“甜味”这个词写下来。\n")
            .toUtf8()));
    const auto user =
        QJsonDocument::fromJson(fixture.preview.snapshot.content.value(QStringLiteral("messages"))
                                    .toArray()[1]
                                    .toObject()
                                    .value(QStringLiteral("content"))
                                    .toString()
                                    .toUtf8())
            .object();
    auto output = analysisFor(user);
    auto segments = output.value(QStringLiteral("segments")).toArray();
    QVERIFY(segments.size() > 2);
    auto quote = segments[1].toObject();
    quote.insert(QStringLiteral("kind"), QStringLiteral("dialogue"));
    quote.insert(QStringLiteral("speaker"), QStringLiteral("她"));
    segments[1] = quote;
    output.insert(QStringLiteral("segments"), segments);
    QVERIFY(app::resolveChapterEvidence(output, fixture.preview.snapshot.content, false)
                .errors.isEmpty());
    // The later quoted term is still narration, not a fabricated speaking turn.
    QCOMPARE(segments[3].toObject().value(QStringLiteral("kind")).toString(),
             QStringLiteral("narration"));
    const auto raw = user.value(QStringLiteral("source")).toObject().value(QStringLiteral("utf8"));
    output.insert(QStringLiteral("segments"),
                  QJsonArray{QJsonObject{{QStringLiteral("text"), raw},
                                         {QStringLiteral("kind"), QStringLiteral("narration")},
                                         {QStringLiteral("speaker"), QString{}}}});
    const auto rejected =
        app::resolveChapterEvidence(output, fixture.preview.snapshot.content, false);
    QVERIFY(!rejected.errors.isEmpty());
    QVERIFY(rejected.errors.join('\n').contains(QStringLiteral("引号片段")));
    QVERIFY(rejected.output.isEmpty());
    // Covers other quotation styles, nesting and repeated text without substring guessing.
    const auto ranges = app::quotationRanges(QStringLiteral("甲「乙『丙』丁」戊\"己\"庚“辛”"));
    QCOMPARE(ranges.size(), 3);
    QCOMPARE(ranges[0], (QPair<qsizetype, qsizetype>{1, 8}));
}

void ChapterAnalysisPipelineTest::validatesCompleteNovelSegmentation() {
    Fixture fixture;
    QVERIFY(fixture.initialize());
    const auto user =
        QJsonDocument::fromJson(fixture.preview.snapshot.content.value(QStringLiteral("messages"))
                                    .toArray()[1]
                                    .toObject()
                                    .value(QStringLiteral("content"))
                                    .toString()
                                    .toUtf8())
            .object();
    const auto valid = analysisFor(user);
    const auto check = [&](const QJsonObject& output) {
        return app::resolveChapterEvidence(output, fixture.preview.snapshot.content, false);
    };
    const auto resolved = check(valid);
    QVERIFY2(resolved.errors.isEmpty(), qPrintable(resolved.errors.join('\n')));
    const auto segment = resolved.output.value(QStringLiteral("segments")).toArray()[0].toObject();
    const auto source = fixture.preview.snapshot.content.value(QStringLiteral("source")).toObject();
    QCOMPARE(segment.value(QStringLiteral("source_start")),
             source.value(QStringLiteral("start_byte")));
    QCOMPARE(segment.value(QStringLiteral("source_end")), source.value(QStringLiteral("end_byte")));
    for (int mode = 0; mode < 7; ++mode) {
        auto output = valid;
        auto parts = output.value(QStringLiteral("segments")).toArray();
        auto part = parts[0].toObject();
        if (mode == 0)
            output.remove(QStringLiteral("segments"));
        else {
            if (mode == 1)
                part.insert(QStringLiteral("text"),
                            part.value(QStringLiteral("text")).toString().chopped(1));
            if (mode == 2)
                part.insert(QStringLiteral("text"), QStringLiteral("改写原文"));
            if (mode == 3)
                part.insert(QStringLiteral("speaker"),
                            QStringLiteral("她")); // narration cannot have a speaker
            if (mode == 4) {
                part.insert(QStringLiteral("kind"), QStringLiteral("dialogue"));
                part.insert(QStringLiteral("speaker"), QStringLiteral("不在人物表中"));
            }
            if (mode == 5)
                part.insert(QStringLiteral("kind"), QStringLiteral("action"));
            parts[0] = part;
            if (mode == 6)
                parts.append(part); // duplicate coverage
            output.insert(QStringLiteral("segments"), parts);
        }
        QVERIFY2(!check(output).errors.isEmpty(), qPrintable(QString::number(mode)));
    }
    // Unknown speaker is explicit, not guessed from a name occurring in narration.
    auto output = valid;
    auto part = output.value(QStringLiteral("segments")).toArray()[0].toObject();
    part.insert(QStringLiteral("kind"), QStringLiteral("dialogue"));
    output.insert(QStringLiteral("segments"), QJsonArray{part});
    QVERIFY(check(output).errors.isEmpty());
}

void ChapterAnalysisPipelineTest::resolvesAllClaimsAndAuditsRawIds() {
    Fixture fixture;
    QVERIFY(fixture.initialize(
        QByteArray("\xef\xbb\xbf") +
        QStringLiteral("# 编号证据\r\n\r\n阿明（绰号老狼）站在码头😀，看见一封未拆信。\r\n")
            .toUtf8()));
    Server server;
    server.makeOutput = [](const QJsonObject& user, int role) {
        if (role > 0)
            return reviewFor(user, QStringLiteral("redo"));
        auto output = analysisFor(user);
        auto claim = output.value(QStringLiteral("summary")).toObject();
        claim.insert(QStringLiteral("text"), QStringLiteral("阿明看见一封未拆信。"));
        output.insert(QStringLiteral("summary"), claim);
        output.insert(QStringLiteral("important_facts"), QJsonArray{claim});
        output.insert(QStringLiteral("open_threads"), QJsonArray{claim});
        auto named = claim;
        named.remove(QStringLiteral("text"));
        named.insert(QStringLiteral("name"), QStringLiteral("码头"));
        output.insert(QStringLiteral("locations"), QJsonArray{named});
        named.insert(QStringLiteral("name"), QStringLiteral("老狼"));
        auto character = named;
        character.insert(QStringLiteral("name"), QStringLiteral("阿明"));
        character.insert(QStringLiteral("aliases"), QJsonArray{named});
        output.insert(QStringLiteral("characters"), QJsonArray{character});
        auto event = claim;
        event.remove(QStringLiteral("text"));
        event.insert(QStringLiteral("description"), QStringLiteral("阿明在码头看见信。"));
        event.insert(QStringLiteral("participants"), QJsonArray{QStringLiteral("阿明")});
        event.insert(QStringLiteral("location"), QStringLiteral("码头"));
        output.insert(QStringLiteral("events"), QJsonArray{event});
        return output;
    };
    Controller controller;
    QSignalSpy finished(&controller, &Controller::finished);
    controller.start(fixture.path, fixture.book, 0, fixture.preview, server.connection());
    QTRY_COMPARE(finished.size(), 1);
    QCOMPARE(server.requests.size(), 3);
    const auto entries = history(fixture);
    QCOMPARE(entries.size(), 3);
    for (int role = 0; role < 3; ++role) {
        const auto& entry = entries[role];
        QCOMPARE(entry.run.status, storage::LLMRunStatus::Succeeded);
        const auto wire = entry.artifacts.parsedResponse->object();
        const auto rawJson = inference::canonicalJson(wire);
        QVERIFY(rawJson.contains("block_id"));
        QVERIFY(!rawJson.contains("source_start"));
        const auto resolved = app::resolveChapterEvidence(wire, entry.context.content, role > 0);
        QVERIFY2(resolved.errors.isEmpty(), qPrintable(resolved.errors.join(QLatin1Char('\n'))));
        QCOMPARE(entry.outcome.value(QStringLiteral("normalized_output")).toObject(),
                 resolved.output);
        QVERIFY(inference::canonicalJson(resolved.output).contains("source_start"));
        QVERIFY(!inference::canonicalJson(resolved.output).contains("block_id"));
        QVERIFY(entry.artifacts.rawResponse->contains("block_id"));
    }
    const auto userFor = [&](int role) {
        return QJsonDocument::fromJson(server.requests[role]
                                           .value(QStringLiteral("messages"))
                                           .toArray()[1]
                                           .toObject()
                                           .value(QStringLiteral("content"))
                                           .toString()
                                           .toUtf8())
            .object();
    };
    QCOMPARE(userFor(1).value(QStringLiteral("analysis")).toObject(),
             entries[0].artifacts.parsedResponse->object());
    QCOMPARE(userFor(2).value(QStringLiteral("checker")).toObject(),
             entries[1].artifacts.parsedResponse->object());
    app::NovelAnalysisWorkbench page;
    page.setSelectedBook(&fixture.book, 0);
    page.showHistory(entries, fixture.book.chapters[0].id.toString());
    auto* tabs = page.findChild<QTabWidget*>(QStringLiteral("analysisResultTabs"));
    for (int index = 0; index < 5; ++index)
        QVERIFY(qobject_cast<QTreeWidget*>(tabs->widget(index))->topLevelItemCount() > 0);
}

void ChapterAnalysisPipelineTest::refusesInvalidBlockReferences_data() {
    QTest::addColumn<int>("role");
    QTest::addColumn<int>("mode");
    QTest::newRow("analyzer-unknown") << 0 << 0;
    QTest::newRow("checker-unknown") << 1 << 0;
    QTest::newRow("reviewer-unknown") << 2 << 0;
    QTest::newRow("duplicate-evidence") << 0 << 1;
    QTest::newRow("raw-byte-offset") << 0 << 2;
    QTest::newRow("wrong-chapter-reference") << 0 << 3;
}

void ChapterAnalysisPipelineTest::refusesInvalidBlockReferences() {
    QFETCH(int, role);
    QFETCH(int, mode);
    Fixture fixture;
    QVERIFY(fixture.initialize());
    Server server;
    server.makeOutput = [=](const QJsonObject& user, int index) {
        auto output = index == 0 ? analysisFor(user) : reviewFor(user, QStringLiteral("redo"));
        if (index != role)
            return output;
        if (role == 0) {
            auto claim = output.value(QStringLiteral("summary")).toObject();
            auto references = claim.value(QStringLiteral("evidence")).toArray();
            auto reference = references[0].toObject();
            if (mode == 0 || mode == 3)
                reference.insert(QStringLiteral("block_id"),
                                 mode == 0 ? QStringLiteral("b9999")
                                           : QStringLiteral("other-chapter:b0002"));
            if (mode == 2)
                reference = {{QStringLiteral("source_start"), 1},
                             {QStringLiteral("source_end"), 2}};
            references[0] = reference;
            if (mode == 1)
                references.append(reference);
            claim.insert(QStringLiteral("evidence"), references);
            output.insert(QStringLiteral("summary"), claim);
        } else {
            auto issues = output.value(QStringLiteral("issues")).toArray();
            auto issue = issues[0].toObject();
            issue.insert(QStringLiteral("block_id"), QStringLiteral("b9999"));
            issues[0] = issue;
            output.insert(QStringLiteral("issues"), issues);
        }
        return output;
    };
    Controller controller;
    QSignalSpy finished(&controller, &Controller::finished);
    controller.start(fixture.path, fixture.book, 0, fixture.preview, server.connection());
    QTRY_COMPARE(finished.size(), 1);
    QCOMPARE(server.requests.size(), role + 1);
    const auto entries = history(fixture);
    QCOMPARE(entries.last().run.status, storage::LLMRunStatus::Failed);
    QVERIFY(entries.last().outcome.value(QStringLiteral("normalized_output")).toObject().isEmpty());
    QVERIFY(entries.last().artifacts.parsedResponse.has_value());
}

void ChapterAnalysisPipelineTest::refusesInvalidMappingAndRendersLegacyHistory() {
    Fixture fixture;
    QVERIFY(fixture.initialize());
    const auto user =
        QJsonDocument::fromJson(fixture.preview.snapshot.content.value(QStringLiteral("messages"))
                                    .toArray()[1]
                                    .toObject()
                                    .value(QStringLiteral("content"))
                                    .toString()
                                    .toUtf8())
            .object();
    const auto wire = analysisFor(user);
    const auto resolved =
        app::resolveChapterEvidence(wire, fixture.preview.snapshot.content, false);
    QVERIFY(resolved.errors.isEmpty());
    auto corrupt = fixture.preview.snapshot.content;
    auto blocks = corrupt.value(QStringLiteral("blocks")).toArray();
    auto block = blocks[1].toObject();
    block.insert(QStringLiteral("start_byte"),
                 block.value(QStringLiteral("start_byte")).toInteger() + 1);
    blocks[1] = block;
    corrupt.insert(QStringLiteral("blocks"), blocks);
    QVERIFY(!app::resolveChapterEvidence(wire, corrupt, false).errors.isEmpty());
    app::AnalysisHistoryEntry legacy;
    legacy.run.status = storage::LLMRunStatus::Succeeded;
    legacy.run.id = core::LLMRunId::fromStableKey(QStringLiteral("legacy-byte-output"));
    legacy.artifacts.parsedResponse = QJsonDocument(resolved.output);
    legacy.artifacts.validation = {inference::ValidationStatus::Valid, {}};
    legacy.artifacts.outputSchemaVersion = 1;
    legacy.artifacts.promptVersion = 2;
    legacy.context = fixture.preview.snapshot;
    legacy.context.content.insert(QStringLiteral("role_index"), 0);
    legacy.context.content.insert(QStringLiteral("pipeline_id"),
                                  QStringLiteral("legacy-byte-pipeline"));
    app::NovelAnalysisWorkbench page;
    page.setSelectedBook(&fixture.book, 0);
    page.showHistory({legacy}, fixture.book.chapters[0].id.toString());
    auto* tabs = page.findChild<QTabWidget*>(QStringLiteral("analysisResultTabs"));
    QVERIFY(qobject_cast<QTreeWidget*>(tabs->widget(0))->topLevelItemCount() > 0);
    QVERIFY(page.findChild<QPushButton*>(QStringLiteral("analysisContinueTask"))->isHidden());
}

void ChapterAnalysisPipelineTest::monitorShowsOnlyTheCurrentRequest() {
    app::StreamMonitorWidget monitor;
    auto* text = monitor.findChild<QPlainTextEdit*>(QStringLiteral("streamMonitorMessages"));
    auto* status = monitor.findChild<QLabel*>(QStringLiteral("streamMonitorStatus"));
    QVERIFY(text->isReadOnly());
    const auto old = QUuid::createUuid();
    const auto current = QUuid::createUuid();
    monitor.beginRequest(old, QStringLiteral("分析员"));
    monitor.updateRequest(old, QStringLiteral("旧内容"), true);
    QTRY_COMPARE(text->toPlainText(), QStringLiteral("旧内容"));
    QVERIFY(status->text().contains(QStringLiteral("分析中")));
    monitor.beginRequest(current, QStringLiteral("复核员"));
    QVERIFY(text->toPlainText().isEmpty());
    monitor.updateRequest(old, QStringLiteral("迟到的旧内容"), true);
    monitor.finishRequest(old, QStringLiteral("旧请求完成"));
    QVERIFY(text->toPlainText().isEmpty());
    monitor.updateRequest(current, QStringLiteral("<b>新内容</b>"), false);
    monitor.finishRequest(current, QStringLiteral("已取消"));
    QCOMPARE(text->toPlainText(), QStringLiteral("<b>新内容</b>"));
    QVERIFY(status->text().contains(QStringLiteral("已取消")));
    monitor.updateRequest(current, QStringLiteral("取消后的迟到数据"), true);
    QCOMPARE(text->toPlainText(), QStringLiteral("<b>新内容</b>"));
}

void ChapterAnalysisPipelineTest::acceptsJsonFallbackWithoutPretendingItIsSse() {
    Fixture fixture;
    QVERIFY(fixture.initialize());
    Server server;
    server.streaming = false;
    Controller controller;
    QSignalSpy finished(&controller, &Controller::finished);
    QSignalSpy streams(&controller, &Controller::streamFinished);
    controller.start(fixture.path, fixture.book, 0, fixture.preview, server.connection());
    QTRY_COMPARE(finished.size(), 1);
    QCOMPARE(server.requests.size(), 3);
    QCOMPARE(streams.size(), 3);
    for (const auto& result : streams)
        QVERIFY(result[1].toString().contains(QStringLiteral("非 SSE")));
}

void ChapterAnalysisPipelineTest::carriesAliasRulesAndPreservesSemanticVerdicts_data() {
    QTest::addColumn<QString>("source");
    QTest::addColumn<QString>("name");
    QTest::addColumn<QString>("alias");
    QTest::addColumn<QString>("verdict");
    QTest::newRow("self-reference-misclassified")
        << QStringLiteral("张明说：老子今天就不走了！") << QStringLiteral("张明")
        << QStringLiteral("老子") << QStringLiteral("redo");
    QTest::newRow("kinship-not-an-alias")
        << QStringLiteral("张明说：我老子不让我去。") << QStringLiteral("张明") << QString()
        << QStringLiteral("pass");
    QTest::newRow("self-reference-wrongly-attached-to-father")
        << QStringLiteral("父亲在门外。张明说：老子今天就不走了！") << QStringLiteral("父亲")
        << QStringLiteral("老子") << QStringLiteral("redo");
    QTest::newRow("historical-person-not-blacklisted")
        << QStringLiteral("老子提出了自己的看法。") << QStringLiteral("老子") << QString()
        << QStringLiteral("pass");
    QTest::newRow("explicit-nickname")
        << QStringLiteral("大家都叫张明老狼。") << QStringLiteral("张明") << QStringLiteral("老狼")
        << QStringLiteral("pass");
    QTest::newRow("explicit-name-not-blacklisted")
        << QStringLiteral("张明的绰号就是老子，伙伴一直这样叫他。") << QStringLiteral("张明")
        << QStringLiteral("老子") << QStringLiteral("pass");
}

void ChapterAnalysisPipelineTest::carriesAliasRulesAndPreservesSemanticVerdicts() {
    QFETCH(QString, source);
    QFETCH(QString, name);
    QFETCH(QString, alias);
    QFETCH(QString, verdict);
    Fixture fixture;
    QVERIFY(fixture.initialize(QStringLiteral("# 测试章\n\n%1\n").arg(source).toUtf8()));
    Server server;
    // These are scripted semantic decisions, not a claim to evaluate a real model.
    server.makeOutput = [&](const QJsonObject& user, int index) {
        if (index > 0) {
            auto review = reviewFor(user, verdict);
            if (verdict == QStringLiteral("redo")) {
                auto issues = review.value(QStringLiteral("issues")).toArray();
                auto issue = issues[0].toObject();
                issue.insert(
                    QStringLiteral("reason"),
                    QStringLiteral("%1的别名老子实际是说话者自称，不能归为该人物的固定别名。")
                        .arg(name));
                issues[0] = issue;
                review.insert(QStringLiteral("issues"), issues);
            }
            return review;
        }
        auto output = analysisFor(user);
        auto character = output.value(QStringLiteral("characters")).toArray()[0].toObject();
        character.insert(QStringLiteral("name"), name);
        if (!alias.isEmpty()) {
            auto claim = character;
            claim.remove(QStringLiteral("aliases"));
            claim.insert(QStringLiteral("name"), alias);
            character.insert(QStringLiteral("aliases"), QJsonArray{claim});
        }
        output.insert(QStringLiteral("characters"), QJsonArray{character});
        return output;
    };
    Controller controller;
    QSignalSpy finished(&controller, &Controller::finished);
    controller.start(fixture.path, fixture.book, 0, fixture.preview, server.connection());
    QTRY_COMPARE(finished.size(), 1);
    QCOMPARE(server.requests.size(), 3);
    const auto entries = history(fixture);
    QCOMPARE(entries.size(), 3);
    const auto savedCharacter = entries[0]
                                    .artifacts.parsedResponse->object()
                                    .value(QStringLiteral("characters"))
                                    .toArray()[0]
                                    .toObject();
    QCOMPARE(savedCharacter.value(QStringLiteral("name")).toString(), name);
    const auto savedAliases = savedCharacter.value(QStringLiteral("aliases")).toArray();
    QCOMPARE(savedAliases.size(), alias.isEmpty() ? 0 : 1);
    if (!alias.isEmpty())
        QCOMPARE(savedAliases[0].toObject().value(QStringLiteral("name")).toString(), alias);
    for (int role = 0; role < 3; ++role) {
        const auto system = server.requests[role]
                                .value(QStringLiteral("messages"))
                                .toArray()[0]
                                .toObject()
                                .value(QStringLiteral("content"))
                                .toString();
        QVERIFY(system.contains(QStringLiteral("指代某个人不等于这个人的固定别名")));
        QVERIFY(system.contains(QStringLiteral("说话者自称")));
        QVERIFY(system.contains(QStringLiteral("历史人物老子")));
        QVERIFY(system.contains(QStringLiteral("明确命名")));
        QVERIFY(system.contains(QStringLiteral("别名自己的 evidence")));
        if (role > 0) {
            QVERIFY(system.contains(QStringLiteral("逐个复核")));
            QVERIFY(system.contains(QStringLiteral("证据范围有效不代表语义正确")));
            QVERIFY(system.contains(QStringLiteral("analysis.segments")));
            QVERIFY(system.contains(QStringLiteral("对白边界")) ||
                    system.contains(QStringLiteral("对白仅含实际发言")));
        }
        QCOMPARE(entries[role].artifacts.promptVersion, app::chapterPromptVersion);
        QCOMPARE(entries[role].artifacts.outputSchemaVersion, app::chapterWireSchemaVersion);
        QCOMPARE(entries[role].run.status, storage::LLMRunStatus::Succeeded);
    }
    QCOMPARE(
        entries[2].artifacts.parsedResponse->object().value(QStringLiteral("verdict")).toString(),
        verdict);
    QVERIFY(finished[0][0].toString().contains(
        verdict == QStringLiteral("redo") ? QStringLiteral("自称") : QStringLiteral("通过")));
}

void ChapterAnalysisPipelineTest::continuesOnlyUnfinishedRoles_data() {
    QTest::addColumn<int>("failedRole");
    QTest::newRow("checker") << 1;
    QTest::newRow("reviewer") << 2;
}

void ChapterAnalysisPipelineTest::continuesOnlyUnfinishedRoles() {
    QFETCH(int, failedRole);
    Fixture fixture;
    QVERIFY(fixture.initialize());
    Server server;
    server.makeOutput = [&](const QJsonObject& user, int index) {
        if (index == failedRole)
            server.status = 500;
        return index == 0 ? analysisFor(user) : reviewFor(user);
    };
    QString pipeline;
    QList<app::AnalysisHistoryEntry> original;
    {
        Controller initial;
        QSignalSpy finished(&initial, &Controller::finished);
        initial.start(fixture.path, fixture.book, 0, fixture.preview, server.connection());
        QTRY_COMPARE(finished.size(), 1);
        original = history(fixture);
        QCOMPARE(original.size(), failedRole + 1);
        pipeline = original[0].context.content.value(QStringLiteral("pipeline_id")).toString();
    }
    server.status = 200;
    server.makeOutput = {};
    // A new controller simulates reopening the project; budgets and timeout may change.
    auto opened = Database::open(fixture.path);
    auto database = std::get<std::unique_ptr<Database>>(std::move(opened));
    auto built = app::ChapterPreviewBuilder::build(fixture.projectId, fixture.book, 0, 65536, 8192);
    auto stored =
        app::ChapterPreviewBuilder::store(*database, std::get<app::ChapterPreview>(built));
    auto preview = std::get<app::ChapterPreview>(stored);
    database.reset();
    auto connection = server.connection();
    connection.timeoutMs = 6000;
    Controller resumed;
    QSignalSpy finished(&resumed, &Controller::finished);
    resumed.start(fixture.path, fixture.book, 0, preview, connection, pipeline);
    resumed.cancel();
    QCOMPARE(finished.size(), 1);
    resumed.start(fixture.path, fixture.book, 0, preview, connection, pipeline);
    QTRY_COMPARE(finished.size(), 2);
    QVERIFY2(finished.last()[0].toString().contains(QStringLiteral("通过")),
             qPrintable(finished.last()[0].toString()));
    QCOMPARE(server.requests.size(), 4);
    const auto entries = history(fixture);
    QCOMPARE(entries.size(), 4);
    const auto roles = app::latestPipelineRoles(entries, pipeline);
    QCOMPARE(app::completedRolePrefix(roles), 3);
    for (const auto& old : original) {
        const auto found = std::find_if(entries.begin(), entries.end(), [&](const auto& entry) {
            return entry.run.id == old.run.id;
        });
        QVERIFY(found != entries.end());
        QCOMPARE(found->run, old.run);
        QCOMPARE(found->artifacts, old.artifacts);
        QCOMPARE(found->outcome, old.outcome);
    }
    for (int role = failedRole; role < 3; ++role) {
        QCOMPARE(
            roles[role]->context.content.value(QStringLiteral("reused_run_ids")).toArray().size(),
            failedRole);
        QCOMPARE(roles[role]->run.status, storage::LLMRunStatus::Succeeded);
    }
    QVERIFY(server.requests[failedRole + 1]
                .value(QStringLiteral("messages"))
                .toArray()[1]
                .toObject()
                .value(QStringLiteral("content"))
                .toString()
                .contains(QStringLiteral("analysis")));
    resumed.start(fixture.path, fixture.book, 0, preview, connection, pipeline);
    QTRY_COMPARE(finished.size(), 3);
    QCOMPARE(server.requests.size(), 4);
}

void ChapterAnalysisPipelineTest::refusesIncompatibleContinuation() {
    Fixture fixture;
    QVERIFY(fixture.initialize());
    Server server;
    server.makeOutput = [&](const QJsonObject& user, int index) {
        if (index == 1)
            server.status = 500;
        return index == 0 ? analysisFor(user) : reviewFor(user);
    };
    Controller controller;
    QSignalSpy finished(&controller, &Controller::finished);
    controller.start(fixture.path, fixture.book, 0, fixture.preview, server.connection());
    QTRY_COMPARE(finished.size(), 1);
    const auto pipeline =
        history(fixture)[0].context.content.value(QStringLiteral("pipeline_id")).toString();
    server.status = 200;
    server.makeOutput = {};
    for (int mode = 0; mode < 4; ++mode) {
        auto connection = server.connection();
        if (mode == 0)
            connection.options.defaultModel = QStringLiteral("different-model");
        if (mode == 1)
            connection.options.enableThinking = false;
        if (mode == 2)
            connection.responseFormat = QStringLiteral("json_schema");
        if (mode == 3)
            connection.options.endpoint.setPath(QStringLiteral("/different"));
        controller.start(fixture.path, fixture.book, 0, fixture.preview, connection, pipeline);
        QTRY_COMPARE(finished.size(), mode + 2);
        QCOMPARE(server.requests.size(), 2);
    }
    controller.start(fixture.path, fixture.book, 0, fixture.preview, server.connection(),
                     QStringLiteral("missing-pipeline"));
    QCOMPARE(finished.size(), 6);
    QCOMPARE(server.requests.size(), 2);
    QVERIFY(test::writeMarkdownFixtureFile(fixture.source.path(), QStringLiteral("02/010.md"),
                                           test::markdownFixtureChapter() + QByteArray("\n")));
    controller.start(fixture.path, fixture.book, 0, fixture.preview, server.connection(), pipeline);
    QTRY_COMPARE(finished.size(), 7);
    QCOMPARE(server.requests.size(), 2);
}

void ChapterAnalysisPipelineTest::confirmsContinuationAndShowsReviewReasons() {
    Fixture fixture;
    QVERIFY(fixture.initialize());
    Server server;
    server.makeOutput = [&](const QJsonObject& user, int index) {
        if (index == 2)
            server.status = 500;
        return index == 0 ? analysisFor(user) : reviewFor(user);
    };
    {
        Controller controller;
        QSignalSpy finished(&controller, &Controller::finished);
        controller.start(fixture.path, fixture.book, 0, fixture.preview, server.connection());
        QTRY_COMPARE(finished.size(), 1);
    }
    app::MainWindow window;
    QVERIFY(window.openProjectFile(fixture.path));
    window.findChild<QLineEdit*>(QStringLiteral("llmEndpoint"))
        ->setText(server.connection().options.endpoint.toString());
    window.findChild<QLineEdit*>(QStringLiteral("llmModelId"))
        ->setText(QStringLiteral("fixture-model"));
    window.findChild<QCheckBox*>(QStringLiteral("llmAllowHttp"))->setChecked(true);
    auto* resume = window.findChild<QPushButton*>(QStringLiteral("analysisContinueTask"));
    QVERIFY(!resume->isHidden());
    QCOMPARE(resume->text(), QStringLiteral("继续审查"));
    const auto capture = [&](const QString& suffix) {
        const auto base = qEnvironmentVariable("LOREFORGE_CONTINUATION_PREVIEW_IMAGE");
        if (base.isEmpty())
            return true;
        const auto font =
            QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/msyh.ttc"));
        const auto families = QFontDatabase::applicationFontFamilies(font);
        if (!families.isEmpty())
            window.setFont(QFont(families.first(), 10));
        window.findChild<QTabWidget*>(QStringLiteral("workspacePages"))->setCurrentIndex(2);
        window.resize(1400, 1100);
        window.show();
        QTest::qWait(30);
        return window.grab().save(base + suffix + QStringLiteral(".png"));
    };
    QVERIFY(capture(QStringLiteral("-ready")));
    QTimer::singleShot(0, &window, [&] {
        window.findChild<QMessageBox*>(QStringLiteral("analysisSendConfirmation"))->reject();
    });
    resume->click();
    QCOMPARE(server.requests.size(), 3);
    server.status = 200;
    server.makeOutput = [](const QJsonObject& user, int) {
        return reviewFor(user, QStringLiteral("redo"));
    };
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = window.findChild<QMessageBox*>(QStringLiteral("analysisSendConfirmation"));
        QVERIFY(dialog->text().contains(QStringLiteral("最多 1 次新请求")));
        dialog->findChild<QPushButton*>(QStringLiteral("analysisConfirmSend"))->click();
    });
    resume->click();
    auto* status = window.findChild<QLabel*>(QStringLiteral("analysisTaskStatus"));
    QTRY_VERIFY(status->text().contains(QStringLiteral("审查建议重做")));
    QVERIFY(status->text().contains(QStringLiteral("这项结论需再核对")));
    QVERIFY(window.findChild<QLabel*>(QStringLiteral("taskStatusText"))
                ->text()
                .contains(QStringLiteral("审查建议重做")));
    QVERIFY(resume->isHidden());
    QCOMPARE(server.requests.size(), 4);
    QCOMPARE(window.findChild<QPushButton*>(QStringLiteral("llmAnalyzeChapter"))->text(),
             QStringLiteral("综合分析（原流程）"));
    QVERIFY(capture(QStringLiteral("-review")));
}

void ChapterAnalysisPipelineTest::validatesLegacyAndCorruptPrerequisites() {
    Fixture fixture;
    QVERIFY(fixture.initialize());
    Server server;
    server.makeOutput = [&](const QJsonObject& user, int index) {
        if (index == 1)
            server.status = 500;
        return index == 0 ? analysisFor(user) : reviewFor(user);
    };
    Controller controller;
    QSignalSpy finished(&controller, &Controller::finished);
    controller.start(fixture.path, fixture.book, 0, fixture.preview, server.connection());
    QTRY_COMPARE(finished.size(), 1);
    const auto original = history(fixture)[0];
    const auto addLegacy = [&](const QString& pipeline, bool corrupt, bool oldPrompt = false) {
        auto opened = Database::open(fixture.path);
        auto database = std::get<std::unique_ptr<Database>>(std::move(opened));
        storage::InferenceRepository repository(*database);
        storage::LLMRunRepository runs(*database);
        auto content = original.context.content;
        content.remove(QStringLiteral("api_endpoint"));
        content.remove(QStringLiteral("attempt_sequence"));
        content.insert(QStringLiteral("pipeline_id"), pipeline);
        auto snapshot = inference::makeContextSnapshot(
            core::ContextSnapshotId::fromStableKey(pipeline), fixture.projectId, content,
            QDateTime::currentDateTimeUtc());
        auto run = original.run;
        run.id = core::LLMRunId::fromStableKey(pipeline);
        run.requestId = QUuid::createUuid();
        run.startedAt = QDateTime::currentDateTimeUtc();
        run.completedAt = run.startedAt;
        auto artifacts = original.artifacts;
        if (oldPrompt) {
            const auto prompt = inference::makePromptVersion(
                {artifacts.promptTemplateId, fixture.preview.prompt.prompt.name}, 1,
                QStringLiteral("旧版分析提示词，不包含固定别名规则。"),
                QDateTime::currentDateTimeUtc());
            if (repository.savePromptVersion(prompt))
                return false;
            artifacts.promptVersion = 1;
            content.insert(QStringLiteral("prompt_version"), 1);
            content.insert(QStringLiteral("prompt_sha256"), prompt.contentHash.toHex());
            auto messages = content.value(QStringLiteral("messages")).toArray();
            auto system = messages[0].toObject();
            system.insert(QStringLiteral("content"), prompt.text);
            messages[0] = system;
            content.insert(QStringLiteral("messages"), messages);
            snapshot = inference::makeContextSnapshot(snapshot.id, fixture.projectId, content,
                                                      QDateTime::currentDateTimeUtc());
            auto request = QJsonDocument::fromJson(artifacts.rawRequest).object();
            request.insert(QStringLiteral("messages"), messages);
            artifacts.rawRequest = QJsonDocument(request).toJson(QJsonDocument::Compact);
        }
        artifacts.runId = run.id;
        artifacts.contextSnapshotId = snapshot.id;
        artifacts.rawResponse.reset();
        artifacts.parsedResponse.reset();
        artifacts.validation = {inference::ValidationStatus::Pending, {}};
        auto output = original.artifacts.parsedResponse->object();
        if (corrupt)
            output.insert(QStringLiteral("chapter_id"), QStringLiteral("wrong-chapter"));
        return !repository.saveContextSnapshot(snapshot) && !runs.save(run) &&
               !repository.createRunArtifacts(artifacts) &&
               !repository.finalizeRunArtifacts(run.id, original.artifacts.rawResponse,
                                                QJsonDocument(output),
                                                {inference::ValidationStatus::Valid, {}});
    };
    QVERIFY(addLegacy(QStringLiteral("legacy-pipeline"), false));
    QVERIFY(addLegacy(QStringLiteral("corrupt-pipeline"), true));
    QVERIFY(addLegacy(QStringLiteral("v1-pipeline"), false, true));
    server.status = 200;
    server.makeOutput = {};
    controller.start(fixture.path, fixture.book, 0, fixture.preview, server.connection(),
                     QStringLiteral("legacy-pipeline"));
    QCOMPARE(finished.size(), 2);
    QVERIFY(finished.last()[0].toString().contains(QStringLiteral("接口地址")));
    QCOMPARE(server.requests.size(), 2);
    controller.start(fixture.path, fixture.book, 0, fixture.preview, server.connection(),
                     QStringLiteral("corrupt-pipeline"), true);
    QCOMPARE(finished.size(), 3);
    QCOMPARE(server.requests.size(), 2);
    controller.start(fixture.path, fixture.book, 0, fixture.preview, server.connection(),
                     QStringLiteral("legacy-pipeline"), true);
    QTRY_COMPARE(finished.size(), 4);
    QVERIFY2(finished.last()[0].toString().contains(QStringLiteral("通过")),
             qPrintable(finished.last()[0].toString()));
    QCOMPARE(server.requests.size(), 4);
    controller.start(fixture.path, fixture.book, 0, fixture.preview, server.connection(),
                     QStringLiteral("v1-pipeline"), true);
    QCOMPARE(finished.size(), 5);
    QVERIFY(finished.last()[0].toString().contains(QStringLiteral("提示词或输出契约已变化")));
    QCOMPARE(server.requests.size(), 4);
}

void ChapterAnalysisPipelineTest::persistsSafeDiagnosticsAndKnownUsageForInvalidResponses() {
    Fixture fixture;
    QVERIFY(fixture.initialize());
    Server server;
    server.responseOverride =
        R"({"choices":[{"message":{"content":"","reasoning_content":"credential-must-not-persist private thinking"},"finish_reason":"length"}],"usage":{"prompt_tokens":23,"completion_tokens":4096,"total_tokens":4119}})";
    Controller controller;
    QSignalSpy finished(&controller, &Controller::finished);
    controller.start(fixture.path, fixture.book, 0, fixture.preview, server.connection());
    QTRY_COMPARE(finished.size(), 1);
    QVERIFY(finished[0][0].toString().contains(QStringLiteral("正文为空")));
    QVERIFY(finished[0][0].toString().contains(QStringLiteral("length")));
    QCOMPARE(server.requests.size(), 1);
    auto entries = history(fixture);
    QCOMPARE(entries.size(), 1);
    const auto& entry = entries[0];
    QCOMPARE(entry.run.status, storage::LLMRunStatus::Failed);
    QCOMPARE(entry.run.totalTokens, 4119);
    QVERIFY(entry.outcome.value(QStringLiteral("usage_known")).toBool());
    const auto diagnostic = entry.outcome.value(QStringLiteral("response_diagnostics")).toObject();
    QCOMPARE(diagnostic.value(QStringLiteral("issue")).toString(), QStringLiteral("empty_content"));
    QCOMPARE(diagnostic.value(QStringLiteral("content_bytes")).toInteger(), 0);
    QCOMPARE(diagnostic.value(QStringLiteral("finish_reason")).toString(),
             QStringLiteral("length"));
    QVERIFY(!entry.artifacts.rawResponse.has_value());
    QVERIFY(!entry.artifacts.parsedResponse.has_value());
    QVERIFY(!QJsonDocument(entry.outcome).toJson().contains("credential-must-not-persist"));
    QVERIFY(!entry.run.errorMessage.contains(QStringLiteral("private thinking")));
    entries = history(fixture);
    QCOMPARE(entries[0].run.totalTokens, 4119);
}

void ChapterAnalysisPipelineTest::doesNotStoreReasoningInSuccessfulArtifacts() {
    Fixture fixture;
    QVERIFY(fixture.initialize());
    Server server;
    server.withReasoning = true;
    Controller controller;
    QSignalSpy finished(&controller, &Controller::finished);
    controller.start(fixture.path, fixture.book, 0, fixture.preview, server.connection());
    QTRY_COMPARE(finished.size(), 1);
    const auto entries = history(fixture);
    QCOMPARE(entries.size(), 3);
    for (const auto& entry : entries) {
        QCOMPARE(entry.run.status, storage::LLMRunStatus::Succeeded);
        QVERIFY(entry.artifacts.rawResponse.has_value());
        QVERIFY(!entry.artifacts.rawResponse->contains("private-thinking-must-not-persist"));
        QVERIFY(!entry.artifacts.rawResponse->contains("reasoning_content"));
    }
}

void ChapterAnalysisPipelineTest::runsThreeRolesAndPersistsHistory() {
    Fixture fixture;
    QVERIFY(fixture.initialize());
    Server server;
    Controller controller;
    QSignalSpy finished(&controller, &Controller::finished);
    auto connection = server.connection();
    QSignalSpy streams(&controller, &Controller::streamStarted);
    QSignalSpy progress(&controller, &Controller::streamProgress);
    connection.options.enableThinking = false;
    controller.start(fixture.path, fixture.book, 0, fixture.preview, connection);
    connection.options.enableThinking = true;
    QVERIFY(controller.busy());
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
    QVERIFY2(finished[0][0].toString().contains(QStringLiteral("通过")),
             qPrintable(finished[0][0].toString()));
    QCOMPARE(server.requests.size(), 3);
    QCOMPARE(streams.size(), 3);
    QVERIFY(progress.size() >= 3);
    const auto entries = history(fixture);
    QCOMPARE(entries.size(), 3);
    for (qsizetype index = 0; index < entries.size(); ++index) {
        QCOMPARE(entries[index].run.status, storage::LLMRunStatus::Succeeded);
        QCOMPARE(entries[index].artifacts.rawRequest, server.rawRequests[index]);
        QCOMPARE(entries[index].run.totalTokens, 14);
        QCOMPARE(entries[index].context.content.value(QStringLiteral("thinking_mode")).toString(),
                 QStringLiteral("disabled"));
        QVERIFY(server.requests[index].value(QStringLiteral("enable_thinking")).isBool());
        QVERIFY(!server.requests[index].value(QStringLiteral("enable_thinking")).toBool());
        QVERIFY(entries[index].outcome.value(QStringLiteral("usage_known")).toBool());
        QCOMPARE(entries[index].outcome.value(QStringLiteral("response_transport")).toString(),
                 QStringLiteral("sse"));
        QCOMPARE(entries[index].outcome.value(QStringLiteral("response_record_format")).toString(),
                 QStringLiteral("assembled-chat-completion-json"));
        QCOMPARE(entries[index].context.content.value(QStringLiteral("role_index")).toInt(),
                 static_cast<int>(index));
        QCOMPARE(server.requests[index].value(QStringLiteral("model")).toString(),
                 QStringLiteral("fixture-model"));
        QVERIFY(server.requests[index].contains(QStringLiteral("max_tokens")));
        QVERIFY(server.requests[index].value(QStringLiteral("stream")).toBool());
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
            range.insert(QStringLiteral("block_id"), QStringLiteral("missing-block"));
        }
        if (mode == 1) {
            range.insert(QStringLiteral("source_start"), 1);
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
    if (mode == 3) {
        QCOMPARE(entries[0].outcome.value(QStringLiteral("response_transport")).toString(),
                 QStringLiteral("sse"));
        QCOMPARE(entries[0].outcome.value(QStringLiteral("response_record_format")).toString(),
                 QStringLiteral("none"));
    }
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
    QVERIFY(finished[0][0].toString().contains(QStringLiteral("待人工确认")));
    QVERIFY(finished[0][0].toString().contains(QStringLiteral("这项结论需再核对")));
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
        window.findChild<QPushButton*>(QStringLiteral("llmAnalyzeChapter"))->click();
        QCOMPARE(window.findChild<QTabWidget*>(QStringLiteral("workspacePages"))->currentIndex(),
                 1);
        QVERIFY(!window.findChild<QMessageBox*>(QStringLiteral("analysisSendConfirmation")));
        QCOMPARE(server.requests.size(), 0);
        auto* llm = window.findChild<QWidget*>(QStringLiteral("llmWorkbench"));
        llm->findChild<QLineEdit*>(QStringLiteral("llmEndpoint"))
            ->setText(server.connection().options.endpoint.toString());
        llm->findChild<QLineEdit*>(QStringLiteral("llmModelId"))
            ->setText(QStringLiteral("fixture-model"));
        llm->findChild<QLineEdit*>(QStringLiteral("llmApiKey"))
            ->setText(QStringLiteral("temporary-pipeline-key"));
        window.findChild<QSpinBox*>(QStringLiteral("llmChapterContextLimit"))->setValue(65536);
        window.findChild<QSpinBox*>(QStringLiteral("llmChapterOutputBudget"))->setValue(8192);
        llm->findChild<QCheckBox*>(QStringLiteral("llmAllowHttp"))->setChecked(true);
        llm->findChild<QComboBox*>(QStringLiteral("llmResponseFormat"))
            ->setCurrentText(QStringLiteral("json_object"));
        auto* prepare = window.findChild<QPushButton*>(QStringLiteral("llmPrepareChapterPreview"));
        auto* analyze = window.findChild<QPushButton*>(QStringLiteral("llmAnalyzeChapter"));
        QVERIFY(analyze->isEnabled());
        QVERIFY(prepare->isEnabled());
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
        QTRY_VERIFY_WITH_TIMEOUT(status->text().startsWith(QStringLiteral("通过（")), 5000);
        QVERIFY(!window.findChild<QLabel*>(QStringLiteral("llmChapterPreviewStatus"))
                     ->text()
                     .contains(QStringLiteral("准备失败")));
        QCOMPARE(server.requests.size(), 3);
        QCOMPARE(window.findChild<QListWidget*>(QStringLiteral("analysisHistory"))->count(), 3);
        auto* highlight = window.findChild<app::CharacterHighlightText*>();
        QVERIFY(highlight);
        QVERIFY(!highlight->spans().isEmpty());
        QVERIFY(window.findChild<QLabel*>(QStringLiteral("highlightPreviewStatus"))
                    ->text()
                    .contains(QStringLiteral("三角色流程通过")));
        auto* workspacePages = window.findChild<QTabWidget*>(QStringLiteral("workspacePages"));
        QCOMPARE(workspacePages->count(), 4);
        QCOMPARE(workspacePages->tabText(3), QStringLiteral("分割预览"));
        QVERIFY(window.findChild<QLabel*>(QStringLiteral("analysisTokenTotals"))
                    ->text()
                    .contains(QStringLiteral("42 token")));
        auto* selector = window.findChild<QComboBox*>(QStringLiteral("analysisChapterSelector"));
        selector->setCurrentIndex(1);
        QVERIFY(highlight->spans().isEmpty());
        QCOMPARE(window.findChild<QListWidget*>(QStringLiteral("analysisHistory"))->count(), 0);
        QVERIFY(analyze->isEnabled());
        selector->setCurrentIndex(0);
        QVERIFY(!highlight->spans().isEmpty());
        QVERIFY(analyze->isEnabled());
        if (const auto path = qEnvironmentVariable("LOREFORGE_SPLIT_PREVIEW_IMAGE");
            !path.isEmpty()) {
            const auto font =
                QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/msyh.ttc"));
            const auto families = QFontDatabase::applicationFontFamilies(font);
            if (!families.isEmpty())
                window.setFont(QFont(families.first(), 10));
            workspacePages->setCurrentIndex(3);
            window.resize(1400, 1000);
            window.show();
            QTest::qWait(30);
            QVERIFY(window.grab().save(path));
        }
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
    QCOMPARE(reopened.findChild<QLineEdit*>(QStringLiteral("llmApiKey"))->text(),
             QStringLiteral("temporary-pipeline-key"));
    QCOMPARE(reopened.findChild<QSpinBox*>(QStringLiteral("llmChapterContextLimit"))->value(),
             65536);
    QCOMPARE(reopened.findChild<QSpinBox*>(QStringLiteral("llmChapterOutputBudget"))->value(),
             8192);
    QCOMPARE(reopened.findChild<QListWidget*>(QStringLiteral("analysisHistory"))->count(), 3);
    QVERIFY(reopened.findChild<QLabel*>(QStringLiteral("analysisTaskStatus"))
                ->text()
                .contains(QStringLiteral("通过")));
    QVERIFY(reopened.findChild<QPushButton*>(QStringLiteral("llmAnalyzeChapter"))->isEnabled());
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
    QVERIFY(
        fixture.initialize(QByteArray("\xef\xbb\xbf") +
                           QStringLiteral("# 重复😀\r\n\r\n重复。\r\n\r\n重复。\r\n").toUtf8()));
    Server server;
    server.makeOutput = [](const QJsonObject& user, int index) {
        if (index > 0) {
            return reviewFor(user);
        }
        auto output = analysisFor(user);
        auto summary = output.value(QStringLiteral("summary")).toObject();
        const auto range = user.value(QStringLiteral("source_blocks")).toArray().at(2).toObject();
        summary.insert(QStringLiteral("evidence"),
                       QJsonArray{QJsonObject{
                           {QStringLiteral("block_id"), range.value(QStringLiteral("block_id"))}}});
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
