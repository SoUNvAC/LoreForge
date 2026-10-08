# Maintained Markdown novel source

The primary desktop input is a read-only Markdown source directory with a
`SUMMARY.md` table of contents. The implementation targets the maintained novel
layout inspected locally, not an arbitrary Markdown website or every mdBook dialect.
External novel text and illustrations are never checked into LoreForge.

## Desktop workflow

1. Choose **File > Import Markdown Source** and select the source directory.
2. Choose a new `.loreforge` file outside that source directory.
3. The project opens in the existing reader and can be reopened with **Open Project**.

Import never writes to the source tree or overwrites an existing project. Parsing
finishes before database creation. The database is staged in a temporary destination
directory, closed, then renamed into place only after all persistence succeeds.
An import failure keeps the previous workspace usable. The stored project is an
import snapshot, not a live watch or an automatic synchronization mechanism.

## Supported structure

```text
src/
  SUMMARY.md
  index.md                optional home front matter with a simple title
  about.md                supplementary, excluded
  01/
    index.md              volume grouping, not a narrative chapter
    001.md                one file = one chapter
    002.md
  02/
    index.md
    001.md
  illustrations.md        supplementary, excluded
  .vitepress/             not scanned
```

```markdown
# Contents

- [About](about.md)
- [Volume One](01/index.md)
  - [Chapter One](01/001.md)
  - [Chapter Two](01/002.md)
- [Volume Two](02/index.md)
  - [Extra Story](02/001.md)
- [Illustrations](illustrations.md)
```

The TOC, not filenames or chapter numbers, determines sequence. Volume parents
must link to `index.md`; all chapter entries use one consistent indentation level
(one to four spaces). Unsupported nesting, malformed links, duplicate targets,
missing linked files, absolute/remote/encoded paths and paths escaping the source
tree fail explicitly. Canonical containment also rejects symlinks that escape the
tree. Top-level entries without children are reported as excluded supplements:
about, foreword, afterword and illustrations in the inspected layout. They do not
become model-analysis chapters. Unlisted files and site configuration are ignored.

The whole novel is one domain `Document`, preserving cross-volume chronological
chapter order for downstream story analysis. Each chapter's title retains
`Volume / Chapter`; its heading blocks retain the file's own headings. The current
chapter explorer is a flat chronological list, not a new volume-tree schema.

## Text and provenance contract

- Strict UTF-8, with optional BOM and LF/CRLF/CR lines.
- Leading YAML front matter is omitted from narrative blocks; an unclosed block fails.
- ATX headings (`#` through `######`), blank-separated paragraphs and common scene
  breaks (`---`, `***`, `* * *`, `___`) are recognized.
- Internal headings never split a linked chapter into additional chapters.
- Paragraph text retains original internal line endings. Source spans use original
  UTF-8 byte offsets, including BOM/front-matter displacement, and relative file IDs
  such as `01/001.md`. Heading display omits `#`; its source span still covers the
  original marked-up line. Do not infer original bytes from heading display text.
- Inline Markdown remains literal. This is a narrative-source importer, not a
  full Markdown renderer. HTML, standalone images and fenced code in narrative
  files fail explicitly; supplementary website/image pages are not parsed as prose.
  The reader escapes text and never executes source HTML or fetches source images.

Book identity is rooted in the canonical source directory; chapter identity is
rooted in that book and the TOC-relative file path. Inserting/reordering chapters or
editing their text does not change existing chapter IDs. Renaming/moving chapter
files or moving the entire source directory changes those identities.

`metadata.sourceHash` is SHA-256 of a versioned, length-framed manifest containing
the exact TOC, referenced volume indexes, narrative files, and optional home page
when it supplies the title. It is **not** the hash of a single concatenated text
file. Excluded supplement contents and unlisted files do not affect it. Existing
single-file Git diff/repair APIs still require a particular file's original bytes
and hash; do not pass this manifest hash as a file revision hash. Automatic
multi-file repair write-back and live Git source-tree synchronization are not
introduced by this importer.

## Verification

The desktop displays **总字数** (total characters) and **总汉字** (Han characters)
in the book summary, with both counts per chapter. Only paragraph blocks count;
chapter/volume titles, headings and scene breaks do not. Total characters count
Unicode letters and numbers individually; Han characters are that total's Unicode
`Script=Han` subset. Punctuation, symbols, emoji, whitespace and combining marks
do not count. Supplementary-plane Han characters count once, not as two UTF-16
code units. For example, `你好，world！2026` has total 11 and Han 2. Existing
word-based analysis/token-estimation APIs retain their separate word-count policy.

Synthetic fixtures cover TOC ordering, Chinese text, UTF-8 byte spans, BOM/CRLF,
front matter, repeated headings, stable IDs, content hashes, invalid inputs,
read-only import and persisted desktop reopening. They contain no external novel
content. A read-only command emits aggregate counts and a hash, never source text:

```powershell
.\build\windows-ninja-release\LoreForgeMarkdownInspect.exe <source-directory>
```

Local acceptance on the authorized maintained source returned 10 volumes,
743 chapters, 59,280 blocks and 4 excluded supplementary entries. This external
tree is not a test dependency and is not published to GitHub.
