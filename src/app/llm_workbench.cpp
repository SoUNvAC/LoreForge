#include "llm_workbench.h"
#include "loreforge/storage/project_database.h"
#include "loreforge/storage/project_repository.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

namespace loreforge::app {
namespace {
bool privateHost(const QUrl& endpoint) {
    if (endpoint.host().compare(QStringLiteral("localhost"), Qt::CaseInsensitive) == 0) {
        return true;
    }
    const QHostAddress address(endpoint.host());
    return address.isLoopback() ||
           address.isInSubnet(QHostAddress(QStringLiteral("10.0.0.0")), 8) ||
           address.isInSubnet(QHostAddress(QStringLiteral("172.16.0.0")), 12) ||
           address.isInSubnet(QHostAddress(QStringLiteral("192.168.0.0")), 16) ||
           address.isInSubnet(QHostAddress(QStringLiteral("fc00::")), 7);
}
} // namespace

LlmWorkbench::LlmWorkbench(QWidget* parent, QString settingsFile) : QWidget(parent) {
    setObjectName(QStringLiteral("llmWorkbench"));
    // Keep source compatibility for callers; global/INI API profiles are no longer used.
    static_cast<void>(settingsFile);
    saveTimer_ = new QTimer(this);
    saveTimer_->setSingleShot(true);
    saveTimer_->setInterval(500);
    connect(saveTimer_, &QTimer::timeout, this, &LlmWorkbench::saveConfiguration);
    auto* layout = new QVBoxLayout(this);
    auto* note = new QLabel(tr("一个模型，三个角色：分析员 → 复核员 → 审查员，默认串行。\n"
                               "本页管理连接和运行统计；小说业务请进入“小说分析”页。"),
                            this);
    note->setWordWrap(true);
    layout->addWidget(note);
    auto* configuration = new QGroupBox(tr("API 配置（未打开工程）"), this);
    configuration->setObjectName(QStringLiteral("llmProjectApiGroup"));
    auto* form = new QFormLayout(configuration);
    endpoint_ = new QLineEdit(configuration);
    endpoint_->setObjectName(QStringLiteral("llmEndpoint"));
    endpoint_->setPlaceholderText(QStringLiteral("http://192.168.x.x:port/v1/chat/completions"));
    modelId_ = new QLineEdit(configuration);
    modelId_->setObjectName(QStringLiteral("llmModelId"));
    modelId_->setPlaceholderText(tr("服务端实际 model ID，不自动推断名称"));
    apiKey_ = new QLineEdit(configuration);
    apiKey_->setObjectName(QStringLiteral("llmApiKey"));
    apiKey_->setEchoMode(QLineEdit::Password);
    apiKey_->setPlaceholderText(tr("仅本次会话；局域网免鉴权服务可留空"));
    allowHttp_ =
        new QCheckBox(tr("允许局域网明文 HTTP（不加密，可能暴露密钥与正文）"), configuration);
    allowHttp_->setObjectName(QStringLiteral("llmAllowHttp"));
    form->addRow(tr("完整 API 地址"), endpoint_);
    form->addRow(tr("模型 ID"), modelId_);
    form->addRow(tr("API Key"), apiKey_);
    form->addRow(allowHttp_);
    layout->addWidget(configuration);

    auto* advanced = new QGroupBox(tr("高级选项（展开）"), this);
    advanced->setCheckable(true);
    advanced->setChecked(false);
    auto* advancedBody = new QWidget(advanced);
    auto* advancedLayout = new QVBoxLayout(advanced);
    advancedLayout->addWidget(advancedBody);
    auto* options = new QFormLayout(advancedBody);
    format_ = new QComboBox(advancedBody);
    format_->setObjectName(QStringLiteral("llmResponseFormat"));
    format_->addItems(
        {QStringLiteral("text"), QStringLiteral("json_object"), QStringLiteral("json_schema")});
    tokenField_ = new QComboBox(advancedBody);
    tokenField_->setObjectName(QStringLiteral("llmTokenField"));
    tokenField_->addItems({QStringLiteral("max_completion_tokens"), QStringLiteral("max_tokens")});
    thinkingMode_ = new QComboBox(advancedBody);
    thinkingMode_->setObjectName(QStringLiteral("llmThinkingMode"));
    thinkingMode_->addItem(tr("跟随服务端（不传参数）"), -1);
    thinkingMode_->addItem(tr("关闭思考（enable_thinking=false）"), 0);
    thinkingMode_->addItem(tr("开启思考（enable_thinking=true）"), 1);
    thinkingMode_->setToolTip(tr("仅适用于支持 enable_thinking "
                                 "的服务。影响连接测试及全部分析角色；不支持时请选择跟随服务端。"));
    timeout_ = new QSpinBox(advancedBody);
    timeout_->setRange(5, 600);
    timeout_->setSuffix(tr(" 秒"));
    outputBudget_ = new QSpinBox(advancedBody);
    outputBudget_->setRange(32, 8192);
    options->addRow(tr("响应格式（测试与分析共用）"), format_);
    outputBudget_->setToolTip(
        tr("仅用于内置短样本；小说任务的每角色预算在小说分析页的“分析参数”中设置。"));
    options->addRow(tr("输出限制字段"), tokenField_);
    options->addRow(tr("思考模式"), thinkingMode_);
    options->addRow(tr("单次超时"), timeout_);
    options->addRow(tr("测试输出 token 上限"), outputBudget_);
    advancedBody->hide();
    connect(advanced, &QGroupBox::toggled, advancedBody, &QWidget::setVisible);
    layout->addWidget(advanced);
    auto* buttons = new QHBoxLayout;
    save_ = new QPushButton(tr("保存到工程（含临时密钥）"), this);
    save_->setEnabled(false);
    save_->setObjectName(QStringLiteral("llmSaveConfiguration"));
    test_ = new QPushButton(tr("测试连接（内置短样本）"), this);
    test_->setObjectName(QStringLiteral("llmTestConnection"));
    cancel_ = new QPushButton(tr("取消测试"), this);
    cancel_->setObjectName(QStringLiteral("llmCancelTest"));
    cancel_->setEnabled(false);
    buttons->addWidget(save_);
    clearKey_ = new QPushButton(tr("清除工程中的 API Key"), this);
    clearKey_->setObjectName(QStringLiteral("llmClearSavedKey"));
    clearKey_->setEnabled(false);
    buttons->addWidget(clearKey_);
    connect(clearKey_, &QPushButton::clicked, this, [this] {
        apiKey_->clear();
        saveConfiguration();
    });
    buttons->addWidget(test_);
    buttons->addWidget(cancel_);
    buttons->addStretch();
    layout->addLayout(buttons);
    connection_ = new QLabel(tr("未测试；不会自动连接"), this);
    connection_->setObjectName(QStringLiteral("llmConnectionStatus"));
    connection_->setWordWrap(true);
    connection_->setTextFormat(Qt::PlainText);
    layout->addWidget(connection_);
    metrics_ = new QLabel(
        tr("精确生成速度：未提供；SSE 接收状态见底部，耗时与 token 用量在响应后显示"), this);
    metrics_->setObjectName(QStringLiteral("llmTestMetrics"));
    metrics_->setWordWrap(true);
    layout->addWidget(metrics_);
    totals_ = new QLabel(tr("连接测试累计：尚无响应；小说任务统计独立显示。"), this);
    totals_->setObjectName(QStringLiteral("llmTestTotals"));
    totals_->setWordWrap(true);
    layout->addWidget(totals_);
    resetTokens_ = new QPushButton(tr("重置token计数"), this);
    resetTokens_->setObjectName(QStringLiteral("llmResetTokenCounts"));
    resetTokens_->setToolTip(tr("仅重置本会话连接测试累计，不修改项目、连接配置或上次响应。"));
    layout->addWidget(resetTokens_, 0, Qt::AlignLeft);
    analysisMetrics_ = new QLabel(tr("小说任务统计：尚未选择项目。"), this);
    analysisMetrics_->setObjectName(QStringLiteral("llmAnalysisMetrics"));
    analysisMetrics_->setWordWrap(true);
    analysisMetrics_->setTextFormat(Qt::PlainText);
    layout->addWidget(analysisMetrics_);
    log_ = new QPlainTextEdit(this);
    log_->setObjectName(QStringLiteral("llmConnectionLog"));
    log_->setReadOnly(true);
    log_->setMaximumBlockCount(100);
    log_->setPlaceholderText(tr("脱敏连接记录；不记录密钥、请求正文或原始响应"));
    layout->addWidget(log_, 1);
    format_->setCurrentText(QStringLiteral("json_object"));
    tokenField_->setCurrentText(QStringLiteral("max_tokens"));
    timeout_->setValue(600);
    outputBudget_->setValue(128);
    layout->addWidget(
        new QLabel(tr("配置修改后自动保存到当前工程，包括明文临时密钥。分享工程前请清除并撤销密钥。"
                      "\n未打开工程时仅供本会话测试，不保存；打开工程不会自动发送请求。"),
                   this));
    connect(save_, &QPushButton::clicked, this, &LlmWorkbench::saveConfiguration);
    connect(test_, &QPushButton::clicked, this, &LlmWorkbench::testConnection);
    connect(cancel_, &QPushButton::clicked, this, &LlmWorkbench::cancelTest);
    connect(resetTokens_, &QPushButton::clicked, this, &LlmWorkbench::resetTokenCounts);
    const auto changed = [this] {
        if (loading_)
            return;
        dirty_ = true;
        if (!projectPath_.isEmpty())
            saveTimer_->start();
        connection_->setText(projectPath_.isEmpty()
                                 ? tr("仅本会话配置；打开工程后可保存。")
                                 : tr("配置已修改，正在自动保存到工程；测试可选。"));
        emit configurationChanged();
    };
    connect(endpoint_, &QLineEdit::textChanged, this, changed);
    connect(modelId_, &QLineEdit::textChanged, this, changed);
    connect(apiKey_, &QLineEdit::textChanged, this, changed);
    connect(format_, &QComboBox::currentTextChanged, this, changed);
    connect(tokenField_, &QComboBox::currentTextChanged, this, changed);
    connect(thinkingMode_, &QComboBox::currentIndexChanged, this, changed);
    connect(allowHttp_, &QCheckBox::toggled, this, changed);
    connect(timeout_, &QSpinBox::valueChanged, this, changed);
    connect(outputBudget_, &QSpinBox::valueChanged, this, changed);
}

LlmWorkbench::~LlmWorkbench() {
    if (dirty_ && !projectPath_.isEmpty())
        saveConfiguration();
    closing_ = true;
    client_.reset();
}

std::variant<AnalysisConnection, QString> LlmWorkbench::analysisConnection() const {
    if (!requestId_.isNull() || analysisBusy_) {
        return tr("已有连接测试或分析任务，请等待完成。");
    }
    if (const auto error = configurationError(); !error.isEmpty()) {
        return error;
    }
    if (format_->currentText() == QStringLiteral("text")) {
        return tr("请在 LLM 页选择已验证的 json_object 或 json_schema。");
    }
    const QUrl endpoint(endpoint_->text().trimmed(), QUrl::StrictMode);
    return AnalysisConnection{{endpoint, apiKey_->text().trimmed().toUtf8(),
                               modelId_->text().trimmed(), privateHost(endpoint),
                               tokenField_->currentText() == QStringLiteral("max_tokens")
                                   ? llm::CompletionTokenParameter::MaxTokens
                                   : llm::CompletionTokenParameter::MaxCompletionTokens,
                               enableThinking()},
                              format_->currentText(),
                              timeout_->value() * 1000};
}

void LlmWorkbench::setAnalysisBusy(bool busy) {
    if (busy && dirty_)
        saveConfiguration();
    analysisBusy_ = busy;
    setBusy(!requestId_.isNull());
}

std::optional<bool> LlmWorkbench::enableThinking() const {
    const auto mode = thinkingMode_->currentData().toInt();
    if (mode < 0)
        return std::nullopt;
    return mode == 1;
}

bool LlmWorkbench::setProject(QString path, core::ProjectId id) {
    if (path == projectPath_ && id == projectId_)
        return true;
    if (!flushConfiguration())
        return false;
    const bool adoptSession = projectPath_.isEmpty() && dirty_ && !path.isEmpty();
    saveTimer_->stop();
    client_.reset();
    requestId_ = {};
    projectPath_ = std::move(path);
    projectId_ = id;
    auto* group = findChild<QGroupBox*>(QStringLiteral("llmProjectApiGroup"));
    group->setTitle(projectPath_.isEmpty()
                        ? tr("API 配置（未打开工程）")
                        : tr("工程 API 配置 · %1").arg(QFileInfo(projectPath_).fileName()));
    group->setToolTip(projectPath_);
    QJsonObject value;
    bool loaded = true;
    if (!projectPath_.isEmpty()) {
        auto opened = storage::ProjectDatabase::open(projectPath_);
        if (auto* database = std::get_if<std::unique_ptr<storage::ProjectDatabase>>(&opened)) {
            storage::ProjectRepository repository(**database);
            auto settings = repository.apiSettings(id);
            if (const auto* object = std::get_if<QJsonObject>(&settings))
                value = *object;
            else
                loaded = false;
        } else
            loaded = false;
    }
    if (adoptSession && loaded && value.isEmpty()) {
        // Preserve parameters typed before the first project was opened; never
        // replace an existing project's profile or copy between projects.
        saveConfiguration();
        setBusy(false);
        emit configurationChanged();
        emit analysisBudgetsLoaded(analysisMaximum_, analysisReserved_);
        return !dirty_;
    }
    loading_ = true;
    endpoint_->setText(value.value(QStringLiteral("endpoint")).toString());
    modelId_->setText(value.value(QStringLiteral("model_id")).toString());
    apiKey_->setText(value.value(QStringLiteral("api_key")).toString());
    allowHttp_->setChecked(value.value(QStringLiteral("allow_http")).toBool());
    format_->setCurrentIndex(
        qMax(0, format_->findText(value.value(QStringLiteral("response_format"))
                                      .toString(QStringLiteral("json_object")))));
    tokenField_->setCurrentIndex(qMax(
        0, tokenField_->findText(
               value.value(QStringLiteral("token_field")).toString(QStringLiteral("max_tokens")))));
    thinkingMode_->setCurrentIndex(
        qMax(0, thinkingMode_->findData(value.value(QStringLiteral("thinking_mode")).toInt(-1))));
    timeout_->setValue(value.value(QStringLiteral("timeout_seconds")).toInt(600));
    outputBudget_->setValue(value.value(QStringLiteral("test_output_tokens")).toInt(128));
    analysisMaximum_ =
        qBound(1024, value.value(QStringLiteral("analysis_context_tokens")).toInt(32768), 1048576);
    analysisReserved_ =
        qBound(256, value.value(QStringLiteral("analysis_output_tokens")).toInt(4096), 65536);
    loading_ = false;
    dirty_ = false;
    setBusy(false);
    emit configurationChanged();
    emit analysisBudgetsLoaded(analysisMaximum_, analysisReserved_);
    connection_->setText(
        !loaded ? tr("工程 API 配置读取失败；已清空连接信息，未发送。")
                : (projectPath_.isEmpty()
                       ? tr("未打开工程；仅本会话配置。")
                       : (value.isEmpty() ? tr("本工程尚无 API 配置；填写后自动保存。")
                                          : tr("已从工程恢复 API 配置；未发送，测试可选。"))));
    return loaded;
}

bool LlmWorkbench::flushConfiguration() {
    if (dirty_ && !projectPath_.isEmpty())
        saveConfiguration();
    return projectPath_.isEmpty() || !dirty_;
}

void LlmWorkbench::setAnalysisBudgets(int maximum, int reserved) {
    analysisMaximum_ = maximum;
    analysisReserved_ = reserved;
    dirty_ = true;
    if (!projectPath_.isEmpty())
        saveTimer_->start();
}

void LlmWorkbench::showAnalysisMetrics(QString message) {
    analysisMetrics_->setText(std::move(message));
}

QString LlmWorkbench::configurationError() const {
    const QUrl endpoint(endpoint_->text().trimmed(), QUrl::StrictMode);
    if (!endpoint.isValid() || endpoint.host().isEmpty() || endpoint.path().isEmpty() ||
        (endpoint.scheme() != QStringLiteral("http") &&
         endpoint.scheme() != QStringLiteral("https")) ||
        !endpoint.userInfo().isEmpty() || endpoint.hasQuery() || endpoint.hasFragment()) {
        return tr("请输入完整 http/https API 地址，不含用户名、查询参数或片段。");
    }
    if (endpoint.scheme() == QStringLiteral("http") &&
        (!privateHost(endpoint) || !allowHttp_->isChecked())) {
        return tr("HTTP 仅允许明确授权的局域网 IP/localhost；公网必须使用 HTTPS。");
    }
    if (modelId_->text().trimmed().isEmpty()) {
        return tr("请填写服务端实际模型 ID。");
    }
    if (apiKey_->text().contains(QLatin1Char('\r')) ||
        apiKey_->text().contains(QLatin1Char('\n'))) {
        return tr("密钥不能包含换行。");
    }
    if (apiKey_->text().trimmed().isEmpty() && !privateHost(endpoint)) {
        return tr("公网服务必须提供 API Key；免鉴权只允许局域网 IP/localhost。");
    }
    return {};
}

void LlmWorkbench::saveConfiguration() {
    saveTimer_->stop();
    if (projectPath_.isEmpty() || !projectId_.isValid())
        return;
    const QJsonObject settings{
        {QStringLiteral("endpoint"), endpoint_->text().trimmed()},
        {QStringLiteral("model_id"), modelId_->text().trimmed()},
        {QStringLiteral("api_key"), apiKey_->text().trimmed()},
        {QStringLiteral("allow_http"), allowHttp_->isChecked()},
        {QStringLiteral("response_format"), format_->currentText()},
        {QStringLiteral("token_field"), tokenField_->currentText()},
        {QStringLiteral("thinking_mode"), thinkingMode_->currentData().toInt()},
        {QStringLiteral("timeout_seconds"), timeout_->value()},
        {QStringLiteral("test_output_tokens"), outputBudget_->value()},
        {QStringLiteral("analysis_context_tokens"), analysisMaximum_},
        {QStringLiteral("analysis_output_tokens"), analysisReserved_}};
    auto opened = storage::ProjectDatabase::open(projectPath_);
    bool saved = false;
    if (auto* database = std::get_if<std::unique_ptr<storage::ProjectDatabase>>(&opened)) {
        storage::ProjectRepository repository(**database);
        saved = !repository.saveApiSettings(projectId_, settings).has_value();
    }
    dirty_ = !saved;
    connection_->setText(saved ? tr("已自动保存到当前工程（含明文临时密钥）；测试可选。")
                               : tr("工程 API 配置保存失败；当前修改仅在内存中。"));
}

void LlmWorkbench::setBusy(bool busy) {
    for (auto* widget :
         QList<QWidget*>{endpoint_, modelId_, apiKey_, allowHttp_, format_, tokenField_,
                         thinkingMode_, timeout_, outputBudget_, save_, test_, resetTokens_}) {
        widget->setEnabled(!busy && !analysisBusy_);
    }
    save_->setEnabled(!busy && !analysisBusy_ && !projectPath_.isEmpty());
    clearKey_->setEnabled(!busy && !analysisBusy_ && !projectPath_.isEmpty());
    cancel_->setEnabled(busy);
}

void LlmWorkbench::testConnection() {
    if (!requestId_.isNull() || analysisBusy_) {
        return;
    }
    if (!flushConfiguration())
        return;
    if (const auto error = configurationError(); !error.isEmpty()) {
        connection_->setText(error);
        const auto id = QUuid::createUuid();
        emit taskStatus(id, TaskState::Local, tr("检查连接配置"));
        emit taskStatus(id, TaskState::Failed, error);
        return;
    }
    const QUrl endpoint(endpoint_->text().trimmed(), QUrl::StrictMode);
    streamSilence_.stop();
    streamSilence_.setSingleShot(true);
    streamSilence_.setInterval(5000);
    disconnect(&streamSilence_, nullptr, this, nullptr);
    connect(&streamSilence_, &QTimer::timeout, this, [this] {
        if (!requestId_.isNull())
            emit taskStatus(requestId_, TaskState::Waiting,
                            tr("连接测试 · 等待后续 SSE 数据（静默 5 秒）"));
    });
    client_ = std::make_unique<llm::QwenClient>(
        llm::QwenClientOptions{endpoint, apiKey_->text().trimmed().toUtf8(),
                               modelId_->text().trimmed(), privateHost(endpoint),
                               tokenField_->currentText() == QStringLiteral("max_tokens")
                                   ? llm::CompletionTokenParameter::MaxTokens
                                   : llm::CompletionTokenParameter::MaxCompletionTokens,
                               enableThinking()});
    connect(client_.get(), &llm::QwenClient::streamStarted, this, [this](QUuid id) {
        if (id == requestId_)
            emit streamStarted(id, tr("连接测试（内置样本）"));
    });
    connect(client_.get(), &llm::QwenClient::streamProgress, this,
            [this](QUuid id, QString text, bool generating) {
                if (id != requestId_)
                    return;
                streamSilence_.start();
                emit streamProgress(id, std::move(text), generating);
                emit taskStatus(id, TaskState::Waiting,
                                generating ? tr("[分析中] 连接测试 · SSE 接收中")
                                           : tr("连接测试 · SSE 连接活跃"));
            });
    connect(client_.get(), &llm::QwenClient::streamFinished, this, &LlmWorkbench::streamFinished);
    llm::LLMRequest request;
    request.messages = {
        {llm::LLMRole::System, QStringLiteral("This is a synthetic connection test. Return only "
                                              "{\"ok\":true}. Do not add reasoning text.")},
        {llm::LLMRole::User, QStringLiteral("Return the JSON object {\"ok\":true}.")}};
    request.timeoutMs = timeout_->value() * 1000;
    request.maxCompletionTokens = outputBudget_->value();
    request.retryPolicy.maxRetries = 0;
    request.stream = true;
    const auto format = format_->currentText();
    if (format != QStringLiteral("text")) {
        request.responseFormat = {{QStringLiteral("type"), format}};
        if (format == QStringLiteral("json_schema")) {
            const QJsonObject schema{
                {QStringLiteral("type"), QStringLiteral("object")},
                {QStringLiteral("properties"),
                 QJsonObject{{QStringLiteral("ok"),
                              QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}}}},
                {QStringLiteral("required"), QJsonArray{QStringLiteral("ok")}},
                {QStringLiteral("additionalProperties"), false}};
            request.responseFormat.insert(
                QStringLiteral("json_schema"),
                QJsonObject{{QStringLiteral("name"), QStringLiteral("connection_probe")},
                            {QStringLiteral("strict"), true},
                            {QStringLiteral("schema"), schema}});
        }
    }
    setBusy(true);
    connection_->setText(tr("请求中：仅发送内置短样本；不自动重试。"));
    log_->appendPlainText(tr("开始连接测试（不含小说、无自动重试）。"));
    const QPointer<LlmWorkbench> guard(this);
    requestId_ = client_->enqueue(std::move(request), [guard](const QUuid&, llm::LLMResult result) {
        if (guard && !guard->closing_) {
            guard->finishTest(std::move(result));
        }
    });
    emit taskStatus(requestId_, TaskState::Waiting, tr("连接测试 · 等待模型响应"));
}

