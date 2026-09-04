# EPUB import contract

Phase 4 imports EPUB 2 and EPUB 3 books into the same immutable `Document` model used by
plain-text sources. The parser accepts archive bytes plus a stable source locator, or reads a
file and derives the locator from its absolute path.

## Container boundary

The importer treats the EPUB as untrusted input. It never extracts archive entries to disk. It
requires the first ZIP local entry to be the uncompressed `mimetype` file with the exact
`application/epub+zip` payload, then resolves the package through
`META-INF/container.xml`.

Archive paths containing absolute roots, drive prefixes, backslashes, empty components, or dot
components are rejected. Duplicate paths and symbolic links are also rejected. The current
limits are 10,000 entries, 64 MiB for one uncompressed entry, and 512 MiB total uncompressed
content.

ZIP access is isolated behind the EPUB parser and currently uses the private Qt
`QZipReader` API. CI pins Qt 6.8.3. Keeping the dependency inside one translation unit makes it
possible to replace the adapter without changing the public parser contract.

## Package and navigation

The OPF manifest and linear spine determine which HTML resources are imported and their source
order. EPUB 3 navigation documents and EPUB 2 NCX files supply chapter titles and boundaries.
Nested navigation links retain their document order.

Navigation fragments can split one XHTML resource into several chapters. A following spine
resource with no navigation target is appended to the current chapter, which supports chapters
spread across multiple HTML files. When navigation is absent, every linear spine resource
becomes one chapter; its first heading is preferred as the title, followed by its filename.

Missing spine resources, unsafe references, missing navigation fragments, malformed XML, and
non-HTML linear spine items produce typed parser errors. These faults are handled by
`EpubParser`; the plain-text normalizer is not used to repair EPUB structure.

## XHTML extraction and provenance

XHTML must be UTF-8 and well-formed XML. DTD declarations are rejected. Headings, paragraphs,
list items, quotations, preformatted blocks, definition items, and horizontal rules become
document blocks. Inline text is collapsed to the document model's whitespace form, XML entities
are decoded, and image alternative text is retained. Scripts, styles, layout, CSS, and image
bytes are not imported as narrative blocks.

The book source hash is SHA-256 over the exact original EPUB bytes. Every imported block has a
half-open byte range into its original XHTML archive entry. Its source ID has the form
`<source locator>!/<archive path>`. The parser does not rewrite or store a modified source file.

## Regression fixtures

The fixture corpus covers:

- EPUB 3 nested navigation;
- multiple chapters in one XHTML file;
- one chapter spanning multiple XHTML files;
- missing navigation fallback;
- EPUB 2 NCX navigation;
- Unicode punctuation, inline footnote links, footnote content, images, and scene breaks;
- unsafe archive paths, malformed XHTML, and invalid mimetype data.

Tests package the source trees in memory and compare the imported semantic structure with
committed golden JSON files. They also verify deterministic re-import, exact source hashing, and
valid source provenance for every block.
