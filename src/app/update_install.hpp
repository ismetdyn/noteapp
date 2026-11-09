#pragma once

#include "app/updater.hpp"
#include "core/error.hpp"
#include "core/limits.hpp"

#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace noteapp::app {

// İndirilmiş ve SHA-256'sı doğrulanmış güncellemenin kurulumu (üç arayüz aynı çekirdek). Ağa çıkmaz; dosya
// sistemine yazar ve çocuk süreç başlatır: işçi iş parçacığında çağrılır. Güven modeli:
// .engineering/adr/0001-update-trust-model.md.
enum class InstallKind {
    DirectAppImage,  // AppImage doğrudan çalışıyor: $APPIMAGE dosyası yerinde değişir
    LocalInstall,    // install-local.sh düzeni (~/.local/share/noteapp/<sürüm>): paketteki betik kurar
    WindowsZip,      // sahiplik işaretli, yazılabilir zip dizini: üç exe, işaret ve licenses/ yer değiştirir
    Unsupported,     // kaynaktan derleme, sistem geneli kurulum, yazılamayan ya da işaretsiz dizin
};

// Windows zip'inin kökündeki sahiplik işareti (scripts/make-release.sh yazar). Yalnız bunu taşıyan dizin
// kurulum dizini sayılır: zip'in elle açıldığı ortak bir klasördeki (İndirilenler) kullanıcı dosyalarına
// dokunulmaz.
inline constexpr std::string_view kWindowsPackageMarker = "noteapp-package.txt";
// Geri alma yarıda kaldıysa exe dizinine yazılır: varken *.old yedekleri silinmez, yeni güncelleme kurulmaz.
inline constexpr std::string_view kRollbackFailedMarker = ".noteapp-rollback-failed";

struct InstallTarget {
    InstallKind kind = InstallKind::Unsupported;
    // DirectAppImage: $APPIMAGE; LocalInstall: çalışan sürümün dizini (<share>/<sürüm>, kanonik);
    // WindowsZip: exe dizini; Unsupported: boş.
    std::filesystem::path path;

    bool operator==(const InstallTarget&) const = default;
};

// Ortam okuyucu: core::get_env_utf8 ile aynı sözleşme (boş değer tanımsızdır). Testler sahte ortam verir.
using EnvReader = std::function<std::optional<std::string>(std::string_view)>;

// `exe`: çalışan ikilinin mutlak yolu (core::current_executable). Dosya sistemine bakar: düzen dosyaları var
// mı, hedef dizin yazılabilir mi (deneme dizini açılıp silinir).
InstallTarget detect_install_target(const std::filesystem::path& exe, const EnvReader& env);
// Çalışan süreç için: core::current_executable() ve süreç ortamı.
InstallTarget detect_install_target();

// Kurulum türünün indireceği varlık; Unsupported için nullopt.
std::optional<AssetKind> asset_kind(InstallKind kind);

struct InstallOutcome {
    // Windows: çalışan exe'ler yeniden başlayınca yenilenir; arayüz "yeniden başlatın" der. Linux'ta çalışan
    // süreç de eski sürümdür ama bir sonraki açılış yeniyi açar, bildirim gerekmez.
    bool restart_required = false;

    bool operator==(const InstallOutcome&) const = default;
};

using InstallResult = std::expected<InstallOutcome, core::Error>;

// `bytes`: download_verified'in döndürdüğü, `asset_kind(target.kind)` varlığı. Her çocuk süreç
// (install-local.sh, tar, note.exe --version) `limits.install_timeout`la sınırlıdır; `stop` ya da süre aşımı
// onu öldürür ("update.install_cancelled" / "update.install_timeout"). Değiştirme başladıktan sonra (Windows:
// ilk ad değişikliği; yerel kurulum: betiğin `committing` işareti) iptal yok sayılır. Unsupported: Error{Io,
// "update.unsupported_install", {url: release sayfası}}. Hata olursa kurulu sürüm değişmemiş kalır
// (Windows'ta geri alma da düşerse "update.rollback_failed" ve kRollbackFailedMarker).
InstallResult install_update(const InstallTarget& target, const ReleaseInfo& release, std::string_view bytes,
                             const core::UpdateLimits& limits, std::stop_token stop);

// Açılışta: Windows'ta önceki güncellemenin *.old yedekleri (kRollbackFailedMarker yoksa), her türde bayat
// `.noteapp-update-*` geçici dizinleri silinir. Kullanımdaki dosya (başka pencere hâlâ eski exe'yi
// çalıştırıyor) sessizce kalır.
void cleanup_after_update(const InstallTarget& target);

// Ağacı bağ izlemeden siler: sembolik bağ ve junction kendisi olarak gider, hedefine inilmez. Yoksa başarı.
std::expected<void, core::Error> remove_tree(const std::filesystem::path& path);

// --- Windows zip kurulumunun platformdan bağımsız çekirdeği (Linux'ta sahte işlemlerle test edilir) ---

// Arşiv ve dosya işlemleri. Üretimde windows_zip_ops(): tar.exe (sistem dizininden), MoveFileExW,
// remove_tree.
// Süreç çalıştıranlar `stop`ta ve süre aşımında çocuğu öldürüp hata döner.
struct ZipInstallOps {
    // Arşivdeki girdi adları (`tar -tf`), açmadan önce allowlist'ten geçer.
    std::function<std::expected<std::vector<std::string>, core::Error>(const std::filesystem::path& zip,
                                                                       std::stop_token stop)>
        list;
    // Arşivi var olan boş `destination` dizinine açar (`tar -xf zip -C destination`).
    std::function<std::expected<void, core::Error>(const std::filesystem::path& zip,
                                                   const std::filesystem::path& destination,
                                                   std::stop_token stop)>
        extract;
    // `<exe> --version` çıktısı.
    std::function<std::expected<std::string, core::Error>(const std::filesystem::path& exe,
                                                          std::stop_token stop)>
        version_output;
    // Ad değiştirme; hedef varsa başarısız olur (üzerine yazmaz).
    std::function<std::expected<void, core::Error>(const std::filesystem::path& from,
                                                   const std::filesystem::path& to)>
        rename;
    // Önceki güncellemenin yedeğini siler; çalışan exe silinemez (Windows) ve hata döner.
    std::function<std::expected<void, core::Error>(const std::filesystem::path& path)> remove;
};

#ifdef _WIN32
ZipInstallOps windows_zip_ops(const core::UpdateLimits& limits);
#endif

// `exe_dir`e kurar: `<exe_dir>/.noteapp-update-XXXXXXXX` içinde açar; allowlist (kökte yalnız note.exe,
// notes.exe, notes-gui.exe, kWindowsPackageMarker ve licenses/**; hepsi var; bağ/reparse point, `..`, sonu
// nokta ya da boşluk olan ad ve aygıt adı yok; her yol geçici dizinin içinde) ve `note.exe --version` ==
// "note <version>" denetlenir. Sonra her öğe için `x` → `x.old`, yeni → `x`; bir adım düşerse yapılanlar ters
// sırayla geri alınır. kRollbackFailedMarker varsa hiç başlamaz ("update.rollback_pending").
InstallResult install_windows_zip(const std::filesystem::path& exe_dir, const ReleaseInfo& release,
                                  std::string_view bytes, const ZipInstallOps& ops, std::stop_token stop);

}  // namespace noteapp::app