void LlmWorkbench::cancelTest() {
    if (client_ && !requestId_.isNull()) {
        static_cast<void>(client_->cancel(requestId_));
    }
}

void LlmWorkbench::resetTokenCounts() {
    if (!requestId_.isNull()) {
        return;
    }
    QMessageBox dialog(QMessageBox::Question, tr("重置token计数"),
                       tr("确认清零本会话连接测试的累计输入、输出 token 和用量未知次数？\n"
                          "不会修改项目、连接配置或上次响应，也不会重置服务端统计。"),
                       QMessageBox::NoButton, this);
    dialog.setObjectName(QStringLiteral("llmResetTokenConfirmation"));
    auto* confirm = dialog.addButton(tr("确认"), QMessageBox::AcceptRole);
    confirm->setObjectName(QStringLiteral("llmConfirmTokenReset"));
    auto* cancel = dialog.addButton(tr("取消"), QMessageBox::RejectRole);
    cancel->setObjectName(QStringLiteral("llmCancelTokenReset"));
    dialog.setDefaultButton(cancel);
    dialog.setEscapeButton(cancel);
    dialog.exec();
    if (dialog.clickedButton() != confirm) {
        return;
    }
    inputTokens_ = 0;
    outputTokens_ = 0;
    missingUsage_ = 0;
    updateTokenTotals();
    log_->appendPlainText(tr("本会话连接测试 token 累计已重置；上次响应与连接配置保留。"));
}

