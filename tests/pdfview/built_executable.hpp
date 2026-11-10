#pragma once

#include "core/process.hpp"

#include <filesystem>
#include <string>
#include <system_error>

namespace noteapp::test {

// Derlemenin gömdüğü mutlak yol; orada yoksa (testler başka makineye kopyalandı: Win11 elle doğrulaması, SPEC-core.md
// "PDF yardımcı süreci") aynı adlı dosya test ikilisinin yanında aranır.
inline std::filesystem::path built_executable(const std::filesystem::path& built) {
    std::error_code ignored;
    if (std::filesystem::exists(built, ignored))
        return built;
    return core::current_executable().parent_path() / built.filename();
}

// argv için UTF-8 (core::ChildProcess argümanları UTF-8 alır; Türkçe karakterli kullanıcı dizini bozulmasın).
inline std::string built_executable_utf8(const std::filesystem::path& built) {
    const std::u8string path = built_executable(built).u8string();
    return {path.begin(), path.end()};
}

}  // namespace noteapp::test
