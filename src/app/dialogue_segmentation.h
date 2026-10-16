#pragma once
#include "chapter_evidence.h"
#include <QJsonArray>

namespace loreforge::app {
[[nodiscard]] QJsonArray dialogueUnits(const QJsonObject& source);
[[nodiscard]] QJsonObject dialogueSchema();
[[nodiscard]] QString dialoguePrompt();
[[nodiscard]] QJsonArray dialogueReviewBlocks(const QJsonObject& snapshot);
[[nodiscard]] ResolvedChapterEvidence resolveDialogue(const QJsonObject& output,
                                                      const QJsonObject& snapshot);
} // namespace loreforge::app
