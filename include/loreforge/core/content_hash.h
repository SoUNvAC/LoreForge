#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QString>
#include <QStringView>

#include <optional>

namespace loreforge::core {

class ContentHash final {
  public:
    ContentHash() = default;

    [[nodiscard]] static ContentHash sha256(QByteArrayView content);
    [[nodiscard]] static ContentHash sha256(QStringView content);
    [[nodiscard]] static std::optional<ContentHash> fromHex(QStringView hexadecimal);

    [[nodiscard]] bool isValid() const noexcept;
    [[nodiscard]] QString toHex() const;
    [[nodiscard]] const QByteArray& bytes() const noexcept;

    friend bool operator==(const ContentHash&, const ContentHash&) = default;

  private:
    explicit ContentHash(QByteArray bytes);

    QByteArray bytes_;
};

size_t qHash(const ContentHash& contentHash, size_t seed = 0) noexcept;

} // namespace loreforge::core
