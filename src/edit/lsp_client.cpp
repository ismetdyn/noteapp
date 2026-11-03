#include "edit/lsp_client.hpp"

#include "core/log.hpp"
#include "edit/child_process.hpp"

#include <algorithm>
#include <charconv>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace noteapp::tui {

namespace {

using Clock = ChildProcess::Clock;
using core::Json;
namespace lsp = core::lsp;

// Aşağıdaki dördü sınır değil iç işleyiştir (gecikme, parça boyu), kullanıcının işini reddetmez: config'te
// yok. Süre sınırları limits.lsp.* (LspClientLimits). İşçi bu aralıkla sunucu çıktısını bekler, sonra UI
// kuyruğuna bakar: yeni metnin gecikmesi en çok budur.
constexpr auto kPollInterval = std::chrono::milliseconds(20);
constexpr auto kExitWait = std::chrono::milliseconds(500);
// Toplam kapanış sınırı shutdown_timeout + kExitWait'ten bu kadar kısa: öldürme ve join payı.
constexpr auto kKillMargin = std::chrono::milliseconds(50);
// EOF'tan sonra çıkış kodu için kısa bekleme: kod yalnız loga gider.
constexpr auto kExitCodeWait = std::chrono::milliseconds(100);
// Büyük gövde parça parça yazılır, aralarda sunucu çıktısı boşaltılır: iki yön birden dolup kilitlenmesin
// (sunucu bize yazarken biz ona yazarsak ikimiz de bekleriz).
constexpr std::size_t kWriteChunk = 64U * 1024U;

// `from + wait`; sınırsız (milliseconds::max()) ya da taşan toplam saatin sonu olur: süresiz bekleme.
Clock::time_point after(Clock::time_point from, std::chrono::milliseconds wait) {
    const auto room = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::time_point::max() - from);
    return wait >= room ? Clock::time_point::max() : from + wait;
}

const Json* member(const Json& object, const std::string& key, Json::Kind kind) {
    if (object.kind != Json::Kind::Object)
        return nullptr;
    const auto found = object.fields.find(key);
    return found != object.fields.end() && found->second.kind == kind ? &found->second : nullptr;
}

// İstemcinin kimlikleri hep tamsayıdır; başka biçimdeki kimlik bize ait değildir.
std::optional<std::int64_t> response_id(const Json& message) {
    const Json* id = member(message, "id", Json::Kind::Number);
    if (id == nullptr)
        return std::nullopt;
    std::int64_t out = 0;
    const auto [end, error] = std::from_chars(id->text.data(), id->text.data() + id->text.size(), out);
    if (error != std::errc{} || end != id->text.data() + id->text.size())
        return std::nullopt;
    return out;
}

std::int64_t current_process_id() {
#ifdef _WIN32
    return _getpid();
#else
    return getpid();
#endif
}

// LSP satır sonları \n, \r\n ve \r'dir; editör \n tutar, \r\n'deki \r satıra sayılmaz. Yalnız \r ayırmaz.
std::vector<std::string_view> split_lines(std::string_view text) {
    std::vector<std::string_view> lines;
    while (true) {
        const std::size_t end = text.find('\n');
        std::string_view line = text.substr(0, end);
        if (line.ends_with('\r'))
            line.remove_suffix(1);
        lines.push_back(line);
        if (end == std::string_view::npos)
            return lines;
        text.remove_prefix(end + 1);
    }
}

