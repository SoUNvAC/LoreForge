#include "loreforge/parser/markdown_source_parser.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    if (application.arguments().size() != 2) {
        QTextStream(stderr) << "Usage: LoreForgeMarkdownInspect <source-directory>\n";
        return 2;
    }
    const auto result =
        loreforge::parser::MarkdownSourceParser::parseDirectory(application.arguments().at(1));
    if (const auto* error = std::get_if<loreforge::parser::MarkdownSourceError>(&result)) {
        QTextStream(stderr) << error->message << '\n';
        return 1;
    }
    const auto& imported = std::get<loreforge::parser::MarkdownSourceImport>(result);
    qsizetype blocks = 0;
    for (const auto& chapter : imported.document.chapters) {
        blocks += chapter.blocks.size();
    }
    // Aggregate-only output: no novel text or source copies are produced.
    const QJsonObject report{
        {QStringLiteral("volumes"), imported.volumeCount},
        {QStringLiteral("chapters"), imported.document.chapters.size()},
        {QStringLiteral("blocks"), blocks},
        {QStringLiteral("excludedEntries"), imported.excludedPaths.size()},
        {QStringLiteral("sourceHash"), imported.document.metadata.sourceHash.toHex()}};
    QTextStream(stdout) << QJsonDocument(report).toJson(QJsonDocument::Compact) << '\n';
    return 0;
}
