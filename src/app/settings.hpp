#pragma once

#include "core/config.hpp"
#include "core/error.hpp"

#include <expected>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace noteapp::app {

// Token kasası dikişi: testler gerçek OS kasasına dokunmaz, üretimde sync/auth.hpp'ye gider.
struct TokenStore {
    std::function<bool(std::string_view host)> has;
    std::function<std::expected<void, core::Error>(std::string_view host, std::string_view token)> store;
    std::function<std::expected<void, core::Error>(std::string_view host)> clear;
};
TokenStore system_token_store();

// "Senkron açıldı" akışı: depoyu kurar (SyncService::enable). `replace_origin` yalnız kullanıcı onayından
// sonra.
using SyncEnabler =
    std::function<std::expected<void, core::Error>(const core::Config& next, bool replace_origin)>;
SyncEnabler system_sync_enabler();

// Bağlantı testi (TUI ve GUI aynı akış): depoyu kurar ve tek senkron koşar. Ağa çıkar: işçi iş parçacığında
// çağrılır. origin değiştirilmez (uyuşmazlık sonuçtur, Kaydet sorar); config.toml'a ve kasaya yazmaz.
std::expected<void, core::Error> run_connection_test(const core::Config& candidate,
                                                     const SyncEnabler& enable);

// Açılışta gösterilecek config uyarıları: her biri format_error ile çevrilir, "; " ile birleşir; uyarı yoksa
// boş. Bozuk config.toml sessizce varsayılan dizine düşmesin diye kullanıcıya gösterilir.
std::string config_warnings_message(const std::vector<core::Error>& warnings);

// Çalışan AutoSync'in yeniden kurulması gerekir mi? Yalnız kurulurken okunan alanlar (etkin/remote/dal/boşta
// süresi/notes_dir) sayılır. Yazar adı/e-postasını `note sync --background` config'ten taze okur; editör gibi
// ilgisiz bir ayar için süren birleştirmeyi öldürmek gerekmez.
bool sync_restart_needed(const core::Config& before, const core::Config& after);

}  // namespace noteapp::app