// Sunucu konumlarını yerinde bayt sütununa çevirir; belge dışına taşan satır son satırın sonuna kısılır.
// UTF-16'da her satır bir kez yürünür: konumlar satır ve sütuna göre sıralanır, yürüyüş bir öncekinin
// bittiği yerden sürer. Konum başına baştan yürüyüş 1000 tanı × MiB'lık satırda dakikalar sürerdi.
void to_byte_positions(const std::vector<std::string_view>& lines, std::vector<lsp::Position*>& positions,
                       lsp::PositionEncoding encoding) {
    std::ranges::sort(positions, [](const lsp::Position* a, const lsp::Position* b) {
        return std::tie(a->line, a->character) < std::tie(b->line, b->character);
    });
    std::optional<std::uint32_t> walked_line;
    std::size_t walked_bytes = 0;
    std::uint32_t walked_units = 0;
    for (lsp::Position* position : positions) {
        if (position->line >= lines.size()) {
            *position = {.line = static_cast<std::uint32_t>(lines.size() - 1),
                         .character = static_cast<std::uint32_t>(lines.back().size())};
            continue;
        }
        const std::string_view line = lines[position->line];
        if (encoding == lsp::PositionEncoding::Utf8) {
            position->character =
                static_cast<std::uint32_t>(std::min<std::size_t>(position->character, line.size()));
            continue;
        }
        if (walked_line != position->line) {
            walked_line = position->line;
            walked_bytes = 0;
            walked_units = 0;
        }
        // Yürüyüş karakter sınırında durur ve sıralı hedef öncekinden küçük değildir: kalan kısım, satırın
        // başından yürümekle aynı sonucu verir.
        const std::string_view rest = line.substr(walked_bytes);
        const std::size_t step = lsp::utf16_to_byte_column(rest, position->character - walked_units);
        walked_units += lsp::byte_to_utf16_column(rest, step);
        walked_bytes += step;
        position->character = static_cast<std::uint32_t>(walked_bytes);
    }
}

lsp::Position to_server_position(std::string_view text, lsp::Position position,
                                 lsp::PositionEncoding encoding) {
    const std::vector<std::string_view> lines = split_lines(text);
    const auto line_index =
        static_cast<std::uint32_t>(std::min<std::size_t>(position.line, lines.size() - 1));
    const std::string_view line = lines[line_index];
    const std::uint32_t column =
        encoding == lsp::PositionEncoding::Utf8
            ? static_cast<std::uint32_t>(std::min<std::size_t>(position.character, line.size()))
            : lsp::byte_to_utf16_column(line, position.character);
    return {.line = line_index, .character = column};
}

std::string_view reason_name(LspClient::StopReason reason) {
    switch (reason) {
    case LspClient::StopReason::StartFailed:
        return "start_failed";
    case LspClient::StopReason::InitializeTimeout:
        return "initialize_timeout";
    case LspClient::StopReason::InitializeFailed:
        return "initialize_failed";
    case LspClient::StopReason::Exited:
        return "exited";
    case LspClient::StopReason::ProtocolError:
        return "protocol_error";
    case LspClient::StopReason::WriteFailed:
        return "write_failed";
    case LspClient::StopReason::Shutdown:
        return "shutdown";
    }
    return "unknown";
}

}  // namespace

// İşçiye ait; UI hiç dokunmaz.
struct LspClient::Session {
    struct ServerDocument {
        std::int64_t version = 0;
        std::string text;
    };
    struct InFlightCompletion {
        std::int64_t id = 0;
        Ticket ticket = 0;
        std::string uri;
        std::int64_t version = 0;
    };

    Session(ChildProcess process, std::stop_token token, const lsp::ProtocolLimits& limits)
        : child(std::move(process)), stop(std::move(token)), reader(limits) {}

    ChildProcess child;
    std::stop_token stop;
    lsp::FrameReader reader;
    lsp::PositionEncoding encoding = lsp::PositionEncoding::Utf16;
    std::int64_t next_id = 1;
    std::int64_t initialize_id = 0;
    std::int64_t shutdown_id = 0;
    std::optional<Json> initialize_result;
    bool initialize_failed = false;
    bool shutdown_acknowledged = false;
    bool shutting_down = false;
    std::map<std::string, ServerDocument> documents;  // sunucunun gördüğü
    std::optional<InFlightCompletion> completion;
    // Sunucu isteklerinin cevapları: okuma yazmanın ortasında da olur (send → boşaltma), cevap sonra gider.
    std::vector<std::string> outbox;
    std::size_t outbox_bytes = 0;
    // Kapanışta her yazma bu ana kadar biter: mesaj başına süre yavaş okuyan sunucuyla dakikalar ederdi.
    Clock::time_point shutdown_deadline{};
};

LspClient::LspClient(Options options, std::function<void()> notify)
    : options_(std::move(options)), notify_(std::move(notify)),
      worker_([this](const std::stop_token& stop) { run(stop); }) {}

LspClient::~LspClient() = default;

void LspClient::open(std::string uri, std::string language_id, std::string text, std::int64_t version) {
    {
        const std::scoped_lock lock(mutex_);
        if (stopped_)
            return;
        dirty_.insert(uri);
        documents_.insert_or_assign(
            std::move(uri),
            Document{.language_id = std::move(language_id), .text = std::move(text), .version = version});
    }
    wake_.notify_all();
}

