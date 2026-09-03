#include "loreforge/document/document_validation.h"

#include <QHash>
#include <QSet>

#include <utility>

namespace loreforge::document {
namespace {

void addError(DocumentValidationResult& result, DocumentValidationCode code, QString path,
              QString message) {
    result.errors.append({code, std::move(path), std::move(message)});
}

} // namespace

DocumentValidationResult validateDocument(const Document& document) {
    DocumentValidationResult result;

    if (!document.id.isValid()) {
        addError(result, DocumentValidationCode::InvalidBookId, QStringLiteral("$.id"),
                 QStringLiteral("Document ID is invalid."));
    }
    if (document.metadata.title.trimmed().isEmpty() ||
        document.metadata.sourceFormat.trimmed().isEmpty() ||
        document.metadata.sourceLocator.trimmed().isEmpty()) {
        addError(result, DocumentValidationCode::MissingMetadata, QStringLiteral("$.metadata"),
                 QStringLiteral("Title, source format, and source locator are required."));
    }
    if (!document.metadata.sourceHash.isValid()) {
        addError(result, DocumentValidationCode::InvalidSourceHash,
                 QStringLiteral("$.metadata.source_hash"),
                 QStringLiteral("Source hash must be a SHA-256 digest."));
    }
    if (document.chapters.isEmpty()) {
        addError(result, DocumentValidationCode::MissingChapters, QStringLiteral("$.chapters"),
                 QStringLiteral("A valid document contains at least one chapter."));
    }

    QSet<QString> chapterIds;
    QHash<QString, qint64> lastEndBySource;
    for (qsizetype chapterPosition = 0; chapterPosition < document.chapters.size();
         ++chapterPosition) {
        const auto& chapter = document.chapters.at(chapterPosition);
        const auto chapterPath = QStringLiteral("$.chapters[%1]").arg(chapterPosition);

        if (!chapter.id.isValid()) {
            addError(result, DocumentValidationCode::InvalidChapterId,
                     chapterPath + QStringLiteral(".id"), QStringLiteral("Chapter ID is invalid."));
        } else if (chapterIds.contains(chapter.id.toString())) {
            addError(result, DocumentValidationCode::DuplicateChapterId,
                     chapterPath + QStringLiteral(".id"),
                     QStringLiteral("Chapter IDs must be unique."));
        } else {
            chapterIds.insert(chapter.id.toString());
        }

        if (chapter.index != chapterPosition) {
            addError(result, DocumentValidationCode::InvalidChapterOrder,
                     chapterPath + QStringLiteral(".index"),
                     QStringLiteral("Chapter indices must be contiguous and zero-based."));
        }

        for (qsizetype blockPosition = 0; blockPosition < chapter.blocks.size(); ++blockPosition) {
            const auto& block = chapter.blocks.at(blockPosition);
            const auto blockPath = chapterPath + QStringLiteral(".blocks[%1]").arg(blockPosition);

            if (!block.sourceSpan.isValid()) {
                addError(result, DocumentValidationCode::InvalidSourceSpan,
                         blockPath + QStringLiteral(".source_span"),
                         QStringLiteral("Source span must be a valid UTF-8 byte range."));
            } else {
                const auto previousEnd = lastEndBySource.value(block.sourceSpan.sourceId, -1);
                if (previousEnd > block.sourceSpan.startByte) {
                    addError(result, DocumentValidationCode::OverlappingSourceSpan,
                             blockPath + QStringLiteral(".source_span"),
                             QStringLiteral("Block source spans must not overlap."));
                }
                lastEndBySource.insert(block.sourceSpan.sourceId,
                                       qMax(previousEnd, block.sourceSpan.endByte));
            }

            if (block.type != BlockType::SceneBreak && block.text.isEmpty()) {
                addError(result, DocumentValidationCode::EmptyBlockText,
                         blockPath + QStringLiteral(".text"),
                         QStringLiteral("Only scene-break blocks may have empty text."));
            }
        }
    }

    return result;
}

} // namespace loreforge::document
