#include "llm_workbench.h"

#include <QCheckBox>
#include <QComboBox>
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
#include <QSettings>
#include <QSpinBox>
#include <QTableWidget>
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
    settings_ =
        settingsFile.isEmpty()
            ? std::make_unique<QSettings>(QStringLiteral("LoreForge"), QStringLiteral("LoreForge"))
            : std::make_unique<QSettings>(settingsFile, QSettings::IniFormat);
    auto* layout = new QVBoxLayout(this);
    auto* note = new QLabel(tr("一个模型，三个角色：分析员 → 复核员 → 审查员，默认串行。\n"
                               "本阶段只测试连接；不会发送小说。章节分析将在后续阶段接通。"),
                            this);
    note->setWordWrap(true);
    layout->addWidget(note);
    auto* configuration = new QGroupBox(tr("模型连接配置"), this);
    auto* form = new QFormLayout(configuration);
    endpoint_ = new QLineEdit(configuration);
    endpoint_->setObjectName(QStringLiteral("llmEndpoint"));
    endpoint_->setPlaceholderText(QStringLiteral("http://192.168.x.x:port/v1/chat/completions"));
    modelSize_ = new QComboBox(configuration);
    modelSize_->setObjectName(QStringLiteral("llmModelSize"));
    modelSize_->addItems({QStringLiteral("14B"), QStringLiteral("30B")});
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
    form->addRow(tr("模型规模"), modelSize_);
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
    timeout_ = new QSpinBox(advancedBody);
    timeout_->setRange(5, 600);
    timeout_->setSuffix(tr(" 秒"));
    outputBudget_ = new QSpinBox(advancedBody);
    outputBudget_->setRange(32, 8192);
    options->addRow(tr("测试输出模式"), format_);
    options->addRow(tr("输出限制字段"), tokenField_);
    options->addRow(tr("单次超时"), timeout_);
    options->addRow(tr("测试输出 token 上限"), outputBudget_);
    advancedBody->hide();
    connect(advanced, &QGroupBox::toggled, advancedBody, &QWidget::setVisible);
    layout->addWidget(advanced);
    auto* buttons = new QHBoxLayout;
    save_ = new QPushButton(tr("保存配置（不保存密钥）"), this);
    save_->setObjectName(QStringLiteral("llmSaveConfiguration"));
    test_ = new QPushButton(tr("测试连接（内置短样本）"), this);
    test_->setObjectName(QStringLiteral("llmTestConnection"));
    cancel_ = new QPushButton(tr("取消测试"), this);
    cancel_->setObjectName(QStringLiteral("llmCancelTest"));
    cancel_->setEnabled(false);
    buttons->addWidget(save_);
    buttons->addWidget(test_);
    buttons->addWidget(cancel_);
    buttons->addStretch();
    auto* analyze = new QPushButton(tr("分析当前章（后续阶段）"), this);
    analyze->setObjectName(QStringLiteral("llmAnalyzeChapter"));
    analyze->setEnabled(false);
    buttons->addWidget(analyze);
    layout->addLayout(buttons);
    connection_ = new QLabel(tr("未测试；不会自动连接"), this);
    connection_->setObjectName(QStringLiteral("llmConnectionStatus"));
    connection_->setWordWrap(true);
    connection_->setTextFormat(Qt::PlainText);
    layout->addWidget(connection_);
    metrics_ =
        new QLabel(tr("生成速度：未提供（非流式测试）；耗时与 token 用量将在响应后显示"), this);
    metrics_->setObjectName(QStringLiteral("llmTestMetrics"));
    metrics_->setWordWrap(true);
    layout->addWidget(metrics_);
    totals_ = new QLabel(
        tr("连接测试累计：尚无响应。项目/每章节 token：尚无章节任务，不包含连接测试。"), this);
    totals_->setObjectName(QStringLiteral("llmTestTotals"));
    totals_->setWordWrap(true);
    layout->addWidget(totals_);
    resetTokens_ = new QPushButton(tr("重置token计数"), this);
    resetTokens_->setObjectName(QStringLiteral("llmResetTokenCounts"));
    resetTokens_->setToolTip(tr("仅重置本会话连接测试累计，不修改项目、连接配置或上次响应。"));
    layout->addWidget(resetTokens_, 0, Qt::AlignLeft);
    auto* roles = new QTableWidget(3, 4, this);
    roles->setObjectName(QStringLiteral("llmRoleStatus"));
    roles->setHorizontalHeaderLabels({tr("角色"), tr("状态"), tr("Token"), tr("耗时")});
    roles->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    roles->setEditTriggers(QAbstractItemView::NoEditTriggers);
    roles->verticalHeader()->hide();
    const QStringList names{tr("分析员"), tr("复核员（同模型交叉检查）"), tr("审查员（LLM 角色）")};
    for (int row = 0; row < 3; ++row) {
        roles->setItem(row, 0, new QTableWidgetItem(names.at(row)));
        roles->setItem(row, 1, new QTableWidgetItem(tr("尚未接通")));
        roles->setItem(row, 2, new QTableWidgetItem(QStringLiteral("—")));
        roles->setItem(row, 3, new QTableWidgetItem(QStringLiteral("—")));
    }
    layout->addWidget(roles);
    log_ = new QPlainTextEdit(this);
    log_->setObjectName(QStringLiteral("llmConnectionLog"));
    log_->setReadOnly(true);
    log_->setMaximumBlockCount(100);
    log_->setPlaceholderText(tr("脱敏连接记录；不记录密钥、请求正文或原始响应"));
    layout->addWidget(log_, 1);
    const auto key = [](const char* name) {
        return QStringLiteral("llmWorkbench/") + QString::fromLatin1(name);
    };
    endpoint_->setText(settings_->value(key("endpoint")).toString());
    modelId_->setText(settings_->value(key("modelId")).toString());
    modelSize_->setCurrentText(
        settings_->value(key("modelSize"), QStringLiteral("14B")).toString());
    format_->setCurrentText(settings_->value(key("format"), QStringLiteral("text")).toString());
    tokenField_->setCurrentText(
        settings_->value(key("tokenField"), QStringLiteral("max_completion_tokens")).toString());
    timeout_->setValue(settings_->value(key("timeoutSeconds"), 120).toInt());
    outputBudget_->setValue(settings_->value(key("outputBudget"), 128).toInt());
    // Plain HTTP permission is session-only, like the secret.
    connect(save_, &QPushButton::clicked, this, &LlmWorkbench::saveConfiguration);
    connect(test_, &QPushButton::clicked, this, &LlmWorkbench::testConnection);
    connect(cancel_, &QPushButton::clicked, this, &LlmWorkbench::cancelTest);
    connect(resetTokens_, &QPushButton::clicked, this, &LlmWorkbench::resetTokenCounts);
    const auto changed = [this] { connection_->setText(tr("配置已修改，请重新测试")); };
    connect(endpoint_, &QLineEdit::textChanged, this, changed);
    connect(modelId_, &QLineEdit::textChanged, this, changed);
    connect(apiKey_, &QLineEdit::textChanged, this, changed);
    connect(modelSize_, &QComboBox::currentTextChanged, this, changed);
    connect(format_, &QComboBox::currentTextChanged, this, changed);
    connect(tokenField_, &QComboBox::currentTextChanged, this, changed);
    connect(allowHttp_, &QCheckBox::toggled, this, changed);
    connect(timeout_, &QSpinBox::valueChanged, this, changed);
    connect(outputBudget_, &QSpinBox::valueChanged, this, changed);
}

