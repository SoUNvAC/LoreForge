#include "loreforge/core/content_hash.h"

#include <QCryptographicHash>
#include <QHashFunctions>

#include <utility>

namespace loreforge::core {

ContentHash::ContentHash(QByteArray bytes) : bytes_(std::move(bytes)) {}

ContentHash ContentHash::sha256(QByteArrayView content) {
    return ContentHash(QCryptographicHash::hash(content, QCryptographicHash::Sha256));
}

ContentHash ContentHash::sha256(QStringView content) {
    const auto utf8 = content.toString().toUtf8();
    return sha256(QByteArrayView(utf8));
}

std::optional<ContentHash> ContentHash::fromHex(QStringView hexadecimal) {
    if (hexadecimal.size() != 64) {
        return std::nullopt;
    }

    const auto encoded = hexadecimal.toString().toLatin1();
    for (const auto character : encoded) {
        const bool decimal = character >= '0' && character <= '9';
        const bool lower = character >= 'a' && character <= 'f';
        const bool upper = character >= 'A' && character <= 'F';
        if (!decimal && !lower && !upper) {
            return std::nullopt;
        }
    }

    const auto bytes = QByteArray::fromHex(encoded);
    if (bytes.size() != QCryptographicHash::hashLength(QCryptographicHash::Sha256)) {
        return std::nullopt;
    }

    return ContentHash(bytes);
}

bool ContentHash::isValid() const noexcept {
    return bytes_.size() == QCryptographicHash::hashLength(QCryptographicHash::Sha256);
}

QString ContentHash::toHex() const {
    return QString::fromLatin1(bytes_.toHex());
}

const QByteArray& ContentHash::bytes() const noexcept {
    return bytes_;
}

size_t qHash(const ContentHash& contentHash, size_t seed) noexcept {
    return qHashBits(contentHash.bytes().constData(), contentHash.bytes().size(), seed);
}

} // namespace loreforge::core
