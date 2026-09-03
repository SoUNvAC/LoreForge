#include "loreforge/parser/plain_text_parser.h"

#include "loreforge/core/content_hash.h"
#include "loreforge/core/identifier.h"
#include "loreforge/document/document_validation.h"
#include "loreforge/text/chapter_heading_detector.h"
#include "loreforge/text/paragraph_detector.h"
#include "loreforge/text/text_normalizer.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringDecoder>

#include <optional>
#include <utility>

namespace loreforge::parser {
namespace {

struct SourceLine final {
    QString text;
    qint64 startByte = 0;
    qint64 endByte = 0;
};

PlainTextParseError error(PlainTextParseErrorCode code, QString message) {
    return {code, std::move(message)};
}

QList<SourceLine> splitSourceLines(const QByteArray& source, qsizetype contentStart) {
    QList<SourceLine> lines;
    auto cursor = contentStart;

    while (cursor < source.size()) {
        const auto lineStart = cursor;
        while (cursor < source.size() && source.at(cursor) != '\r' && source.at(cursor) != '\n') {
            ++cursor;
        }
        const auto lineEnd = cursor;
        lines.append({
            QString::fromUtf8(source.constData() + lineStart, lineEnd - lineStart),
            static_cast<qint64>(lineStart),
            static_cast<qint64>(lineEnd),
        });

        if (cursor < source.size() && source.at(cursor) == '\r') {
            ++cursor;
            if (cursor < source.size() && source.at(cursor) == '\n') {
                ++cursor;
            }
        } else if (cursor < source.size()) {
            ++cursor;
        }
    }

    return lines;
}

QString effectiveTitle(const PlainTextImportOptions& options) {
    const auto requestedTitle = text::TextNormalizer::normalizeBlockLine(options.title);
    if (!requestedTitle.isEmpty()) {
        return requestedTitle;
    }

    const auto fileTitle = text::TextNormalizer::normalizeBlockLine(
        QFileInfo(options.sourceLocator).completeBaseName());
    return fileTitle.isEmpty() ? QStringLiteral("Untitled") : fileTitle;
}

} // namespace

PlainTextParseResult PlainTextParser::parse(QByteArrayView source,
                                            const PlainTextImportOptions& options) {
    const auto sourceLocator = options.sourceLocator.trimmed();
    if (sourceLocator.isEmpty()) {
        return error(PlainTextParseErrorCode::MissingSourceLocator,
                     QStringLiteral("A source locator is required for reproducible import."));
    }

    const QByteArray sourceBytes(source.data(), source.size());
    QStringDecoder decoder(QStringDecoder::Utf8, QStringConverter::Flag::Stateless);
    const QString decodedSource = decoder.decode(sourceBytes);
    static_cast<void>(decodedSource);
    if (decoder.hasError()) {
        return error(PlainTextParseErrorCode::InvalidUtf8,
                     QStringLiteral("The source is not valid UTF-8."));
    }

    qsizetype contentStart = 0;
    if (sourceBytes.startsWith(QByteArrayView("\xEF\xBB\xBF", 3))) {
        contentStart = 3;
    }

    const auto sourceId =
        options.sourceId.trimmed().isEmpty() ? sourceLocator : options.sourceId.trimmed();
    const auto title = effectiveTitle(options);
    const auto bookId = core::BookId::fromStableKey(sourceLocator);
    document::Document document{
        bookId,
        {
            title,
            options.authors,
            options.language.trimmed().isEmpty() ? QStringLiteral("und")
                                                 : options.language.trimmed(),
            QStringLiteral("txt"),
            sourceLocator,
            core::ContentHash::sha256(QByteArrayView(sourceBytes)),
        },
        {},
    };

    QStringList paragraphLines;
    qint64 paragraphStart = 0;
    qint64 paragraphEnd = 0;

    const auto beginChapter = [&document, &bookId](const QString& chapterTitle) {
        const auto index = document.chapters.size();
        const auto chapterKey =
            bookId.toString() + QStringLiteral(":chapter:") + QString::number(index);
        document.chapters.append(
            {core::ChapterId::fromStableKey(chapterKey), index, chapterTitle, {}});
    };

    const auto ensureChapter = [&document, &beginChapter, &title] {
        if (document.chapters.isEmpty()) {
            beginChapter(title);
        }
    };

    const auto flushParagraph = [&] {
        if (paragraphLines.isEmpty()) {
            return;
        }
        ensureChapter();
        document.chapters.last().blocks.append({
            document::BlockType::Paragraph,
            text::ParagraphDetector::joinWrappedLines(paragraphLines),
            {sourceId, paragraphStart, paragraphEnd},
        });
        paragraphLines.clear();
    };

    for (const auto& line : splitSourceLines(sourceBytes, contentStart)) {
        const auto heading = text::ChapterHeadingDetector::detect(line.text);
        if (heading.has_value()) {
            flushParagraph();
            beginChapter(*heading);
            document.chapters.last().blocks.append({
                document::BlockType::Heading,
                *heading,
                {sourceId, line.startByte, line.endByte},
            });
            continue;
        }

        if (text::ParagraphDetector::isBlank(line.text)) {
            flushParagraph();
            continue;
        }

        if (text::ParagraphDetector::isSceneBreak(line.text)) {
            flushParagraph();
            ensureChapter();
            document.chapters.last().blocks.append({
                document::BlockType::SceneBreak,
                text::TextNormalizer::normalizeBlockLine(line.text),
                {sourceId, line.startByte, line.endByte},
            });
            continue;
        }

        if (paragraphLines.isEmpty()) {
            paragraphStart = line.startByte;
        }
        paragraphLines.append(line.text);
        paragraphEnd = line.endByte;
    }
    flushParagraph();

    if (document.chapters.isEmpty()) {
        return error(PlainTextParseErrorCode::EmptyInput,
                     QStringLiteral("The source contains no importable text."));
    }

    const auto validation = document::validateDocument(document);
    if (!validation.isValid()) {
        return error(PlainTextParseErrorCode::InvalidDocument,
                     QStringLiteral("The imported document failed domain validation: %1")
                         .arg(validation.errors.first().message));
    }

    return document;
}

PlainTextParseResult PlainTextParser::parseFile(QStringView filePath,
                                                PlainTextImportOptions options) {
    const QFileInfo fileInfo(filePath.toString());
    if (!fileInfo.exists() || !fileInfo.isFile()) {
        return error(PlainTextParseErrorCode::FileNotFound,
                     QStringLiteral("The text file does not exist."));
    }

    QFile file(fileInfo.absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly)) {
        return error(PlainTextParseErrorCode::ReadFailure,
                     QStringLiteral("The text file could not be read: %1").arg(file.errorString()));
    }

    if (options.sourceLocator.trimmed().isEmpty()) {
        options.sourceLocator = QDir::cleanPath(fileInfo.absoluteFilePath());
    }
    const auto source = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        return error(PlainTextParseErrorCode::ReadFailure,
                     QStringLiteral("The text file could not be read completely: %1")
                         .arg(file.errorString()));
    }
    return parse(QByteArrayView(source), options);
}

} // namespace loreforge::parser
