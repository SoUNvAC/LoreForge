#pragma once

#include "loreforge/core/content_hash.h"
#include "loreforge/core/identifier.h"
#include "loreforge/core/source_span.h"

#include <QList>
#include <QString>
#include <QStringList>
#include <QStringView>

#include <optional>

namespace loreforge::document {

enum class BlockType {
    Paragraph,
    Heading,
    SceneBreak,
    Unknown,
};

[[nodiscard]] QString blockTypeToString(BlockType type);
[[nodiscard]] std::optional<BlockType> blockTypeFromString(QStringView value);

struct BookMetadata final {
    QString title;
    QStringList authors;
    QString language;
    QString sourceFormat;
    QString sourceLocator;
    core::ContentHash sourceHash;

    friend bool operator==(const BookMetadata&, const BookMetadata&) = default;
};

struct Block final {
    BlockType type = BlockType::Unknown;
    QString text;
    core::SourceSpan sourceSpan;
    std::optional<double> extractionConfidence;

    friend bool operator==(const Block&, const Block&) = default;
};

struct Chapter final {
    core::ChapterId id;
    qsizetype index = -1;
    QString title;
    QList<Block> blocks;

    friend bool operator==(const Chapter&, const Chapter&) = default;
};

struct Document final {
    core::BookId id;
    BookMetadata metadata;
    QList<Chapter> chapters;

    friend bool operator==(const Document&, const Document&) = default;
};

} // namespace loreforge::document
