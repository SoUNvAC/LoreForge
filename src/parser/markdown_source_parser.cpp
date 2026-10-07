#include "loreforge/parser/markdown_source_parser.h"

#include "loreforge/document/document_validation.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QStringDecoder>

namespace loreforge::parser {
namespace {

struct Entry final {
    QString title;
    QString path;
    bool nested = false;
};

struct Line final {
    QString text;
    qint64 start;
    qint64 end;
};

using BytesResult = std::variant<QByteArray, MarkdownSourceError>;

BytesResult readUtf8(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return MarkdownSourceError{
            MarkdownSourceErrorCode::ReadFailure,
            QStringLiteral("Cannot read %1: %2").arg(path, file.errorString())};
    }
    auto bytes = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        return MarkdownSourceError{MarkdownSourceErrorCode::ReadFailure,
                                   QStringLiteral("Incomplete read: %1").arg(path)};
    }
    QStringDecoder decoder(QStringDecoder::Utf8, QStringConverter::Flag::Stateless);
    const QString decoded = decoder.decode(bytes);
    static_cast<void>(decoded);
    if (decoder.hasError()) {
        return MarkdownSourceError{MarkdownSourceErrorCode::InvalidUtf8,
                                   QStringLiteral("Invalid UTF-8: %1").arg(path)};
    }
    return bytes;
}

QList<Line> lines(const QByteArray& bytes) {
    QList<Line> result;
    qsizetype cursor = bytes.startsWith(QByteArrayView("\xEF\xBB\xBF", 3)) ? 3 : 0;
    while (cursor < bytes.size()) {
        const auto start = cursor;
        while (cursor < bytes.size() && bytes.at(cursor) != '\r' && bytes.at(cursor) != '\n') {
            ++cursor;
        }
        result.append(
            {QString::fromUtf8(bytes.constData() + start, cursor - start), start, cursor});
        if (cursor < bytes.size() && bytes.at(cursor) == '\r') {
            ++cursor;
        }
        if (cursor < bytes.size() && bytes.at(cursor) == '\n') {
            ++cursor;
        }
    }
    return result;
}

// Length framing prevents ambiguous manifest concatenation; never hash normalized text.
void appendManifest(QByteArray& manifest, const QString& path, const QByteArray& bytes) {
    const auto name = path.toUtf8();
    manifest += QByteArray::number(name.size()) + ':' + name;
    manifest += QByteArray::number(bytes.size()) + ':' + bytes;
}

QString headingText(const QString& line) {
    static const QRegularExpression heading(QStringLiteral("^ {0,3}#{1,6} +(.+?) *#* *$"));
    const auto match = heading.match(line);
    return match.hasMatch() ? match.captured(1) : QString{};
}

std::optional<MarkdownSourceError> parseBlocks(document::Chapter& chapter, const QString& path,
                                               const QByteArray& bytes) {
    const auto sourceLines = lines(bytes);
    qsizetype begin = 0;
    if (!sourceLines.isEmpty() && sourceLines.first().text.trimmed() == QStringLiteral("---")) {
        begin = 1;
        while (begin < sourceLines.size() &&
               sourceLines.at(begin).text.trimmed() != QStringLiteral("---")) {
            ++begin;
        }
        if (begin == sourceLines.size()) {
            return MarkdownSourceError{MarkdownSourceErrorCode::InvalidMarkdown,
                                       QStringLiteral("Unclosed front matter: %1").arg(path)};
        }
        ++begin;
    }
    qint64 paragraphStart = -1;
    qint64 paragraphEnd = 0;
    const auto flush = [&] {
        if (paragraphStart < 0) {
            return;
        }
        const auto text =
            QString::fromUtf8(bytes.mid(paragraphStart, paragraphEnd - paragraphStart));
        chapter.blocks.append(
            {document::BlockType::Paragraph, text, {path, paragraphStart, paragraphEnd}});
        paragraphStart = -1;
    };
    for (qsizetype index = begin; index < sourceLines.size(); ++index) {
        const auto& line = sourceLines.at(index);
        const auto trimmed = line.text.trimmed();
        const auto heading = headingText(line.text);
        if (trimmed.isEmpty()) {
            flush();
        } else if (!heading.isEmpty()) {
            flush();
            chapter.blocks.append(
                {document::BlockType::Heading, heading, {path, line.start, line.end}});
        } else if (trimmed == QStringLiteral("---") || trimmed == QStringLiteral("***") ||
                   trimmed == QStringLiteral("* * *") || trimmed == QStringLiteral("___")) {
            flush();
            chapter.blocks.append(
                {document::BlockType::SceneBreak, trimmed, {path, line.start, line.end}});
        } else {
            // This narrative dialect deliberately does not execute HTML, fetch images,
            // or interpret fenced code as ordinary prose.
            if (trimmed.startsWith(QLatin1Char('<')) || trimmed.startsWith(QStringLiteral("![")) ||
                trimmed.startsWith(QStringLiteral("```")) ||
                trimmed.startsWith(QStringLiteral("~~~"))) {
                return MarkdownSourceError{
                    MarkdownSourceErrorCode::InvalidMarkdown,
                    QStringLiteral("Unsupported narrative markup in %1 at byte %2")
                        .arg(path)
                        .arg(line.start)};
            }
            if (paragraphStart < 0) {
                paragraphStart = line.start;
            }
            paragraphEnd = line.end;
        }
    }
    flush();
    if (chapter.blocks.isEmpty()) {
        return MarkdownSourceError{MarkdownSourceErrorCode::EmptyChapter,
                                   QStringLiteral("Empty chapter: %1").arg(path)};
    }
    return std::nullopt;
}

} // namespace