void LspClient::change(const std::string& uri, std::string text, std::int64_t version) {
    const std::scoped_lock lock(mutex_);
    const auto found = documents_.find(uri);
    if (stopped_ || found == documents_.end())
        return;
    found->second.text = std::move(text);
    found->second.version = version;
    dirty_.insert(uri);
}

void LspClient::close(const std::string& uri) {
    const std::scoped_lock lock(mutex_);
    if (stopped_ || documents_.erase(uri) == 0)
        return;
    diagnostics_.erase(uri);
    dirty_.insert(uri);
}

std::optional<LspClient::Ticket> LspClient::request_completion(const std::string& uri, std::int64_t version,
                                                               core::lsp::Position position) {
    const std::scoped_lock lock(mutex_);
    if (stopped_ || !documents_.contains(uri))
        return std::nullopt;
    pending_completion_ =
        PendingCompletion{.ticket = ++last_ticket_, .uri = uri, .version = version, .position = position};
    completion_result_.reset();
    return last_ticket_;
}

std::optional<std::vector<core::lsp::CompletionItem>> LspClient::take_completion(Ticket ticket) {
    const std::scoped_lock lock(mutex_);
    if (!completion_result_ || completion_result_->ticket != ticket)
        return std::nullopt;
    CompletionResult result = std::move(*completion_result_);
    completion_result_.reset();
    const auto document = documents_.find(result.uri);
    if (document == documents_.end() || document->second.version != result.version)
        return std::nullopt;
    return std::move(result.items);
}

std::vector<core::lsp::Diagnostic> LspClient::diagnostics(const std::string& uri) const {
    const std::scoped_lock lock(mutex_);
    const auto found = diagnostics_.find(uri);
    return found == diagnostics_.end() ? std::vector<core::lsp::Diagnostic>{} : found->second.diagnostics;
}

LspClient::DocumentDiagnostics LspClient::document_diagnostics(const std::string& uri) const {
    const std::scoped_lock lock(mutex_);
    const auto found = diagnostics_.find(uri);
    return found == diagnostics_.end() ? DocumentDiagnostics{} : found->second;
}

std::optional<LspClient::StopReason> LspClient::stopped() const {
    const std::scoped_lock lock(mutex_);
    return stopped_;
}

void LspClient::run(const std::stop_token& stop) {
    {
        std::unique_lock lock(mutex_);
        // Tembel başlatma: kapatılan ya da hiç açılmayan belge sunucu başlatmaz.
        wake_.wait(lock, stop, [this] { return !documents_.empty(); });
        if (stop.stop_requested())
            return;
    }
    auto child =
        ChildProcess::start(options_.command, true, options_.environment, options_.working_directory);
    if (!child) {
        finish(StopReason::StartFailed, std::nullopt);
        return;
    }
    core::log_info("event=lsp_started language=" + options_.language);
    Session session(std::move(*child), stop, options_.limits.protocol);
    auto failure = initialize(session);
    if (!failure && !stop.stop_requested())
        failure = serve(session);
    if (!failure) {
        shut_down(session);
        return;
    }
    // Durma isteğiyle kesilen yazma da hata döner; akış yarım mesajda kaldığından nazik kapanış denenmez.
    if (stop.stop_requested()) {
        session.child.kill();
        finish(StopReason::Shutdown, std::nullopt);
        return;
    }
    const std::optional<int> exit_code = session.child.wait_until(Clock::now() + kExitCodeWait);
    session.child.kill();
    // Ölü sunucuya yazmak EOF'tan önce fark edilebilir: neden yine de çıkıştır.
    if (*failure == StopReason::WriteFailed && exit_code)
        failure = StopReason::Exited;
    finish(*failure, exit_code);
}

