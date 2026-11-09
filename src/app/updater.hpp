#pragma once

#include "core/error.hpp"
#include "core/limits.hpp"
#include "core/version.hpp"
#include "net/http.hpp"

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <variant>

namespace noteapp::app {

// Sürüm denetimi ve doğrulanmış indirme (üç arayüz aynı çekirdek). HTTP bu fonksiyon üzerinden çağrılır:
// üretimde net::http_get, testlerde sahte cevap. Ağa çıkar: işçi iş parçacığında çağrılır.
using HttpGet =
    std::function<std::expected<net::HttpResponse, core::Error>(const net::HttpRequest&, std::stop_token)>;

struct UpdateSettings {
    core::UpdateLimits limits;  // config.limits.update
    // Karşılaştırılan sürüm; testler dışında derlenen sürümdür.
    std::string current_version = std::string(core::version());
};

struct ReleaseAsset {
    std::string name;        // "noteapp-1.2.3-x86_64.AppImage"
    std::string url;         // https://github.com/<repo>/releases/download/<tag>/<name>
    std::uint64_t size = 0;  // API'nin bildirdiği bayt

    bool operator==(const ReleaseAsset&) const = default;
};

enum class AssetKind { AppImage, WindowsZip };

// GitHub'daki son release; yalnız bütün kurallardan geçmiş hâli üretilir (check_latest).
struct ReleaseInfo {
    std::string version;  // "1.2.3"
    std::string tag;      // "v1.2.3" ya da "1.2.3"
    ReleaseAsset appimage;
    ReleaseAsset windows_zip;
    ReleaseAsset sums;  // SHA256SUMS

    const ReleaseAsset& asset(AssetKind kind) const {
        return kind == AssetKind::AppImage ? appimage : windows_zip;
    }
    bool operator==(const ReleaseInfo&) const = default;
};

// Release'in github.com sayfası (etiket check_latest'te doğrulandı): kendini güncelleyemeyen kurulumda
// kullanıcı paketi buradan indirir.
std::string release_page_url(const ReleaseInfo& release);

struct UpToDate {
    std::string latest_version;  // GitHub'daki son sürüm (eşit ya da eski)
    bool operator==(const UpToDate&) const = default;
};

// Ağ, rate limit, bozuk cevap ve kural ihlali: core::Error (Code::Io, anahtar `update.*`).
using UpdateCheck = std::expected<std::variant<UpToDate, ReleaseInfo>, core::Error>;

// Son release'i sorar. Yeni sürüm yalnız `tag_name` `v?X.Y.Z` ise ve current_version'dan KESİN büyükse
// döner (eşit ve eski sürüm UpToDate). `kind` varlığı ve SHA256SUMS adıyla tam eşleşmeli, indirme adresleri
// aynı release'in github.com adresi olmalıdır; aksi hata. Öbür platformun varlığına bakılmaz (ReleaseInfo'da
// boş kalır): eksikliği ya da bozukluğu bu kurulumun güncellemesini engellemez.
UpdateCheck check_latest(const HttpGet& get, const UpdateSettings& settings, AssetKind kind,
                         std::stop_token stop);

// SHA256SUMS'u, sonra paketi belleğe indirir ve SHA-256'yı karşılaştırır. Paketin satırı tam bir kez
// bulunmalıdır. Bildirilen boyut sınırı aşan dosya indirilmez. Uyuşmazlıkta bayt dönmez.
std::expected<std::string, core::Error> download_verified(const HttpGet& get, const ReleaseInfo& release,
                                                          AssetKind kind, const UpdateSettings& settings,
                                                          std::stop_token stop);

// Açılış denetiminin durumu: `<state_dir>/update-state.toml`.
struct UpdateState {
    std::optional<std::int64_t> last_check_unix;  // son denetimin zamanı (unix saniye)
    std::string last_seen_version;                // son denetimde görülen en yeni sürüm

    bool operator==(const UpdateState&) const = default;
};

// Açılışta denetim zamanı mı: `interval` 0 ise asla; hiç denetlenmediyse, aralık dolduysa ya da saat geri
// gittiyse (son denetim gelecekte) evet.
bool should_check_on_startup(std::chrono::system_clock::time_point now, const UpdateState& state,
                             std::chrono::milliseconds interval);

// Dosya yoksa, okunamazsa ya da bozuksa varsayılan (bir sonraki kayıt düzeltir; uyarı loglanır).
UpdateState load_update_state();
// Atomik ve yalnız sahibi okuyabilir (0600).
std::expected<void, core::Error> save_update_state(const UpdateState& state);

}  // namespace noteapp::app
