#pragma once

#include "loreforge/document/document.h"

#include <QByteArrayView>
#include <QString>
#include <QStringList>
#include <QStringView>

#include <variant>

namespace loreforge::parser {

struct PlainTextImportOptions final {
    QString sourceLocator;
    QString sourceId;
    QString title;
    QStringList authors;
    QString language = QStringLiteral("und");
};

enum class PlainTextParseErrorCode {
    MissingSourceLocator,
    FileNotFound,
    ReadFailure,
    InvalidUtf8,
    EmptyInput,
    InvalidDocument,
};

struct PlainTextParseError final {
    PlainTextParseErrorCode code;
    QString message;

    friend bool operator==(const PlainTextParseError&, const PlainTextParseError&) = default;
};

using PlainTextParseResult = std::variant<document::Document, PlainTextParseError>;

class PlainTextParser final {
  public:
    [[nodiscard]] static PlainTextParseResult parse(QByteArrayView source,
                                                    const PlainTextImportOptions& options);
    [[nodiscard]] static PlainTextParseResult parseFile(QStringView filePath,
                                                        PlainTextImportOptions options = {});
};

} // namespace loreforge::parser
