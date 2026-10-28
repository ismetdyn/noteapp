#pragma once

#include "cli/args.hpp"
#include "core/config.hpp"

namespace noteapp::cli {

// Çıkış kodları (SPEC-cli "Çıkış kodları"). 4 (senkron) T27'de gelir.
inline constexpr int kExitOk = 0;
inline constexpr int kExitUser = 1;
inline constexpr int kExitUsage = 2;
inline constexpr int kExitInternal = 3;

// Komut gövdeleri düz fonksiyonlardır; hepsi çıkış kodunu döner, çıktıyı stdout/stderr'e yazar.
int run_add(const ParsedArgs& args, const core::Config& config);
int run_help();
int run_version();
int run_rm(const ParsedArgs& args, const core::Config& config);  // args.rest: YOL [-y]
int run_mv(const ParsedArgs& args, const core::Config& config);  // args.rest: KAYNAK HEDEF
// Alt komut dağıtıcısı; gerçeklenmemiş olanlar "henüz kullanılamıyor" (çıkış 2) verir.
int run_reserved(const ParsedArgs& args, const core::Config& config);
int run_invalid(const ParsedArgs& args);

}  // namespace noteapp::cli