LlmWorkbench::~LlmWorkbench() {
    closing_ = true;
    client_.reset();
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
    if (const auto error = configurationError(); !error.isEmpty()) {
        connection_->setText(error);
        return;
    }
    settings_->beginGroup(QStringLiteral("llmWorkbench"));
    settings_->setValue(QStringLiteral("endpoint"), endpoint_->text().trimmed());
    settings_->setValue(QStringLiteral("modelId"), modelId_->text().trimmed());
    settings_->setValue(QStringLiteral("modelSize"), modelSize_->currentText());
    settings_->setValue(QStringLiteral("format"), format_->currentText());
    settings_->setValue(QStringLiteral("tokenField"), tokenField_->currentText());
    settings_->setValue(QStringLiteral("timeoutSeconds"), timeout_->value());
    settings_->setValue(QStringLiteral("outputBudget"), outputBudget_->value());
    settings_->endGroup();
    settings_->sync();
    connection_->setText(settings_->status() == QSettings::NoError
                             ? tr("配置已保存，密钥和 HTTP 授权未保存；连接尚需测试。")
                             : tr("配置保存失败。"));
}

void LlmWorkbench::setBusy(bool busy) {
    for (auto* widget :
         QList<QWidget*>{endpoint_, modelSize_, modelId_, apiKey_, allowHttp_, format_, tokenField_,
                         timeout_, outputBudget_, save_, test_, resetTokens_}) {
        widget->setEnabled(!busy);
    }
    cancel_->setEnabled(busy);
}

