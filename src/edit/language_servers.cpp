#include "edit/language_servers.hpp"

#include "core/i18n.hpp"
#include "core/paths.hpp"
#include "core/process.hpp"
#include "edit/display_text.hpp"
#include "edit/server_settings.hpp"

#include <algorithm>
#include <iterator>
#include <limits>
#include <system_error>
#include <utility>

namespace noteapp::tui {

namespace fs = std::filesystem;
namespace lsp = core::lsp;

namespace {

const char* stop_reason_key(LspClient::StopReason reason) {
    switch (reason) {
    case LspClient::StopReason::StartFailed:
        return "tui.lsp_start_failed";
    case LspClient::StopReason::InitializeTimeout:
        return "tui.lsp_initialize_timeout";
    case LspClient::StopReason::InitializeFailed:
        return "tui.lsp_initialize_failed";
    case LspClient::StopReason::Exited:
    case LspClient::StopReason::Shutdown:
        return "tui.lsp_exited";
    case LspClient::StopReason::ProtocolError:
        return "tui.lsp_protocol_error";
    case LspClient::StopReason::WriteFailed:
        return "tui.lsp_write_failed";
    }
    return "tui.lsp_exited";
}

// Yerel ayardan gelir ama yine de ekrana basılır: yalnız uzantısız dosya adı (Windows'ta argv[0] bulunan
// .exe yoludur), terminal_safe.
std::string program_name(const std::string& command) {
    const std::u8string name = fs::path(std::u8string(command.begin(), command.end())).stem().u8string();
    return terminal_safe(std::string(name.begin(), name.end()));
}

std::uint32_t to_position_unit(std::size_t value) {
    return static_cast<std::uint32_t>(
        std::min<std::size_t>(value, std::numeric_limits<std::uint32_t>::max()));
}

std::chrono::milliseconds duration_of(const core::Limit& limit) {
    // parse_limit değeri int64'e sığdırır: milisaniyenin temsiline taşmaz.
    return limit.unlimited() ? std::chrono::milliseconds::max()
                             : std::chrono::milliseconds(static_cast<std::int64_t>(limit.value));
}

}  // namespace

LspLimits lsp_limits_of(const core::LspLimits& limits) {
    constexpr std::uint64_t kDeepest = std::numeric_limits<int>::max();
    return {.client = {.protocol = {.header_bytes = limits.header_bytes.cap(),
                                    .body_bytes = limits.body_bytes.cap(),
                                    .message_depth = static_cast<int>(
                                        limits.message_depth.unlimited()
                                            ? kDeepest
                                            : std::min(limits.message_depth.value, kDeepest)),
                                    .message_values = limits.message_values.cap(),
                                    .diagnostics = limits.diagnostics.cap(),
                                    .diagnostic_message_bytes = limits.diagnostic_message_bytes.cap(),
                                    .completion_items = limits.completion_items.cap()},
                       .completion_insert_bytes = limits.completion_insert_bytes.cap(),
                       .completion_label_bytes = limits.completion_label_bytes.cap(),
                       .pending_reply_bytes = limits.pending_reply_bytes.cap(),
                       .write_timeout = duration_of(limits.write_timeout),
                       .shutdown_timeout = duration_of(limits.shutdown_timeout)},
            .initialize_timeout = duration_of(limits.initialize_timeout),
            .note_bytes = limits.note_bytes.unlimited() ? UINTMAX_MAX : limits.note_bytes.value};
}

std::vector<DiagnosticMark> diagnostic_marks(const EditorBuffer& buffer,
                                             std::span<const lsp::Diagnostic> diagnostics) {
    const std::size_t last_line = buffer.line_count() - 1;  // tampon en az bir satırdır
    const auto offset_of = [&](lsp::Position position) {
        const std::size_t line = std::min<std::size_t>(position.line, last_line);
        return buffer.line_start(line) +
               std::min<std::size_t>(position.character, buffer.line_text(line).size());
    };
    std::vector<DiagnosticMark> marks;
    marks.reserve(diagnostics.size());
    for (const lsp::Diagnostic& diagnostic : diagnostics) {
        const std::size_t begin = offset_of(diagnostic.range.start);
        std::size_t end = std::max(begin, offset_of(diagnostic.range.end));
        // Sıfır genişlikli tanı (ör. "':' bekleniyor") görünmezdi: altındaki ilk karakter çizilir.
        if (end == begin && begin < buffer.text().size() && buffer.text()[begin] != '\n' &&
            buffer.text()[begin] != '\r')
            ++end;
        marks.push_back({{begin, end},
                         diagnostic.severity == lsp::DiagnosticSeverity::Unspecified
                             ? lsp::DiagnosticSeverity::Error
                             : diagnostic.severity,
                         diagnostic.message});
    }
    return marks;
}

LanguageServers::LanguageServers(core::LspConfig config, fs::path workspace, core::NoteStore store,
                                 std::function<void()> redraw, std::chrono::milliseconds change_idle,
                                 std::chrono::milliseconds mirror_idle, bool block_network_paths,
                                 LspLimits limits)
    : config_(std::move(config)), workspace_(std::move(workspace)), change_idle_(change_idle),
      block_network_paths_(block_network_paths), limits_(limits), redraw_(std::move(redraw)) {
    if (!config_.enabled)
        return;
    // Tek PATH taraması (sunucu başına birkaç stat): kurulu olmayan varsayılan sunucu tuşta denenmesin ve
    // notları diske yazdırmasın. Bulunan mutlak yol argv[0] olur: başlatırken yeniden PATH'e bakılmaz.
    std::erase_if(config_.servers, [this](auto& entry) {
        auto& [language, commands] = entry;
        std::erase_if(commands, [&](std::vector<std::string>& command) {
            if (auto program = core::find_program_on_absolute_path(command.front())) {
                const std::u8string text = program->u8string();
                command.front().assign(text.begin(), text.end());
                return false;
            }
            missing_[language].push_back(command.front());
            return true;
        });
        return commands.empty();
    });
    if (config_.servers.empty())
        return;
    std::set<std::string> languages;
    for (const auto& [language, commands] : config_.servers)
        languages.insert(language);
    mirror_.emplace(
        WorkspaceMirror(workspace_, std::move(languages), {}, block_network_paths_, limits_.note_bytes),
        std::move(store), mirror_idle, [this] { notify(); });
}

LanguageServers::~LanguageServers() = default;

void LanguageServers::notes_changed() {
    if (mirror_)
        mirror_->request();
}

void LanguageServers::set_store(core::NoteStore store) {
    if (mirror_)
        mirror_->set_store(std::move(store));
}

void LanguageServers::notify() {
    if (!redraw_pending_.exchange(true))
        redraw_();
}

namespace {

// TUI oturumunun dil sunucusu yüzü: davranış uyarlamadan önceki update(EditorSession&) ile aynı.
class SessionEditor final : public LspEditor {
public:
    explicit SessionEditor(EditorSession& session) : session_(session) {}
    const EditorBuffer* buffer() const override { return session_.buffer(); }
    const std::optional<core::NotePath>& path() const override { return session_.path(); }
    bool writable() const override { return session_.read_only() == EditorSession::ReadOnly::No; }
    std::uint64_t buffer_generation() const override { return session_.buffer_generation(); }
    bool completing() const override { return session_.mode() == EditorSession::Mode::Complete; }
    std::uint64_t completion_generation() const override { return session_.completion_generation(); }
    void add_completion_items(std::uint64_t generation, std::span<const lsp::CompletionItem> items) override {
        session_.add_completion_items(generation, items);
    }
    void set_diagnostics(std::vector<DiagnosticMark> marks) override { session_.set_diagnostics(std::move(marks)); }

private:
    EditorSession& session_;
};

}  // namespace

LanguageServers::Update LanguageServers::update(EditorSession& editor,
                                                std::chrono::steady_clock::time_point now) {
    SessionEditor session(editor);
    return update(static_cast<LspEditor&>(session), now);
}

LanguageServers::Update LanguageServers::update(LspEditor& editor, std::chrono::steady_clock::time_point now) {
    // Önce indirilir: bundan sonra gelen bildirim yeni bir çizim ister.
    redraw_pending_.store(false);
    Update out;
    if (!enabled())
        return out;
    report_stops(out);
    sync_document(editor, now, out);
    return out;
}

void LanguageServers::report_stops(Update& out) {
    for (const auto& [key, client] : clients_) {
        if (!client)
            continue;
        const auto reason = client->stopped();
        if (!reason || *reason == LspClient::StopReason::Shutdown || !reported_stops_.insert(key).second)
            continue;
        out.status = core::tr("tui.lsp_stopped",
                              {{"server", program_name(config_.servers.at(key.first).at(key.second).front())},
                               {"reason", core::tr(stop_reason_key(*reason))}});
    }
}

LspClient* LanguageServers::client_for(const ServerKey& key) {
    if (const auto found = clients_.find(key); found != clients_.end())
        return found->second.get();
    std::error_code ec;
    const fs::path root = fs::weakly_canonical(workspace_, ec);
    const auto user = config_.settings.find(key.first);
    // Sunucunun çalışma dizini; yansıtma onu da açar, burada sıradan bağımsız olsun diye.
    if (!ec)
        ec = core::create_private_directories(workspace_.parent_path());
    auto launch = ec ? std::nullopt
                     : server_launch(config_.servers.at(key.first).at(key.second),
                                     user == config_.settings.end() ? nullptr : &user->second,
                                     workspace_.parent_path());
#ifdef _WIN32
    // CreateProcess ve cmd.exe adı yalın programı önce çalışma dizininde arar; bu değişken onu kapatır.
    if (launch)
        launch->environment.emplace_back("NoDefaultCurrentDirectoryInExePath=1");
#endif
    if (!launch) {
        // Boş giriş: her karede yeniden denenip loga yazılmasın; bu sunucu oturumda yok.
        clients_.emplace(key, nullptr);
        return nullptr;
    }
    auto client = std::make_unique<LspClient>(
        LspClient::Options{.language = key.first,
                           .command = std::move(launch->command),
                           .root_uri = lsp::file_uri(root),
                           .initialize_timeout = limits_.initialize_timeout,
                           .environment = std::move(launch->environment),
                           // Bizim çalışma dizinimiz kullanıcının o an bulunduğu klasördür (klonlanmış bir
                           // depo olabilir); sunucunun göreli aramaları oraya bakmasın.
                           .working_directory = workspace_.parent_path(),
                           .initialization_options = std::move(launch->initialization_options),
                           .settings = std::move(launch->settings),
                           .configuration_change = std::move(launch->configuration_change),
                           .notifications = std::move(launch->notifications),
                           .limits = limits_.client},
        [this] { notify(); });
    return clients_.emplace(key, std::move(client)).first->second.get();
}

std::vector<std::pair<std::size_t, LspClient*>> LanguageServers::live_clients(const std::string& language) {
    std::vector<std::pair<std::size_t, LspClient*>> out;
    const auto servers = config_.servers.find(language);
    if (servers == config_.servers.end())
        return out;
    for (std::size_t index = 0; index < servers->second.size(); ++index) {
        LspClient* client = client_for({language, index});
        if (client != nullptr && !client->stopped())
            out.emplace_back(index, client);
    }
    return out;
}

void LanguageServers::report_missing(const std::string& language, Update& out) {
    const auto missing = missing_.find(language);
    if (missing == missing_.end() || !reported_missing_.insert(language).second)
        return;
    const bool none_left = !config_.servers.contains(language);
    out.status = core::tr(none_left ? "tui.lsp_not_found" : "tui.lsp_partly_not_found",
                          {{"server", program_name(missing->second.front())}, {"language", language}});
}

void LanguageServers::open_document(const LspEditor& editor, Update& out) {
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- sync_document yolu ve tamponu denetler
    const core::NotePath& path = *editor.path();
    const std::string language(lsp_language_for(path));
    if (language.empty())
        return;
    report_missing(language, out);
    // İlk eşitleme eski yansıtmanın artık engellenen dosyalarını silmeden sunucu başlamaz; bitince işçi çizim
    // ister, belge bir sonraki karede açılır.
    if (!mirror_ || !mirror_->ready())
        return;
    if (!config_.servers.contains(language) ||
        blocked_by_remote_reference(path, editor.buffer_generation(), *editor.buffer(), out))
        return;
    const auto clients = live_clients(language);
    if (clients.empty())
        return;
    const auto file = mirror_file(workspace_, path);
    if (!file)
        return;
    const EditorBuffer& buffer = *editor.buffer();
    document_ = OpenDocument{.path = path,
                             .uri = lsp::file_uri(*file),
                             .language = language,
                             .seen_edits = buffer.edit_count(),
                             .sent_edits = buffer.edit_count(),
                             .completion_generation = editor.completion_generation(),
                             .buffer_generation = editor.buffer_generation(),
                             .completions = {}};
    for (const auto& [index, client] : clients)
        client->open(document_->uri, language, buffer.text(), document_->version);
}

bool LanguageServers::blocked_by_remote_reference(const core::NotePath& path, std::uint64_t buffer_generation,
                                                  const EditorBuffer& buffer, Update& out) {
    if (remote_scan_ && remote_scan_->path == path && remote_scan_->buffer_generation == buffer_generation &&
        remote_scan_->edits == buffer.edit_count())
        return remote_scan_->remote;
    const bool was_remote = remote_scan_ && remote_scan_->path == path && remote_scan_->remote;
    remote_scan_ = RemoteScan{.path = path,
                              .buffer_generation = buffer_generation,
                              .edits = buffer.edit_count(),
                              .remote = block_network_paths_ && references_network_path(buffer.text())};
    if (remote_scan_->remote && !was_remote)
        out.status = core::tr("tui.lsp_remote_reference");
    return remote_scan_->remote;
}

void LanguageServers::close_document() {
    if (!document_)
        return;
    // Durmuş istemci belgeyi zaten bıraktı; close onda bir şey yapmaz.
    for (const auto& [key, client] : clients_)
        if (client && key.first == document_->language)
            client->close(document_->uri);
    document_.reset();
}

void LanguageServers::sync_document(LspEditor& editor, std::chrono::steady_clock::time_point now,
                                    Update& out) {
    const EditorBuffer* buffer = editor.buffer();
    // Taslak (yolu yok), salt-okunur ve sınırı aşan not sunucuya gitmez.
    if (buffer == nullptr || !editor.path() || !editor.writable() ||
        buffer->text().size() > limits_.note_bytes) {
        close_document();
        editor.set_diagnostics({});
        return;
    }
    // Aynı not yeniden açıldıysa (yeni tampon) sunucunun gördüğü eski metindir: kapanıp yeniden taranarak
    // açılır.
    if (document_ &&
        (document_->path != *editor.path() || document_->buffer_generation != editor.buffer_generation()))
        close_document();
    if (!document_)
        open_document(editor, out);
    const auto clients =
        document_ ? live_clients(document_->language) : std::vector<std::pair<std::size_t, LspClient*>>{};
    if (clients.empty()) {
        document_.reset();  // durmuş istemciler belgeyi zaten bıraktı
        editor.set_diagnostics({});
        return;
    }
    OpenDocument& document = *document_;

    if (const std::uint64_t edits = buffer->edit_count(); edits != document.seen_edits) {
        document.seen_edits = edits;
        document.changed_at = now;
    }
    // Liste açıldıysa öneri güncel metne göre istenir: boşta beklemeden gönderilir.
    const bool completion_asked =
        editor.completing() && editor.completion_generation() != document.completion_generation;
    if (document.seen_edits != document.sent_edits) {
        if (completion_asked || now - document.changed_at >= change_idle_) {
            // Yeni metin uzak dosya başvurusu taşıyorsa hiç gönderilmez, belge kapanır.
            if (blocked_by_remote_reference(document.path, document.buffer_generation, *buffer, out)) {
                close_document();
                editor.set_diagnostics({});
                return;
            }
            ++document.version;
            for (const auto& [index, client] : clients)
                client->change(document.uri, buffer->text(), document.version);
            document.sent_edits = document.seen_edits;
        } else {
            out.wants_frame = true;
        }
    }
    if (completion_asked) {
        document.completion_generation = editor.completion_generation();
        document.completions.clear();
        const std::size_t line = buffer->cursor_line();
        const lsp::Position position{.line = to_position_unit(line),
                                     .character =
                                         to_position_unit(buffer->cursor() - buffer->line_start(line))};
        for (const auto& [index, client] : clients)
            if (auto ticket = client->request_completion(document.uri, document.version, position))
                document.completions.emplace(index, *ticket);
    }
    if (editor.completing() && editor.completion_generation() == document.completion_generation) {
        // Her sunucunun cevabı geldikçe aynı listeye eklenir; aynı etiket bir kez görünür.
        for (const auto& [index, client] : clients) {
            const auto ticket = document.completions.find(index);
            if (ticket == document.completions.end())
                continue;
            if (auto items = client->take_completion(ticket->second)) {
                editor.add_completion_items(document.completion_generation, *items);
                document.completions.erase(ticket);
            }
        }
    }
    const bool tagged = config_.servers.at(document.language).size() > 1;
    std::vector<DiagnosticMark> marks;
    for (const auto& [index, client] : clients) {
        auto own = diagnostic_marks(*buffer, client->diagnostics(document.uri));
        if (tagged) {
            const std::string tag =
                program_name(config_.servers.at(document.language).at(index).front()) + ": ";
            for (DiagnosticMark& mark : own)
                mark.message.insert(0, tag);
        }
        std::ranges::move(own, std::back_inserter(marks));
    }
    editor.set_diagnostics(std::move(marks));
}

}  // namespace noteapp::tui
