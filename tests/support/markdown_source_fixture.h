#pragma once

#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace loreforge::test {

inline bool writeMarkdownFixtureFile(const QString& root, const QString& relative,
                                     const QByteArray& bytes) {
    const auto path = QDir(root).filePath(relative);
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        return false;
    }
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

inline QByteArray markdownFixtureSummary() {
    return QStringLiteral("# 目录\n\n- [关于](about.md)\n- [第二卷](02/index.md)\n"
                          "  - [后写的章节](02/010.md)\n  - [先写的章节](02/002.md)\n"
                          "- [第一卷](01/index.md)\n  - [番外](01/001.md)\n"
                          "- [插画](illustrations.md)\n")
        .toUtf8();
}

inline QByteArray markdownFixtureChapter() {
    return QByteArray("\xEF\xBB\xBF", 3) +
           QStringLiteral("---\r\ntitle: 页面元数据\r\n---\r\n# 第十章\r\n\r\n"
                          "她打开了一扇门。\r\n窗外的灯还亮着。\r\n\r\n"
                          "## 第十一章不是新文件\r\n\r\n***\r\n\r\n他轻轻点头。\r\n")
               .toUtf8();
}

inline bool createMarkdownFixture(const QString& root) {
    // Entirely invented text. No external novel content belongs in test fixtures.
    return writeMarkdownFixtureFile(root, QStringLiteral("SUMMARY.md"), markdownFixtureSummary()) &&
           writeMarkdownFixtureFile(
               root, QStringLiteral("index.md"),
               QByteArray("---\nlayout: home\ntitle: \"Synthetic Novel\"\n---\n")) &&
           writeMarkdownFixtureFile(root, QStringLiteral("about.md"),
                                    QByteArray("<div>Site page</div>\n")) &&
           writeMarkdownFixtureFile(root, QStringLiteral("illustrations.md"),
                                    QByteArray("![Art](art.png)\n")) &&
           writeMarkdownFixtureFile(root, QStringLiteral("02/index.md"),
                                    QByteArray("# Volume Two\n")) &&
           writeMarkdownFixtureFile(root, QStringLiteral("01/index.md"),
                                    QByteArray("# Volume One\n")) &&
           writeMarkdownFixtureFile(root, QStringLiteral("02/010.md"), markdownFixtureChapter()) &&
           writeMarkdownFixtureFile(root, QStringLiteral("02/002.md"),
                                    QByteArray("# Two\n\nSecond file.\n")) &&
           writeMarkdownFixtureFile(root, QStringLiteral("01/001.md"),
                                    QByteArray("# One\n\nThird file.\n")) &&
           writeMarkdownFixtureFile(root, QStringLiteral("unlisted.md"),
                                    QByteArray("Not a chapter.\n"));
}

} // namespace loreforge::test
