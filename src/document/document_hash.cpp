#include "loreforge/document/document_hash.h"

#include <QCryptographicHash>

namespace loreforge::document {
namespace {

void appendField(QCryptographicHash& hash, QByteArrayView name, QByteArrayView value) {
    hash.addData(name);
    hash.addData(QByteArrayView("\0", 1));
    const auto length = QByteArray::number(value.size());
    hash.addData(QByteArrayView(length));
    hash.addData(QByteArrayView(":", 1));
    hash.addData(value);
    hash.addData(QByteArrayView("\n", 1));
}

void appendText(QCryptographicHash& hash, QByteArrayView name, QStringView value) {
    const auto utf8 = value.toString().toUtf8();
    appendField(hash, name, QByteArrayView(utf8));
}

void appendInteger(QCryptographicHash& hash, QByteArrayView name, qint64 value) {
    const auto encoded = QByteArray::number(value);
    appendField(hash, name, QByteArrayView(encoded));
}

} // namespace

core::ContentHash computeContentHash(const Document& document) {
    QCryptographicHash hash(QCryptographicHash::Sha256);

    appendText(hash, QByteArrayView("document.id"), document.id.toString());
    appendText(hash, QByteArrayView("metadata.title"), document.metadata.title);
    appendInteger(hash, QByteArrayView("metadata.authors.count"), document.metadata.authors.size());
    for (const auto& author : document.metadata.authors) {
        appendText(hash, QByteArrayView("metadata.author"), author);
    }
    appendText(hash, QByteArrayView("metadata.language"), document.metadata.language);
    appendText(hash, QByteArrayView("metadata.source_format"), document.metadata.sourceFormat);
    appendText(hash, QByteArrayView("metadata.source_locator"), document.metadata.sourceLocator);
    appendText(hash, QByteArrayView("metadata.source_hash"), document.metadata.sourceHash.toHex());

    appendInteger(hash, QByteArrayView("chapters.count"), document.chapters.size());
    for (const auto& chapter : document.chapters) {
        appendText(hash, QByteArrayView("chapter.id"), chapter.id.toString());
        appendInteger(hash, QByteArrayView("chapter.index"), chapter.index);
        appendText(hash, QByteArrayView("chapter.title"), chapter.title);
        appendInteger(hash, QByteArrayView("chapter.blocks.count"), chapter.blocks.size());

        for (const auto& block : chapter.blocks) {
            appendText(hash, QByteArrayView("block.type"), blockTypeToString(block.type));
            appendText(hash, QByteArrayView("block.text"), block.text);
            appendText(hash, QByteArrayView("block.source_id"), block.sourceSpan.sourceId);
            appendInteger(hash, QByteArrayView("block.start_byte"), block.sourceSpan.startByte);
            appendInteger(hash, QByteArrayView("block.end_byte"), block.sourceSpan.endByte);
        }
    }

    return *core::ContentHash::fromHex(QString::fromLatin1(hash.result().toHex()));
}

} // namespace loreforge::document
