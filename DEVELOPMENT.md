# Development

## Phase discipline

Implement only the active phase. Before moving forward, complete the phase checklist in
`LoreForge_Engineering_Plan.md`, review interfaces, run tests, and establish a stable
commit or tag.

## Formatting

C++ files follow `.clang-format`. Static-analysis defaults are defined in `.clang-tidy`
and can be enabled at configure time with `LOREFORGE_ENABLE_CLANG_TIDY=ON`.

## Commits

Keep one conceptual change per commit. Use the repository owner's configured Git
identity. Do not add automated co-author or contributor trailers.
