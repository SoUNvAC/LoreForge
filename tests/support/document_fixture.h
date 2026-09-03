#pragma once

#include "loreforge/core/content_hash.h"
#include "loreforge/core/identifier.h"
#include "loreforge/document/document.h"

#include <QByteArray>
#include <QString>

namespace loreforge::test {

inline QByteArray handcraftedSource() {
    return QByteArrayLiteral("Chapter One\nHello, world.\n***\nChapter Two\nGoodbye.\n");
}

inline document::Document handcraftedDocument() {
    const auto bookId = core::BookId::fromStableKey(QStringLiteral("fixtures/handcrafted.txt"));

    return {
        bookId,
        {
            QStringLiteral("Handcrafted Fixture"),
            {QStringLiteral("LoreForge Tests")},
            QStringLiteral("en"),
            QStringLiteral("txt"),
            QStringLiteral("fixtures/handcrafted.txt"),
            core::ContentHash::sha256(QByteArrayView(handcraftedSource())),
        },
        {
            {
                core::ChapterId::fromStableKey(bookId.toString() + QStringLiteral(":chapter:0")),
                0,
                QStringLiteral("Chapter One"),
                {
                    {document::BlockType::Heading,
                     QStringLiteral("Chapter One"),
                     {QStringLiteral("fixtures/handcrafted.txt"), 0, 11}},
                    {document::BlockType::Paragraph,
                     QStringLiteral("Hello, world."),
                     {QStringLiteral("fixtures/handcrafted.txt"), 12, 25}},
                    {document::BlockType::SceneBreak,
                     QStringLiteral("***"),
                     {QStringLiteral("fixtures/handcrafted.txt"), 26, 29}},
                },
            },
            {
                core::ChapterId::fromStableKey(bookId.toString() + QStringLiteral(":chapter:1")),
                1,
                QStringLiteral("Chapter Two"),
                {
                    {document::BlockType::Heading,
                     QStringLiteral("Chapter Two"),
                     {QStringLiteral("fixtures/handcrafted.txt"), 30, 41}},
                    {document::BlockType::Paragraph,
                     QStringLiteral("Goodbye."),
                     {QStringLiteral("fixtures/handcrafted.txt"), 42, 50}},
                },
            },
        },
    };
}

} // namespace loreforge::test