void LlmWorkbench::testConnection() {
    if (!requestId_.isNull()) {
        return;
    }
    if (const auto error = configurationError(); !error.isEmpty()) {
        connection_->setText(error);
        return;
    }
    const QUrl endpoint(endpoint_->text().trimmed(), QUrl::StrictMode);
    client_ = std::make_unique<llm::QwenClient>(
        llm::QwenClientOptions{endpoint, apiKey_->text().trimmed().toUtf8(),
                               modelId_->text().trimmed(), privateHost(endpoint),
                               tokenField_->currentText() == QStringLiteral("max_tokens")
                                   ? llm::CompletionTokenParameter::MaxTokens
                                   : llm::CompletionTokenParameter::MaxCompletionTokens});
    llm::LLMRequest request;
    request.messages = {
        {llm::LLMRole::System, QStringLiteral("This is a synthetic connection test. Return only "
                                              "{\"ok\":true}. Do not add reasoning text.")},
        {llm::LLMRole::User, QStringLiteral("Return the JSON object {\"ok\":true}.")}};
    request.timeoutMs = timeout_->value() * 1000;
    request.maxCompletionTokens = outputBudget_->value();
    request.retryPolicy.maxRetries = 0;
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
    requestId_ = {};
    setBusy(false);
    if (const auto* error = std::get_if<llm::LLMError>(&result)) {
        const auto message = error->code == llm::LLMErrorCode::Cancelled
                                 ? tr("已取消；服务端可能仍在计算。")
                                 : tr("测试失败：%1，HTTP %2。检查地址、鉴权、参数和服务状态。")
                                       .arg(llm::errorCodeName(error->code))
                                       .arg(error->httpStatus);
        connection_->setText(message);
        log_->appendPlainText(message);
        metrics_->setText(tr("本次失败/取消；耗时 %1 ms；token 用量未知，可能已产生消耗。")
                              .arg(error->latencyMs));
        ++missingUsage_;
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
                                 "生成速度：未提供（非流式）。端到端输出吞吐 %5 "
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
    }
    updateTokenTotals();
}

void LlmWorkbench::updateTokenTotals() {
    totals_->setText(
        tr("本会话连接测试累计已知：输入 %1 · 输出 %2 · 总计 %3 token；%4 次用量未知。\n"
           "项目/每章节 token：尚无章节任务，不包含连接测试。")
            .arg(inputTokens_)
            .arg(outputTokens_)
            .arg(inputTokens_ + outputTokens_)
            .arg(missingUsage_));
}

} // namespace loreforge::app
