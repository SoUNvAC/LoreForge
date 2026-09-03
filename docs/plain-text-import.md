# Plain-text import contract

Phase 2 imports `.txt` files into the Phase 1 `Document` model. The importer is
deterministic: the same bytes and import options produce the same identifiers,
chapters, blocks, hashes, and serialized document.

## Source integrity

- Input must be valid UTF-8. A leading UTF-8 BOM is accepted.
- `metadata.source_hash` is SHA-256 over the exact input bytes, including a BOM and
  original line endings.
- The importer never rewrites the source file.
- Every block span is a half-open byte range into those original UTF-8 bytes. Line
  terminators are excluded from single-line spans; a wrapped paragraph span includes
  the bytes between its first and last source lines.

## Normalization

Derived block text uses Unicode NFC. BOMs are removed, CRLF and CR become LF where
multiline text is normalized, and outer whitespace is trimmed. Consecutive nonblank
source lines form one paragraph joined by a single space. Blank lines end paragraphs.

## Structure detection

English `Chapter`, `Book`, and `Part` headings with numeric, Roman-numeral, or
alphabetic ordinals are recognized case-insensitively, as are `Prologue` and
`Epilogue`. Chinese `第…章/节/回/卷/部` headings (including space-separated titles),
`序章`, `楔子`, and `尾声` are also recognized. `***`, `* * *`, `---`, and equivalent
spaced forms are scene breaks.

Text before the first detected heading becomes a chapter titled with the imported book
title. Consecutive headings are retained as heading-only chapters; no source content is
invented or discarded.

## Word count

Letters and numbers form words, with internal straight or curly apostrophes retained.
Each CJK ideograph counts as one word. Punctuation and scene breaks do not count. The UI
counts paragraph text only, so chapter headings do not inflate the displayed total.
