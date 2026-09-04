#pragma once

#include "loreforge/document/document.h"

#include <QByteArrayView>
#include <QList>
#include <QString>
#include <QStringList>
#include <QStringView>

#include <variant>

namespace loreforge::parser {

enum class PdfReadingOrder {
    Auto,
    SingleColumn,
    TwoColumnsLeftToRight,
};

struct PdfPageCorrection final {
    qsizetype pageIndex = -1;
    PdfReadingOrder readingOrder = PdfReadingOrder::Auto;
    QString chapterTitle;
    bool skip = false;

    friend bool operator==(const PdfPageCorrection&, const PdfPageCorrection&) = default;
};

struct PdfImportOptions final {
    QString sourceLocator;
    QString title;
    QStringList authors;
    QString language;
    QList<PdfPageCorrection> pageCorrections;
};

enum class PdfParseErrorCode {
    MissingSourceLocator,
    FileNotFound,
    ReadFailure,
    ResourceLimitExceeded,
    InvalidPdf,
    PasswordRequired,
    InvalidCorrection,
    NoExtractableText,
    AmbiguousReadingOrder,
    InvalidDocument,
};

struct PdfParseError final {
    PdfParseErrorCode code;
    QString message;
    qsizetype pageIndex = -1;

    friend bool operator==(const PdfParseError&, const PdfParseError&) = default;
};

using PdfParseResult = std::variant<document::Document, PdfParseError>;

class PdfParser final {
  public:
    [[nodiscard]] static PdfParseResult parse(QByteArrayView source,
                                              const PdfImportOptions& options);
    [[nodiscard]] static PdfParseResult parseFile(QStringView filePath,
                                                  PdfImportOptions options = {});
};

} // namespace loreforge::parser
