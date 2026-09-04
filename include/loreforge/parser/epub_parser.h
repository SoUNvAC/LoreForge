#pragma once

#include "loreforge/document/document.h"

#include <QByteArrayView>
#include <QString>
#include <QStringList>
#include <QStringView>

#include <variant>

namespace loreforge::parser {

struct EpubImportOptions final {
    QString sourceLocator;
    QString title;
    QStringList authors;
    QString language;
};

enum class EpubParseErrorCode {
    MissingSourceLocator,
    FileNotFound,
    ReadFailure,
    InvalidContainer,
    UnsafeArchive,
    ResourceLimitExceeded,
    InvalidPackage,
    MissingManifestItem,
    InvalidNavigation,
    InvalidXhtml,
    EmptyInput,
    InvalidDocument,
};

struct EpubParseError final {
    EpubParseErrorCode code;
    QString message;

    friend bool operator==(const EpubParseError&, const EpubParseError&) = default;
};

using EpubParseResult = std::variant<document::Document, EpubParseError>;

class EpubParser final {
  public:
    [[nodiscard]] static EpubParseResult parse(QByteArrayView source,
                                               const EpubImportOptions& options);
    [[nodiscard]] static EpubParseResult parseFile(QStringView filePath,
                                                   EpubImportOptions options = {});
};

} // namespace loreforge::parser
