#include "loreforge/proofreading/protected_term_registry.h"

#include "proofreading_support.h"

#include <algorithm>
#include <utility>

namespace loreforge::proofreading {

ProtectedTermRegistry::ProtectedTermRegistry(QList<ProtectedTerm> terms)
    : terms_(std::move(terms)) {}

QList<core::SourceSpan>
ProtectedTermRegistry::protectedSpans(const ProofreadingSource& source) const {
    const auto decoded = detail::decodeSource(source);
    if (!decoded.has_value()) {
        return {};
    }
    QList<core::SourceSpan> result;
    for (const auto& term : terms_) {
        if (term.chapterScope.has_value() && *term.chapterScope != source.chapterId) {
            continue;
        }
        auto variants = term.allowedVariants;
        variants.prepend(term.canonicalSpelling);
        for (const auto& variant : variants) {
            for (const auto& [start, end] :
                 detail::literalMatches(*decoded, variant, Qt::CaseSensitive)) {
                result.append(detail::absoluteSpan(source, *decoded, start, end));
            }
        }
    }
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        if (left.startByte != right.startByte) {
            return left.startByte < right.startByte;
        }
        return left.endByte < right.endByte;
    });
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

bool ProtectedTermRegistry::protects(const ProofreadingSource& source,
                                     const core::SourceSpan& candidateSpan) const {
    for (const auto& protectedSpan : protectedSpans(source)) {
        if (candidateSpan.overlaps(protectedSpan)) {
            return true;
        }
        if (candidateSpan.startByte == candidateSpan.endByte &&
            candidateSpan.startByte > protectedSpan.startByte &&
            candidateSpan.startByte < protectedSpan.endByte) {
            return true;
        }
    }
    return false;
}

} // namespace loreforge::proofreading
