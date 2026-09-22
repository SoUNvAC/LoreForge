#pragma once

#include "loreforge/inference/inference_types.h"
#include "loreforge/llm/llm_types.h"
#include "loreforge/narrative/story_memory.h"

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

namespace loreforge::context {

struct ContextBudget final {
    int maximumTokens = 0;
    int reservedCompletionTokens = 0;

    [[nodiscard]] bool isValid() const noexcept;
    [[nodiscard]] int promptTokenLimit() const noexcept;
    friend bool operator==(const ContextBudget&, const ContextBudget&) = default;
};

struct ContextBuildInput final {
    core::ProjectId projectId;
    narrative::StoryStateSnapshot storyState;
    QString systemRules;
    QString background;
    QStringList canonicalTerminology;
    QString recentSummary;
    QString currentChapter;
    QString taskInstructions;
    QJsonObject outputSchema;

    friend bool operator==(const ContextBuildInput&, const ContextBuildInput&) = default;
};

enum class MemoryKind {
    Character,
    Event,
    OpenThread,
};

struct MemoryCandidate final {
    MemoryKind kind = MemoryKind::Character;
    QString stableId;
    QString text;
    int priority = 0;

    friend bool operator==(const MemoryCandidate&, const MemoryCandidate&) = default;
};

struct ContextInspectorData final {
    QString systemRules;
    QString background;
    QStringList canonicalTerminology;
    QStringList characterMemory;
    QStringList eventMemory;
    QStringList openThreads;
    QString recentSummary;
    QString currentChapter;
    QString taskInstructions;
    QJsonObject outputSchema;
    ContextBudget budget;
    int estimatedTokens = 0;
    int omittedCharacters = 0;
    int omittedEvents = 0;
    int omittedOpenThreads = 0;
    QString userPrompt;
    QString rawFinalPrompt;

    friend bool operator==(const ContextInspectorData&, const ContextInspectorData&) = default;
};

struct BuiltContext final {
    inference::ContextSnapshot snapshot;
    ContextInspectorData inspector;
    QList<llm::LLMMessage> messages;

    friend bool operator==(const BuiltContext&, const BuiltContext&) = default;
};

enum class ContextBuildErrorCode {
    InvalidProject,
    InvalidBudget,
    InvalidInput,
    StoryStateMismatch,
    MandatoryContentExceedsBudget,
};

struct ContextBuildError final {
    ContextBuildErrorCode code;
    QString path;
    QString message;

    friend bool operator==(const ContextBuildError&, const ContextBuildError&) = default;
};

struct ContextBuildResult final {
    std::optional<BuiltContext> context;
    QList<ContextBuildError> errors;

    [[nodiscard]] bool isValid() const noexcept;
};

[[nodiscard]] std::optional<ContextInspectorData>
inspectorFromSnapshot(const inference::ContextSnapshot& snapshot);

} // namespace loreforge::context
