#include "loreforge/context/context_builder.h"

#include "loreforge/context/relevant_memory_retriever.h"
#include "loreforge/inference/output_validator.h"
#include "loreforge/narrative/story_memory_json.h"
#include "loreforge/text/token_estimator.h"

#include <QJsonArray>
#include <QSet>

#include <limits>
#include <utility>

namespace loreforge::context {
namespace {

struct RenderedPrompt final {
    QString userPrompt;
    QString rawPrompt;
    int estimatedTokens = 0;
};

void addError(ContextBuildResult& result, ContextBuildErrorCode code, QString path,
              QString message) {
    result.errors.append({code, std::move(path), std::move(message)});
}

QString renderedList(const QStringList& values) {
    if (values.isEmpty()) {
        return QStringLiteral("(none)");
    }
    QStringList lines;
    lines.reserve(values.size());
    for (const auto& value : values) {
        lines.append(QStringLiteral("- ") + value);
    }
    return lines.join(QLatin1Char('\n'));
}

RenderedPrompt render(const ContextBuildInput& input, const QStringList& characterMemory,
                      const QStringList& eventMemory, const QStringList& openThreads) {
    const auto schema = QString::fromUtf8(inference::canonicalJson(input.outputSchema));
    const auto userPrompt =
        QStringLiteral("[PROJECT BACKGROUND]\n%1\n\n"
                       "[CANONICAL TERMINOLOGY]\n%2\n\n"
                       "[CHARACTER MEMORY]\n%3\n\n"
                       "[EVENT MEMORY]\n%4\n\n"
                       "[OPEN THREADS]\n%5\n\n"
                       "[RECENT SUMMARY]\n%6\n\n"
                       "[CURRENT CHAPTER]\n%7\n\n"
                       "[TASK]\n%8\n\n"
                       "[OUTPUT SCHEMA]\n%9")
            .arg(input.background.isEmpty() ? QStringLiteral("(none)") : input.background,
                 renderedList(input.canonicalTerminology), renderedList(characterMemory),
                 renderedList(eventMemory), renderedList(openThreads),
                 input.recentSummary.isEmpty() ? QStringLiteral("(none)") : input.recentSummary,
                 input.currentChapter, input.taskInstructions, schema);
    const auto rawPrompt =
        QStringLiteral("SYSTEM:\n%1\n\nUSER:\n%2").arg(input.systemRules, userPrompt);
    const auto estimate = text::TokenEstimator::estimate(rawPrompt);
    return {userPrompt, rawPrompt,
            estimate > std::numeric_limits<int>::max() ? std::numeric_limits<int>::max()
                                                       : static_cast<int>(estimate)};
}

QJsonArray strings(const QStringList& values) {
    QJsonArray result;
    for (const auto& value : values) {
        result.append(value);
    }
    return result;
}

bool validStoryState(const narrative::StoryStateSnapshot& storyState) {
    if (!storyState.id.isValid() || !storyState.projectId.isValid() ||
        storyState.throughChapterSequence < 0 || storyState.sourceChapters.isEmpty() ||
        !storyState.sourceHash.isValid() || !storyState.stateHash.isValid()) {
        return false;
    }
    auto payload = storyState;
    payload.id = {};
    payload.sourceHash = {};
    payload.stateHash = {};
    const auto expectedStateHash =
        core::ContentHash::sha256(narrative::encodeStoryStateSnapshot(payload));
    const auto expectedId = core::StoryStateSnapshotId::fromStableKey(
        storyState.projectId.toString() + QLatin1Char(':') +
        QString::number(storyState.throughChapterSequence) + QLatin1Char(':') +
        storyState.sourceHash.toHex() + QLatin1Char(':') + expectedStateHash.toHex());
    return storyState.stateHash == expectedStateHash && storyState.id == expectedId;
}

} // namespace

ContextBuildResult ContextBuilder::build(ContextBuildInput input, ContextBudget budget,
                                         QDateTime createdAt) {
    ContextBuildResult result;
    if (!input.projectId.isValid()) {
        addError(result, ContextBuildErrorCode::InvalidProject, QStringLiteral("$.project_id"),
                 QStringLiteral("A valid project ID is required."));
    }
    if (!budget.isValid()) {
        addError(result, ContextBuildErrorCode::InvalidBudget, QStringLiteral("$.budget"),
                 QStringLiteral("The token budget and completion reserve are invalid."));
    }
    if (!createdAt.isValid() || input.systemRules.trimmed().isEmpty() ||
        input.currentChapter.trimmed().isEmpty() || input.taskInstructions.trimmed().isEmpty() ||
        input.outputSchema.isEmpty()) {
        addError(
            result, ContextBuildErrorCode::InvalidInput, QStringLiteral("$.input"),
            QStringLiteral("System rules, current chapter, task, schema, and time are required."));
    }
    const auto schemaErrors = inference::OutputValidator::validateSchema(input.outputSchema);
    if (!schemaErrors.isEmpty()) {
        addError(result, ContextBuildErrorCode::InvalidInput, QStringLiteral("$.output_schema"),
                 schemaErrors.join(QLatin1Char('\n')));
    }
    QSet<QString> terms;
    for (qsizetype index = 0; index < input.canonicalTerminology.size(); ++index) {
        const auto& term = input.canonicalTerminology.at(index);
        const auto key = term.normalized(QString::NormalizationForm_C).toCaseFolded();
        if (term.trimmed().isEmpty() || term != term.trimmed() || terms.contains(key)) {
            addError(result, ContextBuildErrorCode::InvalidInput,
                     QStringLiteral("$.canonical_terminology[%1]").arg(index),
                     QStringLiteral("Terminology must be non-empty, trimmed, and unique."));
        }
        terms.insert(key);
    }
    if (!validStoryState(input.storyState) || input.storyState.projectId != input.projectId) {
        addError(result, ContextBuildErrorCode::StoryStateMismatch, QStringLiteral("$.story_state"),
                 QStringLiteral("A valid story-state snapshot owned by the project is required."));
    }
    if (!result.errors.isEmpty()) {
        return result;
    }

    QStringList characters;
    QStringList events;
    QStringList threads;
    auto rendered = render(input, characters, events, threads);
    if (rendered.estimatedTokens > budget.promptTokenLimit()) {
        addError(result, ContextBuildErrorCode::MandatoryContentExceedsBudget,
                 QStringLiteral("$.budget"),
                 QStringLiteral(
                     "Mandatory context requires %1 estimated tokens but the prompt limit is %2.")
                     .arg(rendered.estimatedTokens)
                     .arg(budget.promptTokenLimit()));
        return result;
    }

    int omittedCharacters = 0;
    int omittedEvents = 0;
    int omittedThreads = 0;
    const auto candidates = RelevantMemoryRetriever::rank(input.storyState, input.currentChapter,
                                                          input.taskInstructions);
    for (const auto& candidate : candidates) {
        auto proposedCharacters = characters;
        auto proposedEvents = events;
        auto proposedThreads = threads;
        switch (candidate.kind) {
        case MemoryKind::Character:
            proposedCharacters.append(candidate.text);
            break;
        case MemoryKind::Event:
            proposedEvents.append(candidate.text);
            break;
        case MemoryKind::OpenThread:
            proposedThreads.append(candidate.text);
            break;
        }
        auto proposed = render(input, proposedCharacters, proposedEvents, proposedThreads);
        if (proposed.estimatedTokens <= budget.promptTokenLimit()) {
            characters = std::move(proposedCharacters);
            events = std::move(proposedEvents);
            threads = std::move(proposedThreads);
            rendered = std::move(proposed);
        } else {
            switch (candidate.kind) {
            case MemoryKind::Character:
                ++omittedCharacters;
                break;
            case MemoryKind::Event:
                ++omittedEvents;
                break;
            case MemoryKind::OpenThread:
                ++omittedThreads;
                break;
            }
        }
    }

    ContextInspectorData inspector{
        input.systemRules,
        input.background,
        input.canonicalTerminology,
        characters,
        events,
        threads,
        input.recentSummary,
        input.currentChapter,
        input.taskInstructions,
        input.outputSchema,
        budget,
        rendered.estimatedTokens,
        omittedCharacters,
        omittedEvents,
        omittedThreads,
        rendered.userPrompt,
        rendered.rawPrompt,
    };
    const QJsonObject content{
        {QStringLiteral("kind"), QStringLiteral("loreforge-context-v1")},
        {QStringLiteral("story_state_snapshot_id"), input.storyState.id.toString()},
        {QStringLiteral("story_state_source_hash"), input.storyState.sourceHash.toHex()},
        {QStringLiteral("story_state_hash"), input.storyState.stateHash.toHex()},
        {QStringLiteral("system_rules"), inspector.systemRules},
        {QStringLiteral("background"), inspector.background},
        {QStringLiteral("canonical_terminology"), strings(inspector.canonicalTerminology)},
        {QStringLiteral("character_memory"), strings(inspector.characterMemory)},
        {QStringLiteral("event_memory"), strings(inspector.eventMemory)},
        {QStringLiteral("open_threads"), strings(inspector.openThreads)},
        {QStringLiteral("recent_summary"), inspector.recentSummary},
        {QStringLiteral("current_chapter"), inspector.currentChapter},
        {QStringLiteral("task_instructions"), inspector.taskInstructions},
        {QStringLiteral("output_schema"), inspector.outputSchema},
        {QStringLiteral("budget"),
         QJsonObject{
             {QStringLiteral("maximum_tokens"), budget.maximumTokens},
             {QStringLiteral("reserved_completion_tokens"), budget.reservedCompletionTokens},
             {QStringLiteral("prompt_token_limit"), budget.promptTokenLimit()}}},
        {QStringLiteral("estimated_tokens"), inspector.estimatedTokens},
        {QStringLiteral("omitted"),
         QJsonObject{{QStringLiteral("characters"), inspector.omittedCharacters},
                     {QStringLiteral("events"), inspector.omittedEvents},
                     {QStringLiteral("open_threads"), inspector.omittedOpenThreads}}},
        {QStringLiteral("user_prompt"), inspector.userPrompt},
        {QStringLiteral("raw_final_prompt"), inspector.rawFinalPrompt},
    };
    const auto contentHash = core::ContentHash::sha256(inference::canonicalJson(content));
    const auto snapshotId = core::ContextSnapshotId::fromStableKey(
        input.projectId.toString() + QLatin1Char(':') + contentHash.toHex());
    auto snapshot = inference::makeContextSnapshot(snapshotId, input.projectId, content, createdAt);
    result.context = BuiltContext{
        std::move(snapshot),
        std::move(inspector),
        {{llm::LLMRole::System, input.systemRules}, {llm::LLMRole::User, rendered.userPrompt}}};
    return result;
}

} // namespace loreforge::context
