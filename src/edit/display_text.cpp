#include "edit/display_text.hpp"

#include "core/i18n.hpp"
#include "core/text.hpp"

#include <array>
#include <chrono>
#include <ctime>
#include <format>

namespace noteapp::tui {

std::string count_text(std::string_view key, std::size_t count) {
    const std::string full = std::string(key) + (count == 1 ? "_one" : "");
    return core::tr(full, {{"count", std::to_string(count)}});
}

}  // namespace noteapp::tui
