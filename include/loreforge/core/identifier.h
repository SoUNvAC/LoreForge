#pragma once

#include <QByteArray>
#include <QCryptographicHash>
#include <QString>
#include <QStringView>

#include <optional>
#include <string_view>
#include <utility>

namespace loreforge::core {

struct ProjectIdTag {
    static constexpr std::string_view prefix = "project";
};

struct BookIdTag {
    static constexpr std::string_view prefix = "book";
};

struct ChapterIdTag {
    static constexpr std::string_view prefix = "chapter";
};

struct LLMRunIdTag {
    static constexpr std::string_view prefix = "llmrun";
};

template <typename Tag> class Identifier final {
  public:
    Identifier() = default;

    [[nodiscard]] static Identifier fromStableKey(QStringView stableKey) {
        QByteArray input(Tag::prefix.data(), static_cast<qsizetype>(Tag::prefix.size()));
        input.append('\0');
        input.append(stableKey.toString().toUtf8());

        const auto digest =
            QCryptographicHash::hash(input, QCryptographicHash::Sha256).toHex().left(32);
        QByteArray value(Tag::prefix.data(), static_cast<qsizetype>(Tag::prefix.size()));
        value.append('_');
        value.append(digest);
        return Identifier(QString::fromLatin1(value));
    }

    [[nodiscard]] static std::optional<Identifier> fromString(QStringView value) {
        const auto prefix =
            QString::fromLatin1(Tag::prefix.data(), static_cast<qsizetype>(Tag::prefix.size()));
        if (!value.startsWith(prefix + QLatin1Char('_')) || value.size() != prefix.size() + 33) {
            return std::nullopt;
        }

        const auto digest = value.sliced(prefix.size() + 1);
        for (const auto character : digest) {
            const bool decimal = character >= QLatin1Char('0') && character <= QLatin1Char('9');
            const bool hexadecimal = character >= QLatin1Char('a') && character <= QLatin1Char('f');
            if (!decimal && !hexadecimal) {
                return std::nullopt;
            }
        }

        return Identifier(value.toString());
    }

    [[nodiscard]] bool isValid() const noexcept {
        return !value_.isEmpty();
    }
    [[nodiscard]] const QString& toString() const noexcept {
        return value_;
    }

    friend bool operator==(const Identifier&, const Identifier&) = default;

  private:
    explicit Identifier(QString value) : value_(std::move(value)) {}

    QString value_;
};

template <typename Tag> size_t qHash(const Identifier<Tag>& identifier, size_t seed = 0) noexcept {
    return ::qHash(identifier.toString(), seed);
}

using ProjectId = Identifier<ProjectIdTag>;
using BookId = Identifier<BookIdTag>;
using ChapterId = Identifier<ChapterIdTag>;
using LLMRunId = Identifier<LLMRunIdTag>;

} // namespace loreforge::core
