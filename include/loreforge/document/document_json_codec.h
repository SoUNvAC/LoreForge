#pragma once

#include "loreforge/document/document.h"

#include <QByteArray>
#include <QString>

#include <variant>

namespace loreforge::document {

enum class DocumentJsonErrorCode {
    InvalidJson,
    UnsupportedSchema,
    MissingField,
    WrongType,
    InvalidValue,
    InvalidDocument,
    ContentHashMismatch,
};

struct DocumentJsonError final {
    DocumentJsonErrorCode code;
    QString path;
    QString message;

    friend bool operator==(const DocumentJsonError&, const DocumentJsonError&) = default;
};

using DocumentEncodeResult = std::variant<QByteArray, DocumentJsonError>;
using DocumentDecodeResult = std::variant<Document, DocumentJsonError>;

class DocumentJsonCodec final {
  public:
    static constexpr int schemaVersion = 1;

    [[nodiscard]] static DocumentEncodeResult encode(const Document& document);
    [[nodiscard]] static DocumentDecodeResult decode(QByteArrayView json);
};

} // namespace loreforge::document
