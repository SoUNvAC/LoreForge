#pragma once

#include "loreforge/document/document.h"

#include <QByteArray>
#include <QStringList>
#include <QStringView>

#include <variant>

namespace loreforge::parser {

struct MarkdownSourceOptions final {
    QString title;
    QStringList authors;
    QString language = QStringLiteral("zh");
};

enum class MarkdownSourceErrorCode {
    InvalidRoot,
    ReadFailure,
    InvalidUtf8,
    InvalidSummary,
    UnsafePath,
    DuplicatePath,
    EmptyChapter,
    InvalidMarkdown,
    InvalidDocument,
};

struct MarkdownSourceError final {
    MarkdownSourceErrorCode code;
    QString message;
};

struct MarkdownSourceImport final {
    document::Document document;
    qsizetype volumeCount = 0;
    // Top-level supplementary entries are intentionally outside the narrative document.
    QStringList excludedPaths;
};

using MarkdownSourceResult = std::variant<MarkdownSourceImport, MarkdownSourceError>;

class MarkdownSourceParser final {
  public:
    // Read-only import of a directory containing SUMMARY.md (or SUMMARY.md itself).
    // One linked file is one chapter; nested headings never create extra chapters.
    [[nodiscard]] static MarkdownSourceResult
    parseDirectory(QStringView path, const MarkdownSourceOptions& options = {});
    // Reparse a single read-only source file using the same narrative dialect as import.
    [[nodiscard]] static std::variant<document::Chapter, MarkdownSourceError>
    parseChapterBytes(document::Chapter chapter, QStringView sourceId, const QByteArray& bytes);
};

} // namespace loreforge::parser
