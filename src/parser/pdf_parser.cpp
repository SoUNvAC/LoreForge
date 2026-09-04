#include "loreforge/parser/pdf_parser.h"

#include "loreforge/core/content_hash.h"
#include "loreforge/core/identifier.h"
#include "loreforge/document/document_validation.h"
#include "loreforge/text/chapter_heading_detector.h"
#include "loreforge/text/text_normalizer.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QPdfDocument>
#include <QPdfSelection>
#include <QRectF>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <ranges>
#include <utility>

namespace loreforge::parser {
namespace {

constexpr qsizetype kMaximumSourceBytes = 512 * 1024 * 1024;
constexpr int kMaximumPages = 5'000;
constexpr double kSingleColumnConfidence = 0.92;
constexpr double kTwoColumnConfidence = 0.82;
constexpr double kManualConfidence = 0.98;

struct Word final {
    QString text;
    QRectF bounds;
};

struct PageLine final {
    QString text;
    QRectF bounds;
};

struct ExtractedPage final {
    qsizetype index = -1;
    QList<PageLine> lines;
    double confidence = 0.0;
};

PdfParseError error(PdfParseErrorCode code, QString message, qsizetype pageIndex = -1) {
    return {code, std::move(message), pageIndex};
}

QString normalizedLine(QStringView line) {
    return text::TextNormalizer::normalizeBlockLine(line).simplified();
}

bool isPageNumber(const PageLine& line, qreal pageHeight) {
    static const QRegularExpression number(QStringLiteral(R"(^\s*(?:Page\s+)?\d+\s*$)"),
                                           QRegularExpression::CaseInsensitiveOption);
    return line.bounds.center().y() > pageHeight * 0.9 && number.matchView(line.text).hasMatch();
}

std::variant<QList<PageLine>, PdfParseError> geometricLines(QPdfDocument& pdf, int pageIndex) {
    const auto allText = pdf.getAllText(pageIndex);
    if (!allText.isValid() || allText.text().trimmed().isEmpty()) {
        return error(
            PdfParseErrorCode::NoExtractableText,
            QStringLiteral("Page %1 has no extractable text; OCR or an explicit skip is required.")
                .arg(pageIndex + 1),
            pageIndex);
    }

    QList<Word> words;
    static const QRegularExpression nonWhitespace(QStringLiteral(R"(\S+)"));
    auto matches = nonWhitespace.globalMatch(allText.text());
    while (matches.hasNext()) {
        const auto match = matches.next();
        const auto selection =
            pdf.getSelectionAtIndex(pageIndex, match.capturedStart(), match.capturedLength());
        const auto bounds = selection.boundingRectangle();
        if (!selection.isValid() || !bounds.isValid() || bounds.isEmpty() ||
            !std::isfinite(bounds.x()) || !std::isfinite(bounds.y())) {
            return error(PdfParseErrorCode::AmbiguousReadingOrder,
                         QStringLiteral("Page %1 has text without usable geometric bounds.")
                             .arg(pageIndex + 1),
                         pageIndex);
        }
        words.append({match.captured(), bounds});
    }
    if (words.isEmpty()) {
        return error(PdfParseErrorCode::NoExtractableText,
                     QStringLiteral("Page %1 has no extractable words.").arg(pageIndex + 1),
                     pageIndex);
    }

    std::sort(words.begin(), words.end(), [](const Word& left, const Word& right) {
        if (std::abs(left.bounds.center().y() - right.bounds.center().y()) > 2.0) {
            return left.bounds.center().y() < right.bounds.center().y();
        }
        return left.bounds.left() < right.bounds.left();
    });

    QList<QList<Word>> grouped;
    for (const auto& word : words) {
        auto destination = grouped.end();
        auto bestDistance = std::numeric_limits<qreal>::max();
        for (auto candidate = grouped.begin(); candidate != grouped.end(); ++candidate) {
            const auto candidateCenter = candidate->first().bounds.center().y();
            const auto tolerance = qMax<qreal>(3.0, word.bounds.height() * 0.45);
            const auto distance = std::abs(candidateCenter - word.bounds.center().y());
            if (distance <= tolerance && distance < bestDistance) {
                destination = candidate;
                bestDistance = distance;
            }
        }
        if (destination == grouped.end()) {
            grouped.append({word});
        } else {
            destination->append(word);
        }
    }

    QList<PageLine> lines;
    const auto segmentGap = qMax<qreal>(24.0, pdf.pagePointSize(pageIndex).width() * 0.06);
    for (auto& group : grouped) {
        std::sort(group.begin(), group.end(), [](const Word& left, const Word& right) {
            return left.bounds.x() < right.bounds.x();
        });
        QStringList pieces;
        QRectF bounds;
        const auto flushSegment = [&lines, &pieces, &bounds] {
            const auto line = normalizedLine(pieces.join(QLatin1Char(' ')));
            if (!line.isEmpty()) {
                lines.append({line, bounds});
            }
            pieces.clear();
            bounds = {};
        };
        for (const auto& word : group) {
            if (!bounds.isNull() && word.bounds.left() - bounds.right() > segmentGap) {
                flushSegment();
            }
            pieces.append(word.text);
            bounds = bounds.isNull() ? word.bounds : bounds.united(word.bounds);
        }
        flushSegment();
    }
    return lines;
}

QList<qreal> horizontalLanes(const QList<PageLine>& lines, qreal pageWidth) {
    QList<qreal> starts;
    for (const auto& line : lines) {
        if (text::ChapterHeadingDetector::detect(line.text).has_value() ||
            line.bounds.width() > pageWidth * 0.7) {
            continue;
        }
        starts.append(line.bounds.left());
    }
    std::sort(starts.begin(), starts.end());

    QList<qreal> lanes;
    QList<int> counts;
    const auto tolerance = pageWidth * 0.12;
    for (const auto start : starts) {
        if (lanes.isEmpty() || start - lanes.last() > tolerance) {
            lanes.append(start);
            counts.append(1);
        } else {
            const auto count = counts.last();
            lanes.last() = (lanes.last() * count + start) / (count + 1);
            counts.last() = count + 1;
        }
    }
    return lanes;
}

std::variant<ExtractedPage, PdfParseError>
extractPage(QPdfDocument& pdf, int pageIndex, const std::optional<PdfPageCorrection>& correction) {
    const auto linesResult = geometricLines(pdf, pageIndex);
    if (std::holds_alternative<PdfParseError>(linesResult)) {
        return std::get<PdfParseError>(linesResult);
    }
    auto lines = std::get<QList<PageLine>>(linesResult);
    const auto pageSize = pdf.pagePointSize(pageIndex);
    lines.removeIf(
        [height = pageSize.height()](const auto& line) { return isPageNumber(line, height); });
    if (lines.isEmpty()) {
        return error(PdfParseErrorCode::NoExtractableText,
                     QStringLiteral("Page %1 contains only a page label.").arg(pageIndex + 1),
                     pageIndex);
    }

    auto order = correction.has_value() ? correction->readingOrder : PdfReadingOrder::Auto;
    auto confidence = correction.has_value() && order != PdfReadingOrder::Auto
                          ? kManualConfidence
                          : kSingleColumnConfidence;
    const auto lanes = horizontalLanes(lines, pageSize.width());
    if (order == PdfReadingOrder::Auto) {
        if (lanes.size() >= 3) {
            return error(PdfParseErrorCode::AmbiguousReadingOrder,
                         QStringLiteral("Page %1 has three or more plausible text lanes; add a "
                                        "manual reading-order correction.")
                             .arg(pageIndex + 1),
                         pageIndex);
        }
        if (lanes.size() == 2 && lanes.at(1) - lanes.at(0) > pageSize.width() * 0.25) {
            order = PdfReadingOrder::TwoColumnsLeftToRight;
            confidence = kTwoColumnConfidence;
        } else {
            order = PdfReadingOrder::SingleColumn;
        }
    }

    if (order == PdfReadingOrder::TwoColumnsLeftToRight) {
        const auto midpoint = pageSize.width() / 2.0;
        std::stable_sort(lines.begin(), lines.end(),
                         [midpoint](const auto& left, const auto& right) {
                             const auto leftLane = left.bounds.center().x() < midpoint ? 0 : 1;
                             const auto rightLane = right.bounds.center().x() < midpoint ? 0 : 1;
                             if (leftLane != rightLane) {
                                 return leftLane < rightLane;
                             }
                             if (std::abs(left.bounds.top() - right.bounds.top()) > 2.0) {
                                 return left.bounds.top() < right.bounds.top();
                             }
                             return left.bounds.left() < right.bounds.left();
                         });
    } else {
        std::stable_sort(lines.begin(), lines.end(), [](const auto& left, const auto& right) {
            if (std::abs(left.bounds.top() - right.bounds.top()) > 2.0) {
                return left.bounds.top() < right.bounds.top();
            }
            return left.bounds.left() < right.bounds.left();
        });
    }
    return ExtractedPage{pageIndex, std::move(lines), confidence};
}

std::variant<QHash<qsizetype, PdfPageCorrection>, PdfParseError>
validatedCorrections(const PdfImportOptions& options, int pageCount) {
    QHash<qsizetype, PdfPageCorrection> corrections;
    for (const auto& correction : options.pageCorrections) {
        const auto validOrder = correction.readingOrder == PdfReadingOrder::Auto ||
                                correction.readingOrder == PdfReadingOrder::SingleColumn ||
                                correction.readingOrder == PdfReadingOrder::TwoColumnsLeftToRight;
        if (correction.pageIndex < 0 || correction.pageIndex >= pageCount ||
            corrections.contains(correction.pageIndex) || !validOrder ||
            (correction.skip && (correction.readingOrder != PdfReadingOrder::Auto ||
                                 !correction.chapterTitle.trimmed().isEmpty()))) {
            return error(
                PdfParseErrorCode::InvalidCorrection,
                QStringLiteral("PDF page corrections must have unique, in-range page indices."),
                correction.pageIndex);
        }
        corrections.insert(correction.pageIndex, correction);
    }
    return corrections;
}

void beginChapter(document::Document& document, QString title) {
    const auto index = document.chapters.size();
    const auto key = document.id.toString() + QStringLiteral(":chapter:") + QString::number(index);
    document.chapters.append(
        {core::ChapterId::fromStableKey(key),
         index,
         title.trimmed().isEmpty() ? QStringLiteral("Untitled chapter") : title.trimmed(),
         {}});
}

void appendPages(document::Document& document, const QList<ExtractedPage>& pages,
                 const QHash<qsizetype, PdfPageCorrection>& corrections) {
    for (const auto& page : pages) {
        const auto correction = corrections.value(page.index);
        auto manualHeadingPending = false;
        if (!correction.chapterTitle.trimmed().isEmpty()) {
            beginChapter(document, normalizedLine(correction.chapterTitle));
            manualHeadingPending = true;
        }
        const auto sourceId = document.metadata.sourceLocator + QStringLiteral("#page=") +
                              QString::number(page.index + 1) +
                              QStringLiteral("&layer=extracted-text");
        qint64 byteCursor = 0;
        for (const auto& line : page.lines) {
            const auto encoded = line.text.toUtf8();
            const core::SourceSpan span{sourceId, byteCursor, byteCursor + encoded.size()};
            byteCursor = span.endByte + 1;
            const auto heading = text::ChapterHeadingDetector::detect(line.text);
            if (heading.has_value()) {
                if (!manualHeadingPending) {
                    beginChapter(document, *heading);
                }
                document.chapters.last().blocks.append(
                    {document::BlockType::Heading, *heading, span, page.confidence});
                manualHeadingPending = false;
                continue;
            }
            if (document.chapters.isEmpty()) {
                beginChapter(document, document.metadata.title);
            }
            document.chapters.last().blocks.append(
                {document::BlockType::Paragraph, line.text, span, page.confidence});
            manualHeadingPending = false;
        }
    }
}

PdfParseError loadError(QPdfDocument::Error pdfError) {
    if (pdfError == QPdfDocument::Error::IncorrectPassword ||
        pdfError == QPdfDocument::Error::UnsupportedSecurityScheme) {
        return error(
            PdfParseErrorCode::PasswordRequired,
            QStringLiteral(
                "The PDF is encrypted and cannot be imported without supported credentials."));
    }
    return error(PdfParseErrorCode::InvalidPdf,
                 QStringLiteral("The source is not a readable PDF document."));
}

} // namespace

PdfParseResult PdfParser::parse(QByteArrayView source, const PdfImportOptions& options) {
    const auto sourceLocator = options.sourceLocator.trimmed();
    if (sourceLocator.isEmpty()) {
        return error(PdfParseErrorCode::MissingSourceLocator,
                     QStringLiteral("A source locator is required for reproducible import."));
    }
    if (source.isEmpty() || source.size() > kMaximumSourceBytes) {
        return error(PdfParseErrorCode::ResourceLimitExceeded,
                     QStringLiteral("The PDF is empty or exceeds the 512 MiB import limit."));
    }

    QByteArray bytes(source.data(), source.size());
    QBuffer buffer(&bytes);
    if (!buffer.open(QIODevice::ReadOnly)) {
        return error(PdfParseErrorCode::ReadFailure,
                     QStringLiteral("The PDF bytes could not be opened."));
    }
    QPdfDocument pdf;
    pdf.load(&buffer);
    if (pdf.status() != QPdfDocument::Status::Ready) {
        return loadError(pdf.error());
    }
    if (pdf.pageCount() <= 0 || pdf.pageCount() > kMaximumPages) {
        return error(
            PdfParseErrorCode::ResourceLimitExceeded,
            QStringLiteral("The PDF has no pages or exceeds the 5,000-page import limit."));
    }
    const auto correctionsResult = validatedCorrections(options, pdf.pageCount());
    if (std::holds_alternative<PdfParseError>(correctionsResult)) {
        return std::get<PdfParseError>(correctionsResult);
    }
    const auto& corrections = std::get<QHash<qsizetype, PdfPageCorrection>>(correctionsResult);

    const auto metadataTitle = pdf.metaData(QPdfDocument::MetaDataField::Title).toString();
    const auto requestedTitle = normalizedLine(options.title);
    const auto title = !requestedTitle.isEmpty()
                           ? requestedTitle
                           : (!normalizedLine(metadataTitle).isEmpty()
                                  ? normalizedLine(metadataTitle)
                                  : QFileInfo(sourceLocator).completeBaseName());
    QStringList authors = options.authors;
    if (authors.isEmpty()) {
        const auto author =
            normalizedLine(pdf.metaData(QPdfDocument::MetaDataField::Author).toString());
        if (!author.isEmpty()) {
            authors.append(author);
        }
    }
    const auto bookId = core::BookId::fromStableKey(sourceLocator);
    document::Document document{
        bookId,
        {title.isEmpty() ? QStringLiteral("Untitled") : title, authors,
         options.language.trimmed().isEmpty() ? QStringLiteral("und") : options.language.trimmed(),
         QStringLiteral("pdf"), sourceLocator, core::ContentHash::sha256(source)},
        {},
    };

    QList<ExtractedPage> pages;
    for (int pageIndex = 0; pageIndex < pdf.pageCount(); ++pageIndex) {
        const auto correction = corrections.contains(pageIndex)
                                    ? std::optional<PdfPageCorrection>(corrections.value(pageIndex))
                                    : std::nullopt;
        if (correction.has_value() && correction->skip) {
            continue;
        }
        const auto page = extractPage(pdf, pageIndex, correction);
        if (std::holds_alternative<PdfParseError>(page)) {
            return std::get<PdfParseError>(page);
        }
        pages.append(std::get<ExtractedPage>(page));
    }
    appendPages(document, pages, corrections);
    const auto hasContent = std::ranges::any_of(
        document.chapters, [](const auto& chapter) { return !chapter.blocks.isEmpty(); });
    if (!hasContent) {
        return error(PdfParseErrorCode::NoExtractableText,
                     QStringLiteral("The PDF contains no pages selected for text import."));
    }
    const auto validation = document::validateDocument(document);
    if (!validation.isValid()) {
        return error(PdfParseErrorCode::InvalidDocument,
                     QStringLiteral("The imported document failed domain validation: %1")
                         .arg(validation.errors.first().message));
    }
    return document;
}

PdfParseResult PdfParser::parseFile(QStringView filePath, PdfImportOptions options) {
    const QFileInfo fileInfo(filePath.toString());
    if (!fileInfo.exists() || !fileInfo.isFile()) {
        return error(PdfParseErrorCode::FileNotFound,
                     QStringLiteral("The PDF file does not exist."));
    }
    QFile file(fileInfo.absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly)) {
        return error(PdfParseErrorCode::ReadFailure,
                     QStringLiteral("The PDF file could not be read: %1").arg(file.errorString()));
    }
    if (options.sourceLocator.trimmed().isEmpty()) {
        options.sourceLocator = QDir::cleanPath(fileInfo.absoluteFilePath());
    }
    const auto source = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        return error(PdfParseErrorCode::ReadFailure,
                     QStringLiteral("The PDF file could not be read completely: %1")
                         .arg(file.errorString()));
    }
    return parse(source, options);
}

} // namespace loreforge::parser
