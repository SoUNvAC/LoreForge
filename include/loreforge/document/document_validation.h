#pragma once

#include "loreforge/document/document.h"

#include <QList>
#include <QString>

namespace loreforge::document {

enum class DocumentValidationCode {
    InvalidBookId,
    MissingMetadata,
    InvalidSourceHash,
    MissingChapters,
    InvalidChapterId,
    DuplicateChapterId,
    InvalidChapterOrder,
    InvalidSourceSpan,
    OverlappingSourceSpan,
    EmptyBlockText,
};

struct DocumentValidationError final {
    DocumentValidationCode code;
    QString path;
    QString message;

    friend bool operator==(const DocumentValidationError&,
                           const DocumentValidationError&) = default;
};

struct DocumentValidationResult final {
    QList<DocumentValidationError> errors;

    [[nodiscard]] bool isValid() const noexcept {
        return errors.isEmpty();
    }
};

[[nodiscard]] DocumentValidationResult validateDocument(const Document& document);

} // namespace loreforge::document