std::optional<LspClient::StopReason> LspClient::initialize(Session& session) {
    session.initialize_id = session.next_id++;
    if (auto failed = send(session,
                           lsp::initialize_request(session.initialize_id, current_process_id(),
                                                   options_.root_uri, options_.initialization_options),
                           after(Clock::now(), options_.limits.write_timeout)))
        return failed;
    const auto deadline = after(Clock::now(), options_.initialize_timeout);
    while (!session.initialize_result) {
        if (session.stop.stop_requested())
            return std::nullopt;
        if (session.initialize_failed)
            return StopReason::InitializeFailed;
        const auto now = Clock::now();
        if (now >= deadline)
            return StopReason::InitializeTimeout;
        if (auto failed = pump(session, std::min(deadline, now + kPollInterval)))
            return failed;
    }
    session.encoding = lsp::negotiated_encoding(*session.initialize_result);
    if (auto failed = send(session, lsp::initialized_notification(),
                           after(Clock::now(), options_.limits.write_timeout)))
        return failed;
    // Belgelerden önce: ayarı bildirimle alan sunucu (taplo, vscode-json) ilk belgeyi varsayılan ayarla, yani
    // ağ açıkken işlemesin.
    if (!options_.configuration_change.empty()) {
        if (auto failed =
                send(session, lsp::did_change_configuration_notification(options_.configuration_change),
                     after(Clock::now(), options_.limits.write_timeout)))
            return failed;
    }
    for (const std::string& method : options_.notifications) {
        if (auto failed = send(session, lsp::bare_notification(method),
                               after(Clock::now(), options_.limits.write_timeout)))
            return failed;
    }
    return std::nullopt;
}

std::optional<LspClient::StopReason> LspClient::serve(Session& session) {
    while (!session.stop.stop_requested()) {
        if (auto failed = sync(session))
            return failed;
        if (auto failed = pump(session, Clock::now() + kPollInterval))
            return failed;
    }
    return std::nullopt;
}

std::optional<LspClient::StopReason> LspClient::sync(Session& session) {
    std::vector<std::pair<std::string, std::optional<Document>>> work;
    std::optional<PendingCompletion> completion;
    {
        const std::scoped_lock lock(mutex_);
        for (const std::string& uri : dirty_) {
            const auto found = documents_.find(uri);
            work.emplace_back(uri, found == documents_.end() ? std::nullopt : std::optional(found->second));
        }
        dirty_.clear();
        completion = std::exchange(pending_completion_, std::nullopt);
    }
    for (auto& [uri, desired] : work) {
        const auto server = session.documents.find(uri);
        const auto deadline = after(Clock::now(), options_.limits.write_timeout);
        if (!desired) {
            if (server == session.documents.end())
                continue;
            session.documents.erase(server);
            if (auto failed = send(session, lsp::did_close_notification(uri), deadline))
                return failed;
            // Kapanıştan önce okunan tanı yeniden yazılmış olabilir.
            const std::scoped_lock lock(mutex_);
            if (!documents_.contains(uri))
                diagnostics_.erase(uri);
            continue;
        }
        std::string body;
        if (server == session.documents.end())
            body = lsp::did_open_notification(uri, desired->language_id, desired->version, desired->text);
        else if (server->second.version != desired->version)
            body = lsp::did_change_notification(uri, desired->version, desired->text);
        else
            continue;
        session.documents.insert_or_assign(
            uri, Session::ServerDocument{.version = desired->version, .text = std::move(desired->text)});
        if (auto failed = send(session, body, deadline))
            return failed;
    }
    if (!completion)
        return std::nullopt;
    const auto server = session.documents.find(completion->uri);
    if (server == session.documents.end() || server->second.version != completion->version)
        return std::nullopt;  // istek gönderilmeden eskidi
    const std::int64_t id = session.next_id++;
    const lsp::Position position =
        to_server_position(server->second.text, completion->position, session.encoding);
    session.completion = Session::InFlightCompletion{
        .id = id, .ticket = completion->ticket, .uri = completion->uri, .version = completion->version};
    return send(session, lsp::completion_request(id, completion->uri, position),
                after(Clock::now(), options_.limits.write_timeout));
}

std::optional<LspClient::StopReason> LspClient::pump(Session& session, Clock::time_point deadline) {
    std::string bytes;
    const auto status = session.child.read_some(bytes, deadline);
    if (status == ChildProcess::ReadStatus::Eof || status == ChildProcess::ReadStatus::Failed)
        return StopReason::Exited;
    if (status == ChildProcess::ReadStatus::Data) {
        if (auto failed = receive(session, bytes))
            return failed;
    }
    // Yalnız şimdiye dek birikenler: gönderirken okunan yeni istekler sonraki tura kalır, yağdıran sunucu bu
    // döngüyü bitmez kılamaz ve işçi her tur durma isteğine bakar.
    const std::vector<std::string> replies = std::exchange(session.outbox, {});
    session.outbox_bytes = 0;
    for (const std::string& reply : replies) {
        if (auto failed = send(session, reply, after(Clock::now(), options_.limits.write_timeout)))
            return failed;
    }
    return std::nullopt;
}

