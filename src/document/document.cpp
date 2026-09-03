#include "loreforge/document/document.h"

namespace loreforge::document {

QString blockTypeToString(BlockType type) {
    switch (type) {
    case BlockType::Paragraph:
        return QStringLiteral("paragraph");
    case BlockType::Heading:
        return QStringLiteral("heading");
    case BlockType::SceneBreak:
        return QStringLiteral("scene_break");
    case BlockType::Unknown:
        return QStringLiteral("unknown");
    }

    return QStringLiteral("unknown");
}

std::optional<BlockType> blockTypeFromString(QStringView value) {
    if (value == QStringLiteral("paragraph")) {
        return BlockType::Paragraph;
    }
    if (value == QStringLiteral("heading")) {
        return BlockType::Heading;
    }
    if (value == QStringLiteral("scene_break")) {
        return BlockType::SceneBreak;
    }
    if (value == QStringLiteral("unknown")) {
        return BlockType::Unknown;
    }
    return std::nullopt;
}

} // namespace loreforge::document
