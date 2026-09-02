#pragma once

#include <string_view>

namespace loreforge::core {

class ApplicationConfig final {
  public:
    static constexpr std::string_view applicationName() noexcept {
        return "LoreForge";
    }
    static constexpr std::string_view organizationName() noexcept {
        return "LoreForge";
    }
    static constexpr std::string_view organizationDomain() noexcept {
        return "loreforge.local";
    }

    static void applyMetadata();
};

} // namespace loreforge::core
