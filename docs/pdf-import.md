# PDF import contract

Phase 5 imports text-bearing PDF files without treating PDF layout as equivalent to EPUB
structure. The parser uses Qt PDF 6.8.3 to obtain page text and character geometry, then builds a
`Document` through explicit reading-order and chapter heuristics.

## Input and failure boundary

The parser accepts immutable PDF bytes and a stable source locator, or reads a file and derives
the locator from its absolute path. It limits input to 512 MiB and 5,000 pages. Invalid,
unsupported, and password-protected files return typed errors.

A page with no usable text layer fails with `NoExtractableText`; OCR is deliberately not run or
simulated. A page with three or more plausible text lanes fails with
`AmbiguousReadingOrder`. These failures include the zero-based page index so difficult documents
remain visible to the caller instead of silently losing or scrambling text.

## Reading order

Words are located through their PDF character indices and geometric bounds. Nearby words form a
line, while large horizontal gaps preserve separate column fragments. Page labels in the bottom
margin are removed when they contain only a page number.

Automatic layout classification supports:

- single-column pages, ordered from top to bottom;
- clear two-column pages, read down the left column and then down the right column.

Single-column extraction records confidence `0.92`; an automatically detected two-column page
records `0.82`. These values describe structural extraction confidence, not factual confidence
in the book's content.

## Chapter and correction hooks

The deterministic chapter-heading detector recognizes chapter, book, part, prologue, and
epilogue lines, including its existing Chinese patterns. A document without a recognized heading
uses its metadata title as one chapter.

`PdfPageCorrection` lets a future UI or import profile:

- force single-column or left-to-right two-column order;
- assign a chapter title at a selected page;
- explicitly skip a known blank or non-narrative page.

Forced reading order records confidence `0.98`. Corrections must use unique, in-range zero-based
page indices. A manual chapter title supersedes the first detected heading boundary on that page
while retaining the heading as a block.

## Source mapping and persistence

The source hash is SHA-256 over the exact original PDF bytes. PDF glyph text does not generally
have a meaningful UTF-8 byte range inside the compressed source file, so block spans refer to a
deterministic derived page-text layer:

```text
<source locator>#page=<one-based page>&layer=extracted-text
```

Byte offsets address the UTF-8 line sequence after geometry ordering and normalization. The raw
PDF remains immutable and authoritative through its source hash.

`Block::extractionConfidence` is optional for source formats that do not require extraction
estimation and mandatory for PDF-produced blocks. Document JSON includes the field when present.
SQLite schema migration 002 adds a nullable, range-checked column so confidence survives project
round trips.

## Fixtures

Committed deterministic fixtures cover a two-page single-column book, content-stream order that
must be repaired into two-column reading order, an ambiguous three-lane page, and a raster-only
page that requires OCR. Golden tests check text, chapter boundaries, page source IDs, confidence,
raw source hashing, manual corrections, and visible failure codes.
