#pragma once

#include <string_view>

namespace noteapp::core {

// Derleme anında CMake'teki proje sürümünden gelir; tek doğruluk kaynağı CMakeLists.txt'tir.
std::string_view version();

}  // namespace noteapp::core