MarkdownSourceResult MarkdownSourceParser::parseDirectory(QStringView path,
                                                          const MarkdownSourceOptions& options) {
    QFileInfo input(path.toString());
    if (input.isFile() && input.fileName() == QStringLiteral("SUMMARY.md")) {
        input = QFileInfo(input.absolutePath());
    }
    const auto root = input.canonicalFilePath();
    if (!input.isDir() || root.isEmpty()) {
        return MarkdownSourceError{
            MarkdownSourceErrorCode::InvalidRoot,
            QStringLiteral("Choose a source directory containing SUMMARY.md.")};
    }
    const QDir directory(root);
    const auto safePath = [&](const QString& relative) -> QString {
        if (relative.isEmpty() || relative.contains(QLatin1Char('\\')) ||
            relative.contains(QLatin1Char(':')) || relative.contains(QLatin1Char('%')) ||
            relative.contains(QLatin1Char('#')) || relative.contains(QLatin1Char('?')) ||
            QDir::isAbsolutePath(relative) ||
            relative.split(QLatin1Char('/')).contains(QStringLiteral("..")) ||
            !relative.endsWith(QStringLiteral(".md"), Qt::CaseInsensitive)) {
            return {};
        }
        QFileInfo target(directory.filePath(relative));
        const auto canonical = target.canonicalFilePath();
        const auto contained = directory.relativeFilePath(canonical);
        if (!target.isFile() || canonical.isEmpty() || QDir::isAbsolutePath(contained) ||
            contained == QStringLiteral("..") || contained.startsWith(QStringLiteral("../"))) {
            return {};
        }
        return canonical;
    };
    const auto summaryPath = safePath(QStringLiteral("SUMMARY.md"));
    if (summaryPath.isEmpty()) {
        return MarkdownSourceError{MarkdownSourceErrorCode::UnsafePath,
                                   QStringLiteral("Missing or unsafe SUMMARY.md.")};
    }
    auto summaryRead = readUtf8(summaryPath);
    if (const auto* error = std::get_if<MarkdownSourceError>(&summaryRead)) {
        return *error;
    }
    const auto summary = std::get<QByteArray>(summaryRead);
    QList<Entry> entries;
    QSet<QString> seen;
    qsizetype chapterIndent = -1;
    static const QRegularExpression link(
        QStringLiteral("^( *)(?:[-*+]) +\\[([^\\]]+)\\]\\(([^)]+)\\) *$"));
    for (const auto& line : lines(summary)) {
        if (line.text.trimmed().isEmpty() || !headingText(line.text).isEmpty()) {
            continue;
        }
        const auto match = link.match(line.text);
        if (!match.hasMatch() || match.captured(1).size() > 4) {
            return MarkdownSourceError{
                MarkdownSourceErrorCode::InvalidSummary,
                QStringLiteral("Unsupported SUMMARY.md entry at byte %1.").arg(line.start)};
        }
        const auto relative = QDir::cleanPath(match.captured(3));
        const auto indent = match.captured(1).size();
        if (indent > 0) {
            if (chapterIndent >= 0 && chapterIndent != indent) {
                return MarkdownSourceError{
                    MarkdownSourceErrorCode::InvalidSummary,
                    QStringLiteral("Only one chapter nesting level is supported.")};
            }
            chapterIndent = indent;
        }
        const auto canonical = safePath(match.captured(3));
        if (canonical.isEmpty()) {
            return MarkdownSourceError{
                MarkdownSourceErrorCode::UnsafePath,
                QStringLiteral("Missing or unsafe linked file: %1").arg(match.captured(3))};
        }
#ifdef Q_OS_WIN
        const auto identity = canonical.toCaseFolded();
#else
        const auto identity = canonical;
#endif
        if (seen.contains(identity)) {
            return MarkdownSourceError{MarkdownSourceErrorCode::DuplicatePath,
                                       QStringLiteral("Duplicate linked file: %1").arg(relative)};
        }
        seen.insert(identity);
        entries.append({match.captured(2), relative, !match.captured(1).isEmpty()});
    }

    QByteArray manifest("loreforge-markdown-source-v1");
    appendManifest(manifest, QStringLiteral("SUMMARY.md"), summary);
    auto title = options.title.trimmed();
    // Read only a simple top-level title from the site's optional home front matter.
    // Site layout, HTML hero text, configs and assets never become narrative blocks.
    const auto home = safePath(QStringLiteral("index.md"));
    if (title.isEmpty() && !home.isEmpty()) {
        auto homeRead = readUtf8(home);
        if (const auto* error = std::get_if<MarkdownSourceError>(&homeRead)) {
            return *error;
        }
        const auto homeBytes = std::get<QByteArray>(homeRead);
        const auto homeLines = lines(homeBytes);
        if (!homeLines.isEmpty() && homeLines.first().text.trimmed() == QStringLiteral("---")) {
            for (qsizetype index = 1; index < homeLines.size(); ++index) {
                const auto& line = homeLines.at(index).text;
                if (line.trimmed() == QStringLiteral("---")) {
                    break;
                }
                if (line.startsWith(QStringLiteral("title:"))) {
                    title = line.sliced(6).trimmed();
                    if (title.size() >= 2 && ((title.startsWith('"') && title.endsWith('"')) ||
                                              (title.startsWith('\'') && title.endsWith('\'')))) {
                        title = title.sliced(1, title.size() - 2);
                    }
                    break;
                }
            }
        }
        appendManifest(manifest, QStringLiteral("index.md"), homeBytes);
    }
    if (title.isEmpty()) {
        title = directory.dirName();
    }
    const auto bookId = core::BookId::fromStableKey(QStringLiteral("markdown-source:") + root);
    MarkdownSourceImport imported{
        {bookId,
         {title, options.authors, options.language, QStringLiteral("markdown-source"), root, {}},
         {}},
        0,
        {}};
    QString volume;
    for (qsizetype index = 0; index < entries.size(); ++index) {
        const auto& entry = entries.at(index);
        if (!entry.nested) {
            volume.clear();
            const bool hasChildren = index + 1 < entries.size() && entries.at(index + 1).nested;
            if (!hasChildren) {
                imported.excludedPaths.append(entry.path);
                continue;
            }
            if (QFileInfo(entry.path).fileName() != QStringLiteral("index.md")) {
                return MarkdownSourceError{
                    MarkdownSourceErrorCode::InvalidSummary,
                    QStringLiteral("A volume must link to its index.md: %1").arg(entry.path)};
            }
            volume = entry.title;
            ++imported.volumeCount;
        } else if (volume.isEmpty()) {
            return MarkdownSourceError{
                MarkdownSourceErrorCode::InvalidSummary,
                QStringLiteral("Chapter without a volume: %1").arg(entry.path)};
        }
        auto read = readUtf8(safePath(entry.path));
        if (const auto* error = std::get_if<MarkdownSourceError>(&read)) {
            return *error;
        }
        const auto bytes = std::get<QByteArray>(read);
        appendManifest(manifest, entry.path, bytes);
        if (!entry.nested) {
            continue;
        }
        document::Chapter chapter{
            core::ChapterId::fromStableKey(bookId.toString() + QLatin1Char(':') + entry.path),
            imported.document.chapters.size(),
            volume + QStringLiteral(" / ") + entry.title,
            {}};
        if (const auto error = parseBlocks(chapter, entry.path, bytes)) {
            return *error;
        }
        imported.document.chapters.append(std::move(chapter));
    }
    imported.document.metadata.sourceHash = core::ContentHash::sha256(QByteArrayView(manifest));
    const auto validation = document::validateDocument(imported.document);
    if (!validation.isValid()) {
        return MarkdownSourceError{
            MarkdownSourceErrorCode::InvalidDocument,
            QStringLiteral("Invalid source document: %1").arg(validation.errors.first().message)};
    }
    return imported;
}

} // namespace loreforge::parser