void LlmWorkbench::finishTest(llm::LLMResult result) {
    streamSilence_.stop();
    const auto completedId = requestId_;
    requestId_ = {};
    setBusy(false);
    if (const auto* error = std::get_if<llm::LLMError>(&result)) {
        auto message = error->code == llm::LLMErrorCode::Cancelled
                           ? tr("已取消；服务端可能仍在计算。")
                           : tr("测试失败：%1，HTTP %2。检查地址、鉴权、参数和服务状态。")
                                 .arg(llm::errorCodeName(error->code))
                                 .arg(error->httpStatus);
        const auto diagnostic = llm::responseDiagnosticSummary(error->diagnostics);
        if (!diagnostic.isEmpty())
            message += QLatin1Char('\n') + diagnostic;
        connection_->setText(message);
        log_->appendPlainText(message);
        if (error->diagnostics.usageReported) {
            const auto& usage = error->diagnostics.usage;
            inputTokens_ += usage.promptTokens;
            outputTokens_ += usage.completionTokens;
            metrics_->setText(
                tr("响应校验失败；耗时 %1 ms · 输入 %2 · 输出 %3 · 总计 %4 token（服务报告）。")
                    .arg(error->latencyMs)
                    .arg(usage.promptTokens)
                    .arg(usage.completionTokens)
                    .arg(usage.totalTokens));
        } else {
            metrics_->setText(tr("本次失败/取消；耗时 %1 ms；token 用量未知，可能已产生消耗。")
                                  .arg(error->latencyMs));
            ++missingUsage_;
        }
        emit taskStatus(completedId,
                        error->code == llm::LLMErrorCode::Cancelled ? TaskState::Cancelled
                                                                    : TaskState::Failed,
                        message);
    } else {
        const auto& response = std::get<llm::LLMResponse>(result);
        const auto format = format_->currentText();
        bool accepted = !response.content.trimmed().isEmpty() &&
                        response.finishReason == QStringLiteral("stop");
        if (format != QStringLiteral("text")) {
            const auto object = response.structuredContent.toObject();
            accepted = accepted && object.size() == 1 &&
                       object.value(QStringLiteral("ok")).isBool() &&
                       object.value(QStringLiteral("ok")).toBool();
        }
        connection_->setText(accepted
                                 ? (format == QStringLiteral("text")
                                        ? tr("基础连接测试通过；尚未验证结构化分析能力。")
                                        : tr("JSON 短样本测试通过；不代表章节证据分析已通过。"))
                                 : tr("服务已响应，但短样本/结束标志校验失败，不能标记通过。"));
        if (response.usageReported) {
            inputTokens_ += response.usage.promptTokens;
            outputTokens_ += response.usage.completionTokens;
            metrics_->setText(tr("耗时 %1 ms · 输入 %2 · 输出 %3 · 总计 %4 token\n"
                                 "精确生成速度：未提供。端到端输出吞吐 %5 "
                                 "token/s（含排队与等待，非解码速度）。")
                                  .arg(response.latencyMs)
                                  .arg(response.usage.promptTokens)
                                  .arg(response.usage.completionTokens)
                                  .arg(response.usage.totalTokens)
                                  .arg(response.latencyMs > 0
                                           ? QString::number(response.usage.completionTokens *
                                                                 1000.0 / response.latencyMs,
                                                             'f', 2)
                                           : tr("未提供")));
        } else {
            ++missingUsage_;
            metrics_->setText(
                tr("耗时 %1 ms · Token：服务未提供；生成速度：未提供。").arg(response.latencyMs));
        }
        log_->appendPlainText(connection_->text());
        emit taskStatus(completedId, accepted ? TaskState::Success : TaskState::Failed,
                        connection_->text());
    }
    updateTokenTotals();
}

void LlmWorkbench::updateTokenTotals() {
    totals_->setText(
        tr("本会话连接测试累计已知：输入 %1 · 输出 %2 · 总计 %3 token；%4 次用量未知。\n"
           "此计数仅含连接测试；小说任务统计独立显示。")
            .arg(inputTokens_)
            .arg(outputTokens_)
            .arg(inputTokens_ + outputTokens_)
            .arg(missingUsage_));
}

} // namespace loreforge::app