// Her eklemeden sonra hazır gövdelerin hepsi alınır: FrameReader tamponunun sınırı buna dayanır.
std::optional<LspClient::StopReason> LspClient::receive(Session& session, std::string_view bytes) {
    session.reader.append(bytes);
    while (true) {
        // Durma isteği iletiler arasında yoklanır: büyük bir yığını işlemek yıkıcıyı bekletmesin. Kapanışta
        // shutdown cevabı beklendiğinden sürer; orada istek ve tanılar zaten atlanır.
        if (session.stop.stop_requested() && !session.shutting_down)
            return std::nullopt;
        auto body = session.reader.next();
        if (!body)
            return StopReason::ProtocolError;
        if (!*body)
            return std::nullopt;
        // Ayrıştırılamayan gövde atlanır: çerçeve sağlam, akışın hizası korunur.
        if (const auto message = lsp::parse_message(**body, options_.limits.protocol))
            handle(session, *message);
        if (session.outbox_bytes > options_.limits.pending_reply_bytes)
            return StopReason::ProtocolError;
    }
}

std::optional<LspClient::StopReason> LspClient::send(Session& session, std::string_view body,
                                                     Clock::time_point deadline) {
    const std::string framed = lsp::frame(body);
    // Kapanışta durma isteği zaten verilmiştir; yazmayı yalnız süre sınırlar.
    const std::stop_token stop = session.shutting_down ? std::stop_token{} : session.stop;
    if (session.shutting_down)
        deadline = std::min(deadline, session.shutdown_deadline);
    std::string_view rest = framed;
    while (!rest.empty()) {
        const std::string_view chunk = rest.substr(0, kWriteChunk);
        if (!session.child.write_all(chunk, deadline, stop))
            return StopReason::WriteFailed;
        rest.remove_prefix(chunk.size());
        if (rest.empty())
            break;
        // Parça başına tek okuma: yağdıran sunucu işçiyi burada tutamaz. EOF ve okuma hatası burada yok
        // sayılır, bir sonraki pump görür.
        std::string bytes;
        if (session.child.read_some(bytes, Clock::now()) == ChildProcess::ReadStatus::Data) {
            if (auto failed = receive(session, bytes))
                return failed;
        }
    }
    return std::nullopt;
}

void LspClient::handle(Session& session, const Json& message) {
    if (member(message, "method", Json::Kind::String) != nullptr) {
        if (session.shutting_down)
            return;
        if (auto reply = lsp::reply_to_server_request(message, options_.settings,
                                                      options_.limits.pending_reply_bytes)) {
            session.outbox_bytes += reply->size();
            session.outbox.push_back(std::move(*reply));
            return;
        }
        const Json* params = member(message, "params", Json::Kind::Object);
        if (params != nullptr && message.fields.at("method").text == "textDocument/publishDiagnostics")
            store_diagnostics(session, *params);
        return;
    }
    const auto id = response_id(message);
    if (!id)
        return;
    if (*id == session.initialize_id && !session.initialize_result) {
        if (const Json* result = member(message, "result", Json::Kind::Object))
            session.initialize_result = *result;
        else
            session.initialize_failed = true;
    } else if (*id == session.shutdown_id) {
        session.shutdown_acknowledged = true;
    } else if (session.completion && *id == session.completion->id) {
        deliver_completion(session, message);
    }
}

