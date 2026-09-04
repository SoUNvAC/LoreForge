#pragma once

#include "loreforge/document/document.h"

#include <QByteArrayView>
#include <QString>
#include <QStringList>
#include <QStringView>

#include <variant>

namespace loreforge::parser {

struct MobiImportOptions final {
    QString sourceLocator;
    QString title;
    QStringList authors;
    QString language;
};

enum class MobiParseErrorCode {
    MissingSourceLocator,
    FileNotFound,
    ReadFailure,
    InvalidContainer,
    InvalidHeader,
    UnsupportedFormat,
    UnsupportedCompression,
    UnsupportedEncoding,
    EncryptedDocument,
    ResourceLimitExceeded,
    CorruptText,
    EmptyInput,
    InvalidDocument,
};

struct MobiParseError final {
    MobiParseErrorCode code;
    QString message;

    friend bool operator==(const MobiParseError&, const MobiParseError&) = default;
};

using MobiParseResult = std::variant<document::Document, MobiParseError>;

class MobiParser final {
  public:
    [[nodiscard]] static MobiParseResult parse(QByteArrayView source,
                                               const MobiImportOptions& options);
    [[nodiscard]] static MobiParseResult parseFile(QStringView filePath,
                                                   MobiImportOptions options = {});
};

} // namespace loreforge::parser
