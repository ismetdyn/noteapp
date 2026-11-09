#pragma once

#include "core/lsp_protocol.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
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
#include <vector>

namespace noteapp::tui {

// İstemcinin sınırları: `limits.lsp.*` (lsp_limits_of). Sınırsız değer SIZE_MAX, süre milliseconds::max()
// (bekleme süresiz). Üye varsayılanları defaults.toml ile aynıdır, bir test denetler.
struct LspClientLimits {
    core::lsp::ProtocolLimits protocol{};
    // Ekleme metni bundan büyük öğe atlanır: sunucu her öneriye MiB'larca metin koyabilir.
    std::size_t completion_insert_bytes = std::size_t{8} * 1024U;
    // Etiketi bundan uzun öğe atlanır: popup tek satır gösterir, MiB'lık etiket yalnız bellek ve sıralama
    // yer.
    std::size_t completion_label_bytes = 1024U;
    // Sunucu isteklerine bekleyen cevapların toplamı; aşan sunucu protokol hatasıyla durdurulur. Meşru sunucu
    // birkaç küçük istek gönderir, istek yağdıran sunucu belleği büyütemesin.
    std::size_t pending_reply_bytes = std::size_t{1024} * 1024U;
    // Sunucu stdin'i bu süre okumazsa takılmış sayılır.
    std::chrono::milliseconds write_timeout{5'000};
    // Kapanışta shutdown cevabı; ardından çıkış için sabit 500 ms (LspClient::shut_down).
    std::chrono::milliseconds shutdown_timeout{1'000};

    bool operator==(const LspClientLimits&) const = default;
};

// Tek dil sunucusunun istemcisi; SpellChecker kalıbı. Süreç ilk açılan belgeyle başlar, nesneyle ölür. Genel
// yöntemlerin hepsi kilidi kısa tutup döner: G/Ç yalnız işçidedir, UI asla beklemez. Çökme, EOF, çerçeve
// hatası ya da zaman aşımında istemci oturum boyunca durur (yeniden başlatma yok). Sunucu metinleri (tanı
// mesajı, öneri etiketi) ham döner; ekrana basmadan önce temizlik UI katmanındadır.
// Sunucu süreci core::ChildProcess'tir (TUI ve GUI aynı istemciyi kullanır).
class LspClient {
public:
    // `limits.lsp.initialize_timeout` varsayılanı.
    static constexpr std::chrono::milliseconds kDefaultInitializeTimeout{10'000};

    using Ticket = std::uint64_t;

    struct Options {
        std::string language;              // yalnız log için
        std::vector<std::string> command;  // kabuksuz argv
        std::string root_uri;
        std::chrono::milliseconds initialize_timeout = kDefaultInitializeTimeout;
        std::vector<std::string> environment{};  // "AD=değer"; ChildProcess'in temiz ortamının üstüne
        std::filesystem::path working_directory{};  // boşsa bizimki
        std::string initialization_options{};    // JSON; boşsa initialize'da yok
        // workspace/configuration cevaplarının kaynağı; Null her öğeye null.
        core::Json settings{.kind = core::Json::Kind::Null};
        std::string configuration_change{};  // JSON; boş değilse initialized'dan hemen sonra gönderilir
        std::vector<std::string>
            notifications{};  // initialized'dan sonra parametresiz gönderilen bildirimler
        LspClientLimits limits{};
    };

    enum class StopReason {
        StartFailed,
        InitializeTimeout,
        InitializeFailed,
        Exited,         // EOF ya da okuma hatası: sunucu çöktü ya da kapandı
        ProtocolError,  // çerçeve hatası: akışın hizası kayboldu
        WriteFailed,    // sunucu stdin'i okumuyor ya da kapattı
        Shutdown        // yıkıcı; UI bunu görmez
    };

    // `notify` işçiden çağrılır (tanı, öneri ya da durma); kilit tutulmaz. Her publishDiagnostics için bir
    // kez çağrılır: ucuz olmalı (iş kuyruğa atılır, burada çizilmez) ve ardışık çağrıları birleştirmek UI'nin
    // işidir. Durum değişikliği bildirimden önce görünür olur: başka iş parçacığından yoklayan, değişikliği
    // bildirim gelmeden görebilir. TUI `app_.Post(redraw)` verir.
    LspClient(Options options, std::function<void()> notify);
    ~LspClient();
    LspClient(const LspClient&) = delete;
    LspClient& operator=(const LspClient&) = delete;

    // Tam metin eşitleme. Kuyruğa en son metin girer: işçi yetişemezse ara sürümler hiç gönderilmez. Sürüm
    // belge açıkken artmalıdır. Açık olmayan belgede `change` yok sayılır.
    void open(std::string uri, std::string language_id, std::string text, std::int64_t version);
    void change(const std::string& uri, std::string text, std::int64_t version);
    void close(const std::string& uri);

    // `position.character` bayt sütunudur. Belge açık değilse ya da istemci durduysa nullopt. Yeni istek
    // öncekini geçersiz kılar.
    std::optional<Ticket> request_completion(const std::string& uri, std::int64_t version,
                                             core::lsp::Position position);
    // Bilete gelen öneriler; bir kez verilir. Henüz gelmediyse, bilet eskidiyse ya da belge sürümü o arada
    // ilerlediyse nullopt.
    std::optional<std::vector<core::lsp::CompletionItem>> take_completion(Ticket ticket);

    struct DocumentDiagnostics {
        std::int64_t version = 0;  // tanıların yayınlandığı, aralıkların kısıldığı belge sürümü
        std::vector<core::lsp::Diagnostic> diagnostics;
    };
    // Kopyası. Aralıklarda `character` bayt sütunudur; satır ve sütun `version`daki metne kısılmıştır,
    // başlangıç sondan sonra gelmez. UI'nin güncel metni daha yeniyse aralıklar onun dışına düşebilir:
    // `version` güncel sürümden eskiyse çizmeden önce yeniden kısılmalıdır. Belge açık değilse ya da tanı
    // yoksa boş.
    DocumentDiagnostics document_diagnostics(const std::string& uri) const;
    // document_diagnostics(uri).diagnostics; sürüm sözleşmesi aynıdır.
    std::vector<core::lsp::Diagnostic> diagnostics(const std::string& uri) const;
    // Durduysa nedeni.
    std::optional<StopReason> stopped() const;

private:
    struct Session;
    struct Document {
        std::string language_id;
        std::string text;
        std::int64_t version = 0;
    };
    struct PendingCompletion {
        Ticket ticket = 0;
        std::string uri;
        std::int64_t version = 0;
        core::lsp::Position position;
    };
    struct CompletionResult {
        Ticket ticket = 0;
        std::string uri;
        std::int64_t version = 0;
        std::vector<core::lsp::CompletionItem> items;
    };

    // İşçi tarafı; nullopt "sürdür", değer "oturum bitti" demektir.
    void run(const std::stop_token& stop);
    std::optional<StopReason> initialize(Session& session);
    std::optional<StopReason> serve(Session& session);
    std::optional<StopReason> sync(Session& session);
    std::optional<StopReason> pump(Session& session, std::chrono::steady_clock::time_point deadline);
    std::optional<StopReason> receive(Session& session, std::string_view bytes);
    std::optional<StopReason> send(Session& session, std::string_view body,
                                   std::chrono::steady_clock::time_point deadline);
    void handle(Session& session, const core::Json& message);
    void store_diagnostics(Session& session, const core::Json& params);
    void deliver_completion(Session& session, const core::Json& response);
    void shut_down(Session& session);
    void finish(StopReason reason, std::optional<int> exit_code);

    Options options_;
    std::function<void()> notify_;
    mutable std::mutex mutex_;
    std::condition_variable_any wake_;
    std::map<std::string, Document> documents_;  // UI'nin istediği durum
    std::set<std::string> dirty_;                // işçinin sunucuya iletmesi gereken belgeler
    std::map<std::string, DocumentDiagnostics> diagnostics_;
    Ticket last_ticket_ = 0;
    std::optional<PendingCompletion> pending_completion_;
    std::optional<CompletionResult> completion_result_;
    std::optional<StopReason> stopped_;
    std::jthread worker_;  // son üye: önce o durur, çocuk süreç onunla ölür
};

}  // namespace noteapp::tui
