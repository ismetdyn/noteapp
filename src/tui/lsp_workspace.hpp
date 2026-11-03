#pragma once

#include "core/note_path.hpp"
#include "core/note_store.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>

namespace noteapp::tui {

// Bundan büyük not dil sunucusuna gitmez ve yansıtılmaz (plan T59).
inline constexpr std::uintmax_t kMaxLspNoteBytes = std::uintmax_t{2} * 1024U * 1024U;

// state_dir()/lsp/workspace: sunucunun kökü; sunucusu tanımlı dillerin notları burada dosya olarak durur ki
// notlar arası import çözülsün.
std::filesystem::path lsp_workspace_dir();

// Notun LSP languageId'si: tam dosya adı (Dockerfile, Makefile, CMakeLists.txt) ya da büyük/küçük harf
// duyarsız uzantı; tanınmayan tipte boş. Env ve Properties sır taşır: asla eşlenmez.
std::string_view lsp_language_for(const core::NotePath& path);

// `<root>/<note>`; kök, kökün üstündeki lsp/ ya da yolun herhangi bir bileşeni bağsa (Windows'ta junction ve
// reparse point dahil) nullopt. NotePath doğrulanmıştır, bu yine de dizine konmuş bir bağın yazmayı dışarı
// taşımasını önler. Dönen yolun kökü weakly_canonical'dır.
std::optional<std::filesystem::path> mirror_file(const std::filesystem::path& root,
                                                 const core::NotePath& note);

// Sunucunun ayarını değiştiren ya da çalıştırdığı kodu sağlayan yol (rust-analyzer.toml, node_modules/…): hiç
// yansıtılmaz. Karşılaştırma, büyük/küçük harf duyarsız dosya sistemlerinin (APFS, NTFS) ASCII'ye denk
// saydığı harfleri de katlar: ſ, ı, İ, K (Kelvin) ve tam genişlik ASCII.
bool is_server_owned_path(const core::NotePath& note);

// Ağ yolunu açmak Windows'ta SMB bağlantısı kurar ve parola özetini (NTLM) karşıya verir; Linux ve macOS'te
// "//host/x" yerel bir yoldur. Kural bu yüzden varsayılanda yalnız Windows'ta uygulanır.
#ifdef _WIN32
inline constexpr bool kBlockNetworkPaths = true;
#else
inline constexpr bool kBlockNetworkPaths = false;
#endif

// Metnin herhangi bir yerinde, anahtar ya da dosya biçimi gözetmeksizin, ham hâliyle ve JSON kaçışları
// (\\, \/, \uXXXX) ile yüzde kodlaması çözülmüş hâliyle bir ağ yolu var mı: \\host\paylaşım ya da
// //host/paylaşım (önünde ':' yok, host'tan sonra tek ayırıcı ve ayırıcı olmayan bir karakter), ya da
// `file:` + iki eğik çizgi + `localhost` olmayan host, `file:` + dört ya da daha çok eğik çizgi. Bilinen
// yanlış pozitif: protokolsüz URL (//cdn.example.com/x.js).
bool references_network_path(std::string_view text);

// Yansıtmanın kendisi: iş parçacığı yok, testler doğrudan çağırır. Kök 0700, dosyalar 0600 ve atomik yazılır;
// listede olmayan, dili seçilmemiş ya da sınırı aşan her dosya silinir.
class WorkspaceMirror {
public:
    // İki yol dosya sisteminde aynı girdi mi; varsayılan fs::equivalent. Testler katlayan dosya sistemini
    // taklit eder.
    using SameEntry = std::function<bool(const std::filesystem::path&, const std::filesystem::path&)>;

    // `block_network_paths`: references_network_path'e takılan not yansıtılmaz.
    WorkspaceMirror(std::filesystem::path root, std::set<std::string> languages, SameEntry same_entry = {},
                    bool block_network_paths = kBlockNetworkPaths);

    // Durdurma isteğinde not başında keser; yarım kalan bir sonraki çağrıda tamamlanır. Eski dosyalar dahil
    // tamamlandıysa true; durdurulduysa, kök reddedildiyse ya da dizin açılamadıysa false.
    bool sync(const core::NoteStore& store, const std::stop_token& stop = {});

private:
    void remove_stale(const std::filesystem::path& root, const std::set<std::string>& expected);
    // Dosya sistemi yazılanı ya da açılan klasörlerden birini engellenen bir adla aynı sayıyor mu.
    bool resolves_to_denied(const std::filesystem::path& root, const std::filesystem::path& file) const;

    std::filesystem::path root_;
    std::set<std::string> languages_;
    SameEntry same_entry_;
    bool block_network_paths_;
    // Yazılan dosyanın notun hangi sürümünden geldiği: değişmeyen not yeniden okunmaz.
    std::map<std::string, core::Revision> written_;
    bool refused_ = false;  // kök ya da lsp/ bağ: oturum boyunca yansıtılmaz
};

// Yansıtmayı arka planda yürütür (WordTableBuilder kalıbı): kurulunca bir kez, sonra her `request`ten `idle`
// sonra. UI hiç beklemez. İlk tam eşitleme (eski dosyaların silinmesi dahil) bitince `ready` olur ve
// `on_ready` işçiden bir kez çağrılır: sunucular ondan önce başlamaz, önceki sürümün yansıttığı artık
// engellenen dosyaları göremez.
class LspWorkspace {
public:
    LspWorkspace(WorkspaceMirror mirror, core::NoteStore store, std::chrono::milliseconds idle,
                 std::function<void()> on_ready = {});
    ~LspWorkspace();
    LspWorkspace(const LspWorkspace&) = delete;
    LspWorkspace& operator=(const LspWorkspace&) = delete;

    void set_store(core::NoteStore store);
    void request();
    bool ready() const { return ready_.load(); }

private:
    void run(const std::stop_token& stop);

    WorkspaceMirror mirror_;  // yalnız işçi dokunur
    std::chrono::milliseconds idle_;
    std::function<void()> on_ready_;
    std::atomic<bool> ready_{false};
    std::mutex mutex_;
    std::condition_variable_any wake_;
    core::NoteStore store_;
    bool pending_ = true;
    std::chrono::steady_clock::time_point requested_at_{};
    std::jthread worker_;  // son üye: önce o durur
};

}  // namespace noteapp::tui
