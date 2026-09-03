# Document JSON schema v1

`DocumentJsonCodec` persists the normalized internal document model. The schema is
versioned independently from the application version.

## Root

- `schema_version`: integer, currently `1`.
- `document`: the normalized document object.

## Document

- `id`: deterministic `BookId` (`book_` plus 32 lowercase SHA-256 hex characters).
- `content_hash`: SHA-256 of the canonical field stream described below.
- `metadata`: source and book metadata.
- `chapters`: ordered chapter array.

Chapter indices are contiguous and zero-based. Chapter IDs are unique and deterministic.

## Source spans

Every block carries a half-open `[start_byte, end_byte)` range into the immutable source
resource named by `source_id`. Offsets count UTF-8 bytes, not UTF-16 code units or visual
characters. Spans from the same source resource may touch but must not overlap.

## Hashes

`metadata.source_hash` is the SHA-256 digest of the immutable imported source bytes.
`document.content_hash` covers IDs, metadata, chapter order, block types, block text, and
source spans. The codec rejects a document when its stored content hash does not match the
decoded content.

Canonical hash fields are encoded as:

```text
field-name NUL utf8-byte-length ':' utf8-value LF
```

Fields are appended in model order. This representation is independent of JSON whitespace
and object-key ordering.
