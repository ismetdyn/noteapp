#pragma once

#include "core/config.hpp"
#include "core/error.hpp"
#include "core/note_store.hpp"

#include <expected>
#include <span>
#include <string_view>

namespace noteapp::sync {

// Son yol bileşeni çakışma kopyası adı mı: `<ad> (conflict YYYY-MM-DD HHMM)[ N][.<uzantı>]` (SPEC-sync döngü
// 4).
bool is_conflict_copy_name(std::string_view path);

// notes[i].sync_status'u doldurur. Ağa çıkmaz, kilit almaz, depoya yazmaz; ölçüt NoteMeta'daki bu cihazın
// yazma numarası ile durum dosyasındaki son gönderilen işlemdir (SPEC-sync).
//  - sync kapalı ya da notes_dir depo değil: hepsi sync_off;
//  - adı çakışma kopyası: conflict_copy (içerik uzakta olsa da, kullanıcı çözene dek);
//  - bu cihazda oluşturulup gönderilmemiş: new; gönderilmemiş yazması var: modified; aksi: synced.
std::expected<void, core::Error> fill_sync_status(const core::Config& config,
                                                  std::span<core::NoteMeta> notes);

}  // namespace noteapp::sync
