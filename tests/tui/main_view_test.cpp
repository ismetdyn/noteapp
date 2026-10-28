#include "tui/main_view.hpp"

#include <doctest/doctest.h>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>
#include <string>

TEST_CASE("main view renders Turkish note paths and content") {
    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(80), ftxui::Dimension::Fixed(15));
    ftxui::Render(screen, noteapp::tui::main_view());
    const std::string rendered = screen.ToString();

    CHECK(rendered.find("toplantı-notları.md") != std::string::npos);
    CHECK(rendered.find("çğıİöşü ÇĞIÖŞÜ") != std::string::npos);
}
