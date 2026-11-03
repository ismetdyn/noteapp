#pragma once

#include "core/config.hpp"

#include <cstddef>
#include <stop_token>

namespace noteapp::app {

// Açılış ipucunun sayısı (TUI ve GUI): geçişten sonra notes_dir'e düşen, grafta olmayan dosyalar. Bütçe
// limits.tui.stray_notice_files ve stray_notice_bytes; aşan kökte ipucu gecikmez, yalnız `note graph status`
// söyler. İşçide çağrılır: kendi depo bağlantısını açar (UI deposunun kilidini paylaşmaz), `stop` taramayı
// dosya başında keser. Hata ve istisna loglanır, 0 döner: ipucu vazgeçer, uygulama sürer.
std::size_t count_stray_files(const core::Config& config, const std::stop_token& stop) noexcept;

}  // namespace noteapp::app
