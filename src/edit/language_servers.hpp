#pragma once

#include "core/config.hpp"
#include "core/lsp_protocol.hpp"
#include "core/note_store.hpp"
#include "edit/editor_session.hpp"
#include "edit/lsp_client.hpp"
#include "edit/lsp_workspace.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace noteapp::tui {

// Sunucu konumlarını GÜNCEL tampona kısar: sunucu eski bir sürümü görmüş olabilir. Satır ve sütun tampon
// içine iner, ters aralık (start > end) başta boş aralığa döner; Unspecified önem hata sayılır.
std::vector<DiagnosticMark> diagnostic_marks(const EditorBuffer& buffer,
                                             std::span<const core::lsp::Diagnostic> diagnostics);

// `limits.lsp.*`: istemcinin, yansıtmanın ve editörden giden belgenin sınırları. Üye varsayılanları
// defaults.toml ile aynıdır (test denetler).
struct LspLimits {
    LspClientLimits client{};
    std::chrono::milliseconds initialize_timeout = LspClient::kDefaultInitializeTimeout;
    std::uintmax_t note_bytes = kDefaultLspNoteBytes;  // büyük not ne yansıtılır ne gönderilir

    bool operator==(const LspLimits&) const = default;
};
// Config'teki değerler; 0 (sınırsız) SIZE_MAX, süre milliseconds::max() olur.
LspLimits lsp_limits_of(const core::LspLimits& limits);

// TUI'nin dil sunucusu tarafı; yalnız UI iş parçacığından kullanılır. Ayar kapalıysa ya da sunucu yoksa
// hiçbir şey yapmaz: süreç başlamaz, çalışma alanı dizini oluşmaz. Programlar kurulurken bir kez PATH'in
// mutlak girdilerinde aranır: bulunamayan sunucu denenmez, hiçbir sunucusu bulunmayan dilin notu yansıtılmaz,
// dilin ilk notunda tek bir ileti çıkar. Bulunan diller için çalışma alanını arka planda yansıtır, editör bir
// notu açınca o dilin istemcilerini tembel başlatır. Bir dilin birden çok sunucusu (html + htmx-lsp) aynı
// belgeyi alır; tanılar birleşir ve sunucu adıyla etiketlenir, öneriler aynı listeye eklenir. Her sunucu
// gömülü ayarıyla (server_settings.hpp) ve kullanıcının [lsp.settings.<dil>] tablosuyla başlar.
class LanguageServers {
public:
    static constexpr std::chrono::milliseconds kChangeIdle{250};  // SpellChecker::kDefaultIdle ile aynı tempo
    static constexpr std::chrono::milliseconds kMirrorIdle{500};

    struct Update {
        bool wants_frame = false;  // değişiklik boşta bekliyor: süre dolunca gönderilsin diye yeni kare iste
        std::string status;        // durmuş sunucunun iletisi; her sunucu için bir kez
    };

    // `redraw` işçiden çağrılır: beklemez, istemciyi yıkmaz. Tekrarları burada birleşir: bir sonraki
    // update'e dek en çok bir kez çağrılır, tanı seli UI kuyruğunu büyütmez.
    LanguageServers(core::LspConfig config, std::filesystem::path workspace, core::NoteStore store,
                    std::function<void()> redraw, std::chrono::milliseconds change_idle = kChangeIdle,
                    std::chrono::milliseconds mirror_idle = kMirrorIdle,
                    bool block_network_paths = kBlockNetworkPaths, LspLimits limits = {});
    ~LanguageServers();
    LanguageServers(const LanguageServers&) = delete;
    LanguageServers& operator=(const LanguageServers&) = delete;

    // Notlar değişti (kayıt, taşıma, silme, çöp, yeniden tarama): yansıtma boşta yenilenir.
    void notes_changed();
    void set_store(core::NoteStore store);

    // Her çizimden önce: belgeyi açar/kapatır, boşta değişikliği gönderir, öneri ister/ekler, tanıları
    // editöre koyar.
    Update update(EditorSession& editor, std::chrono::steady_clock::time_point now);

    std::size_t started_clients() const {
        return static_cast<std::size_t>(
            std::ranges::count_if(clients_, [](const auto& entry) { return entry.second != nullptr; }));
    }

private:
    // (languageId, dilin sunucu listesindeki sıra)
    using ServerKey = std::pair<std::string, std::size_t>;

    struct OpenDocument {
        core::NotePath path;
        std::string uri;
        std::string language;
        std::int64_t version = 1;
        std::uint64_t seen_edits = 0;  // son görülen EditorBuffer::edit_count
        std::uint64_t sent_edits = 0;  // sunucuya giden metnin edit_count'u
        std::chrono::steady_clock::time_point changed_at{};
        std::uint64_t completion_generation = 0;
        std::uint64_t buffer_generation =
            0;  // EditorSession::buffer_generation; değişirse belge yeniden açılır
        std::map<std::size_t, LspClient::Ticket> completions;  // sunucu sırası -> bekleyen öneri bileti
    };

    bool enabled() const { return config_.enabled && (!config_.servers.empty() || !missing_.empty()); }
    LspClient* client_for(const ServerKey& key);
    // Belgenin dilinin durmamış istemcileri, sırayla.
    std::vector<std::pair<std::size_t, LspClient*>> live_clients(const std::string& language);
    void open_document(const EditorSession& editor, Update& out);
    void report_missing(const std::string& language, Update& out);
    // Metin uzak dosya başvurusu taşıyorsa true; not o hâliyle ilk kez engellenince iletiyi koyar. Sonuç yol,
    // tampon kuşağı ve edit_count başına bir kez hesaplanır: açılamayan not her karede 2 MiB taranmasın.
    // Kuşak şarttır: yeniden açılan notun yeni tamponu edit_count'u 0'dan sayar, senkronla değişmiş metin
    // eski temiz sonuçla gönderilirdi.
    bool blocked_by_remote_reference(const core::NotePath& path, std::uint64_t buffer_generation,
                                     const EditorBuffer& buffer, Update& out);
    void close_document();
    void sync_document(EditorSession& editor, std::chrono::steady_clock::time_point now, Update& out);
    void report_stops(Update& out);
    void notify();

    core::LspConfig config_;  // servers: yalnız programı bulunanlar, argv[0] mutlak yol
    // Programı bulunamayan sunucular: dil -> program adları. Ne süreç ne (dilin hiç sunucusu yoksa) yansıtma;
    // dilin ilk notunda bir kez söylenir.
    std::map<std::string, std::vector<std::string>> missing_;
    std::filesystem::path workspace_;
    std::chrono::milliseconds change_idle_;
    bool block_network_paths_;  // references_network_path'e takılan not ne yansıtılır ne gönderilir
    LspLimits limits_;
    std::function<void()> redraw_;
    // İstemcilerden önce bildirilir: onlardan sonra yıkılır, kapanıştaki geç bildirim geçerli üyeye düşer.
    std::atomic<bool> redraw_pending_{false};
    std::optional<LspWorkspace> mirror_;
    std::optional<OpenDocument> document_;
    struct RemoteScan {
        core::NotePath path;
        std::uint64_t buffer_generation = 0;
        std::uint64_t edits = 0;
        bool remote = false;
    };
    std::optional<RemoteScan> remote_scan_;
    std::set<std::string> reported_missing_;  // dil
    std::set<ServerKey> reported_stops_;
    // Son üye: ilk yıkılır; her istemci kendi işçisini durdurup sunucusunu kapatır.
    std::map<ServerKey, std::unique_ptr<LspClient>> clients_;
};

}  // namespace noteapp::tui