// URI sunucu metnidir: yalnız bizim gönderdiğimiz URI'lerle bayt bayt karşılaştırılır, yola çevrilmez.
void LspClient::store_diagnostics(Session& session, const Json& params) {
    auto published = lsp::parse_publish_diagnostics(params, options_.limits.protocol);
    if (!published)
        return;
    const auto document = session.documents.find(published->uri);
    if (document == session.documents.end())
        return;
    if (published->version && *published->version != document->second.version)
        return;  // sunucunun eski metne ait tanısı
    const std::vector<std::string_view> lines = split_lines(document->second.text);
    std::vector<lsp::Position*> positions;
    positions.reserve(published->diagnostics.size() * 2);
    for (lsp::Diagnostic& diagnostic : published->diagnostics) {
        positions.push_back(&diagnostic.range.start);
        positions.push_back(&diagnostic.range.end);
    }
    to_byte_positions(lines, positions, session.encoding);
    for (lsp::Diagnostic& diagnostic : published->diagnostics) {
        // Ters aralık sıraya konur: çizen taraf başlangıçtan sona yürür.
        if (std::tie(diagnostic.range.end.line, diagnostic.range.end.character) <
            std::tie(diagnostic.range.start.line, diagnostic.range.start.character))
            std::swap(diagnostic.range.start, diagnostic.range.end);
    }
    {
        const std::scoped_lock lock(mutex_);
        if (!documents_.contains(published->uri))
            return;
        diagnostics_.insert_or_assign(std::move(published->uri),
                                      DocumentDiagnostics{.version = document->second.version,
                                                          .diagnostics = std::move(published->diagnostics)});
    }
    notify_();
}

void LspClient::deliver_completion(Session& session, const Json& response) {
    const Session::InFlightCompletion request = *std::exchange(session.completion, std::nullopt);
    const auto result = response.fields.find("result");
    if (result == response.fields.end())
        return;  // hata cevabı: öneri yok
    std::vector<lsp::CompletionItem> items =
        lsp::parse_completion_result(result->second, options_.limits.protocol);
    std::erase_if(items, [&limits = options_.limits](const lsp::CompletionItem& item) {
        return item.insert_text.size() > limits.completion_insert_bytes ||
               item.label.size() > limits.completion_label_bytes;
    });
    {
        const std::scoped_lock lock(mutex_);
        const auto document = documents_.find(request.uri);
        if (document == documents_.end() || document->second.version != request.version ||
            last_ticket_ != request.ticket)
            return;
        completion_result_ = CompletionResult{.ticket = request.ticket,
                                              .uri = request.uri,
                                              .version = request.version,
                                              .items = std::move(items)};
    }
    notify_();
}

// shutdown (shutdown_timeout, varsayılan 1 s) → exit → stdin kapanır → 500 ms bekleme; kalan süreç oturumla
// birlikte öldürülür. Toplam sınır varsayılanla 1,45 s.
void LspClient::shut_down(Session& session) {
    session.shutting_down = true;
    const auto started = Clock::now();
    const auto shutdown_deadline = after(started, options_.limits.shutdown_timeout);
    const auto budget = after(shutdown_deadline, kExitWait - kKillMargin);
    session.shutdown_deadline = budget;
    // Kapanan sunucunun isteklerine artık cevap borcumuz yok; kalan cevaplar kapanış yazmalarını
    // geciktirmesin.
    session.outbox.clear();
    session.outbox_bytes = 0;
    if (session.initialize_result) {
        const auto deadline = shutdown_deadline;
        session.shutdown_id = session.next_id++;
        if (!send(session, lsp::shutdown_request(session.shutdown_id), deadline)) {
            while (!session.shutdown_acknowledged && Clock::now() < deadline) {
                if (pump(session, deadline))
                    break;
            }
        }
    }
    const auto exit_deadline = std::min(after(Clock::now(), kExitWait), budget);
    static_cast<void>(send(session, lsp::exit_notification(), exit_deadline));
    session.child.close_stdin();
    const std::optional<int> exit_code = session.child.wait_until(exit_deadline);
    session.child.kill();
    finish(StopReason::Shutdown, exit_code);
}

void LspClient::finish(StopReason reason, std::optional<int> exit_code) {
    {
        const std::scoped_lock lock(mutex_);
        stopped_ = reason;
        documents_.clear();
        dirty_.clear();
        diagnostics_.clear();
        pending_completion_.reset();
        completion_result_.reset();
    }
    const std::string line = "event=lsp_stopped language=" + options_.language +
                             " exit_code=" + (exit_code ? std::to_string(*exit_code) : "none") +
                             " reason=" + std::string(reason_name(reason));
    if (reason == StopReason::Shutdown) {
        core::log_info(line);
        return;  // yıkıcıdayız: UI'yi uyandırmanın anlamı yok
    }
    // Önce UI uyanır: log dosya G/Ç'sidir (Wine'da milisaniyeler), durumu gören çizim onu beklememeli.
    notify_();
    core::log_warn(line);
}

}  // namespace noteapp::tui
