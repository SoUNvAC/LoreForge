#include "loreforge/context/context_types.h"

#include "loreforge/inference/output_validator.h"
#include "loreforge/text/token_estimator.h"

#include <QJsonArray>

#include <limits>

namespace loreforge::context {
namespace {

std::optional<QStringList> stringArray(const QJsonObject& object, QStringView key) {
    const auto value = object.value(key.toString());
    if (!value.isArray()) {
        return std::nullopt;
    }
    QStringList result;
    for (const auto& item : value.toArray()) {
        if (!item.isString()) {
            return std::nullopt;
        }
        result.append(item.toString());
    }
    return result;
}

bool isInteger(const QJsonValue& value) {
    return value.isDouble() && value.toDouble() == value.toInteger(-1);
}

} // namespace

bool ContextBudget::isValid() const noexcept {
    return maximumTokens > 0 && reservedCompletionTokens > 0 &&
           reservedCompletionTokens < maximumTokens;
}

int ContextBudget::promptTokenLimit() const noexcept {
    return isValid() ? maximumTokens - reservedCompletionTokens : 0;
}

bool ContextBuildResult::isValid() const noexcept {
    return context.has_value() && errors.isEmpty();
}

std::optional<ContextInspectorData>
inspectorFromSnapshot(const inference::ContextSnapshot& snapshot) {
    if (!snapshot.id.isValid() || !snapshot.projectId.isValid() || !snapshot.createdAt.isValid() ||
        snapshot.contentHash !=
            core::ContentHash::sha256(inference::canonicalJson(snapshot.content)) ||
        snapshot.content.value(QStringLiteral("kind")).toString() !=
            QStringLiteral("loreforge-context-v1")) {
        return std::nullopt;
    }
    const auto budgetObject = snapshot.content.value(QStringLiteral("budget")).toObject();
    const auto maximumTokens = budgetObject.value(QStringLiteral("maximum_tokens"));
    const auto reservedTokens = budgetObject.value(QStringLiteral("reserved_completion_tokens"));
    const auto promptTokenLimit = budgetObject.value(QStringLiteral("prompt_token_limit"));
    const auto estimatedTokens = snapshot.content.value(QStringLiteral("estimated_tokens"));
    const auto omitted = snapshot.content.value(QStringLiteral("omitted")).toObject();
    const auto omittedCharacters = omitted.value(QStringLiteral("characters"));
    const auto omittedEvents = omitted.value(QStringLiteral("events"));
    const auto omittedThreads = omitted.value(QStringLiteral("open_threads"));
    const auto terminology = stringArray(snapshot.content, QStringLiteral("canonical_terminology"));
    const auto characters = stringArray(snapshot.content, QStringLiteral("character_memory"));
    const auto events = stringArray(snapshot.content, QStringLiteral("event_memory"));
    const auto threads = stringArray(snapshot.content, QStringLiteral("open_threads"));
    const auto outputSchema = snapshot.content.value(QStringLiteral("output_schema"));
    for (const auto& requiredString :
         {QStringLiteral("system_rules"), QStringLiteral("background"),
          QStringLiteral("recent_summary"), QStringLiteral("current_chapter"),
          QStringLiteral("task_instructions"), QStringLiteral("user_prompt"),
          QStringLiteral("raw_final_prompt")}) {
        if (!snapshot.content.value(requiredString).isString()) {
            return std::nullopt;
        }
    }
    const auto storyStateId = core::StoryStateSnapshotId::fromString(
        snapshot.content.value(QStringLiteral("story_state_snapshot_id")).toString());
    const auto storyStateSourceHash = core::ContentHash::fromHex(
        snapshot.content.value(QStringLiteral("story_state_source_hash")).toString());
    const auto storyStateHash = core::ContentHash::fromHex(
        snapshot.content.value(QStringLiteral("story_state_hash")).toString());
    const auto expectedSnapshotId = core::ContextSnapshotId::fromStableKey(
        snapshot.projectId.toString() + QLatin1Char(':') + snapshot.contentHash.toHex());
    if (!isInteger(maximumTokens) || !isInteger(reservedTokens) || !isInteger(promptTokenLimit) ||
        !isInteger(estimatedTokens) || !isInteger(omittedCharacters) || !isInteger(omittedEvents) ||
        !isInteger(omittedThreads) || !terminology.has_value() || !characters.has_value() ||
        !events.has_value() || !threads.has_value() || !outputSchema.isObject() ||
        !storyStateId.has_value() || !storyStateSourceHash.has_value() ||
        !storyStateHash.has_value() || snapshot.id != expectedSnapshotId) {
        return std::nullopt;
    }
    ContextInspectorData inspector{
        snapshot.content.value(QStringLiteral("system_rules")).toString(),
        snapshot.content.value(QStringLiteral("background")).toString(),
        *terminology,
        *characters,
        *events,
        *threads,
        snapshot.content.value(QStringLiteral("recent_summary")).toString(),
        snapshot.content.value(QStringLiteral("current_chapter")).toString(),
        snapshot.content.value(QStringLiteral("task_instructions")).toString(),
        outputSchema.toObject(),
        {maximumTokens.toInt(), reservedTokens.toInt()},
        estimatedTokens.toInt(),
        omittedCharacters.toInt(),
        omittedEvents.toInt(),
        omittedThreads.toInt(),
        snapshot.content.value(QStringLiteral("user_prompt")).toString(),
        snapshot.content.value(QStringLiteral("raw_final_prompt")).toString(),
    };
    if (!inspector.budget.isValid() || inspector.estimatedTokens < 0 ||
        inspector.estimatedTokens > inspector.budget.promptTokenLimit() ||
        promptTokenLimit.toInt() != inspector.budget.promptTokenLimit() ||
        inspector.omittedCharacters < 0 || inspector.omittedEvents < 0 ||
        inspector.omittedOpenThreads < 0 || inspector.systemRules.trimmed().isEmpty() ||
        inspector.currentChapter.trimmed().isEmpty() ||
        inspector.taskInstructions.trimmed().isEmpty() || inspector.outputSchema.isEmpty() ||
        !inference::OutputValidator::validateSchema(inspector.outputSchema).isEmpty() ||
        inspector.rawFinalPrompt != QStringLiteral("SYSTEM:\n%1\n\nUSER:\n%2")
                                        .arg(inspector.systemRules, inspector.userPrompt)) {
        return std::nullopt;
    }
    const auto estimated = text::TokenEstimator::estimate(inspector.rawFinalPrompt);
    if (estimated > std::numeric_limits<int>::max() ||
        inspector.estimatedTokens != static_cast<int>(estimated)) {
        return std::nullopt;
    }
    return inspector;
}

} // namespace loreforge::context
