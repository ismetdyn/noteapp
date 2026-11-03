// LspClient testlerinde gerçek dil sunucusu yerine çalışan stdio LSP sunucusu. Kip argv[1]:
//   normal           her didOpen/didChange için bir tanı yayınlar ("count=N text=<metin>", ilk satırın
//                    tamamı), completion'a "pos:<satır>:<sütun>" ve "alpha" ile cevap verir; utf-8 anlaşır
//   hang             initialize'a hiç cevap vermez, stdin'i de okumaz
//   deaf             initialize'a cevap verir, sonra ne okur ne çıkar (kapanışın en kötü durumu)
//   crash            initialized bildiriminden sonra kCrashExitCode ile çıkar
//   oversize         initialize cevabından sonra sınırı aşan Content-Length gönderir
//   ask-config       initialize'a cevaptan önce workspace/configuration sorar; cevap bozuksa çıkar
//   utf16            utf-16 anlaşır; tanılar ilk satırdaki "bad" sözcüğünün, sonra ikinci karakterin
//                    UTF-16 aralığıdır (sıra sütuna göre değil)
//   slow-completion  normal; completion cevabı kSlowCompletionDelay gecikir
//   hostile         tanıyı önce yabancı ve "../" URI'lere, sonra devasa satır numarasıyla ve başı sonundan
//                    sonra gelen aralıkla gönderir;
//                    completion'da ekleme metni ya da (kısa ekleme metniyle) etiketi sınırı aşan birer öğe
//                    döner
//   flood            initialize cevabından sonra ayrı iş parçacığından durmadan istek yağdırır,
//                    stdin'i okumayı sürdürür, shutdown'a cevap vermez
//   config-bomb      initialized'dan sonra cevabı kMaxPendingReplyBytes'ı aşan tek bir
//                    workspace/configuration sorar
//   slow-reader      normal; initialized'dan sonra birkaç workspace/configuration sorar, shutdown'a
//                    cevap verdikten sonra stdin'den kSlowReadInterval'da bir kSlowReadPage okur
//   utf16-heavy      utf-16 anlaşır; didOpen'a ilk satırda devasa sütunlu kHeavyDiagnostics tanıyla
//                    cevap verir
//   diagnostic-flood her didOpen/didChange için kDiagnosticFloodCount tanı bildirimi art arda (çizim
//                    birleştirme testi)
//   record <dosya>   normal; gördüklerini <dosya>'ya satır başına bir JSON olarak ekler: initialize
//                    (initializationOptions, argv, kSettingsEnv ortamı, çalışma dizini, çalışma dizinindeki
//                    workspace/'in girdileri), initialized'dan sonra sorduğu
//                    kRecordSections'ın cevapları, didChangeConfiguration, didOpen ve tanımadığı her bildirim
// normal kipte argv[2] verilirse completion cevabına o etiketle bir öğe daha eklenir (çok sunuculu
// birleştirme).
#include "core/json_text.hpp"
#include "core/lsp_protocol.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace {

using noteapp::core::Json;
namespace lsp = noteapp::core::lsp;

constexpr int kCrashExitCode = 3;
constexpr int kBadMessageExitCode = 2;
constexpr int kBadConfigReplyExitCode = 4;
constexpr int kDiagnosticFloodCount = 500;
constexpr auto kSlowCompletionDelay = std::chrono::milliseconds(400);
constexpr auto kForever = std::chrono::hours(1);
constexpr std::string_view kHeaderEnd = "\r\n\r\n";
constexpr std::string_view kContentLength = "Content-Length:";
constexpr std::string_view kHostileWord = "bad";
constexpr std::uint32_t kHugeLine = 4'000'000'000U;
constexpr std::size_t kHugeInsertBytes = 1024U * 1024U;
constexpr std::uint32_t kReversedStart = 5;  // aralığın başı sonundan sonra
constexpr std::uint32_t kReversedEnd = 2;
constexpr unsigned char kContinuationMask = 0xC0;
constexpr unsigned char kContinuationBits = 0x80;
constexpr unsigned char kFourByteLead = 0xF0;
constexpr int kFloodFramesPerBatch = 1000;
// İstemci sınırı 250000 değerdir; bu kadar öğe ayrıştırılır ama cevabı ("null," × öğe) 1 MiB'ı aşar.
constexpr int kBombItems = 240'000;
constexpr int kSlowReaderRequests = 8;
constexpr auto kSlowReadInterval = std::chrono::seconds(2);
constexpr std::size_t kSlowReadPage = 4096;
constexpr int kHeavyDiagnostics = 1000;
constexpr std::uint32_t kHugeCharacter = 4'000'000'000U;
constexpr std::uint32_t kSecondCharacterStart = 2;  // ilk astral karakter iki UTF-16 birimi
constexpr std::uint32_t kSecondCharacterEnd = 4;
constexpr std::string_view kRecordConfigId = "record-config";
constexpr std::array<std::string_view, 7> kRecordSections = {
    "rust-analyzer", "gopls", "java", "yaml", "http", "evenBetterToml", "json"};
// Sertleştirme tablosunun ortam değişkenleri; ortamın tamamı (kullanıcının sırları) dosyaya yazılmaz.
constexpr std::array<const char*, 8> kSettingsEnv = {"CARGO_NET_OFFLINE",
                                                     "RUSTUP_AUTO_INSTALL",
                                                     "GOTOOLCHAIN",
                                                     "GOPROXY",
                                                     "GOVCS",
                                                     "GOFLAGS",
                                                     "CGO_ENABLED",
                                                     "NoDefaultCurrentDirectoryInExePath"};

// Başlık bayt bayt okunur: getc, borudaki kadarını döner, tam tamponu beklemez. Gövde tam uzunlukta okunur.
std::optional<std::string> read_message() {
    std::string header;
    while (!header.ends_with(kHeaderEnd)) {
        const int c = std::getc(stdin);
        if (c == EOF)
            return std::nullopt;
        header += static_cast<char>(c);
    }
    const std::size_t at = header.find(kContentLength);
    if (at == std::string::npos)
        return std::nullopt;
    const std::size_t length = std::stoul(header.substr(at + kContentLength.size()));
    std::string body(length, '\0');
    if (std::fread(body.data(), 1, length, stdin) != length)
        return std::nullopt;
    return body;
}

void write_raw(std::string_view bytes) {
    std::fwrite(bytes.data(), 1, bytes.size(), stdout);
    std::fflush(stdout);
}

void write_message(const std::string& body) {
    write_raw(lsp::frame(body));
}

std::string json_string(std::string_view text) {
    std::string out;
    noteapp::core::append_json_string(out, text);
    return out;
}

const Json* member(const Json& object, const std::string& key) {
    const auto found = object.fields.find(key);
    return found == object.fields.end() ? nullptr : &found->second;
}

std::string text_of(const Json* json) {
    return json != nullptr && json->kind == Json::Kind::String ? json->text : std::string();
}

// Kimlik ham JSON olarak geri yazılır: sayı belirteci ya da tırnaklı dize.
std::string raw_id(const Json& message) {
    const Json* id = member(message, "id");
    if (id == nullptr)
        return "null";
    return id->kind == Json::Kind::Number ? id->text : json_string(id->text);
}

void reply(const Json& message, std::string_view result) {
    write_message(R"({"jsonrpc":"2.0","id":)" + raw_id(message) + R"(,"result":)" + std::string(result) +
                  "}");
}

// Bağımsız kehanet: istemcinin dönüşümünü sınamak için lsp_protocol'ü kullanmaz.
std::uint32_t utf16_units(std::string_view utf8) {
    std::uint32_t units = 0;
    for (const char c : utf8) {
        const auto byte = static_cast<unsigned char>(c);
        if ((byte & kContinuationMask) != kContinuationBits)
            units += byte >= kFourByteLead ? 2U : 1U;
    }
    return units;
}

std::string range_json(std::uint32_t line, std::uint32_t start, std::uint32_t end) {
    const std::string l = std::to_string(line);
    return R"({"start":{"line":)" + l + R"(,"character":)" + std::to_string(start) + R"(},"end":{"line":)" +
           l + R"(,"character":)" + std::to_string(end) + "}}";
}

std::string diagnostic_json(const std::string& range, std::string_view message) {
    return R"({"range":)" + range + R"(,"severity":1,"message":)" + json_string(message) + "}";
}

// `list`: virgülle ayrılmış tanı nesneleri.
void publish_list(std::string_view uri, std::int64_t version, const std::string& list) {
    write_message(R"({"jsonrpc":"2.0","method":"textDocument/publishDiagnostics","params":{"uri":)" +
                  json_string(uri) + R"(,"version":)" + std::to_string(version) + R"(,"diagnostics":[)" +
                  list + "]}}");
}

void publish(std::string_view uri, std::int64_t version, const std::string& range, std::string_view message) {
    publish_list(uri, version, diagnostic_json(range, message));
}

Json object() {
    return Json{.kind = Json::Kind::Object};
}

Json string_value(std::string text) {
    return Json{.kind = Json::Kind::String, .text = std::move(text)};
}

class Server {
public:
    explicit Server(std::vector<std::string> argv) : argv_(std::move(argv)), mode_(argv_.at(1)) {
        if (mode_ == "record" && argv_.size() > 2)
            record_path_ = argv_[2];
        else if (argv_.size() > 2)
            extra_label_ = argv_[2];
    }

    int run() {
        while (true) {
            const auto body = read_message();
            if (!body)
                return 0;
            const auto message = lsp::parse_message(*body);
            if (!message)
                return kBadMessageExitCode;
            const std::string method = text_of(member(*message, "method"));
            if (method.empty()) {
                record_answer(*message);
            } else if (method == "initialize") {
                record_initialize(*message);
                if (const auto failed = initialize(*message))
                    return *failed;
            } else if (method == "initialized") {
                if (mode_ == "crash")
                    return kCrashExitCode;
                if (mode_ == "config-bomb")
                    ask_bomb();
                if (mode_ == "slow-reader")
                    ask_configuration();
                if (!record_path_.empty())
                    ask_record_sections();
            } else if (method == "textDocument/didOpen" || method == "textDocument/didChange") {
                if (method == "textDocument/didOpen")
                    record("didOpen", object());
                document_changed(method, *message);
            } else if (method == "textDocument/completion") {
                complete(*message);
            } else if (method == "shutdown") {
                if (mode_ != "flood")  // stdout yağdıran iş parçacığının
                    reply(*message, "null");
                if (mode_ == "slow-reader")
                    read_slowly();
            } else if (method == "exit") {
                return 0;
            } else if (method == "workspace/didChangeConfiguration") {
                const Json* params = member(*message, "params");
                const Json* settings = params == nullptr ? nullptr : member(*params, "settings");
                Json fields = object();
                fields.fields.emplace("settings",
                                      settings == nullptr ? Json{.kind = Json::Kind::Null} : *settings);
                record("didChangeConfiguration", std::move(fields));
            } else if (member(*message, "id") == nullptr) {
                record(method, object());
            }
        }
    }

private:
    // Olay satırı dosyaya hemen yazılır: test dosyayı sunucu çalışırken yoklar.
    void record(std::string_view event, Json fields) {
        if (record_path_.empty())
            return;
        fields.fields.insert_or_assign("event", string_value(std::string(event)));
        std::ofstream out(record_path_, std::ios::binary | std::ios::app);
        out << noteapp::core::json_text(fields) << '\n';
    }

    void record_initialize(const Json& message) {
        Json fields = object();
        const Json* params = member(message, "params");
        if (const Json* options = params == nullptr ? nullptr : member(*params, "initializationOptions"))
            fields.fields.emplace("initializationOptions", *options);
        Json argv{.kind = Json::Kind::Array};
        for (const std::string& argument : argv_)
            argv.items.push_back(string_value(argument));
        fields.fields.emplace("argv", std::move(argv));
        Json env = object();
        for (const char* name : kSettingsEnv)
            // NOLINTNEXTLINE(concurrency-mt-unsafe) -- tek iş parçacıklı test aracı, setenv yok
            if (const char* value = std::getenv(name))
                env.fields.emplace(name, string_value(value));
        fields.fields.emplace("env", std::move(env));
        std::error_code ec;
        const std::filesystem::path cwd = std::filesystem::current_path(ec);
        const std::u8string cwd_text = cwd.u8string();
        fields.fields.emplace("cwd", string_value(std::string(cwd_text.begin(), cwd_text.end())));
        // Sunucunun başladığı anda kökte duranlar (kök = çalışma dizini/workspace): eski yansıtma artığı
        // görülür mü.
        Json entries{.kind = Json::Kind::Array};
        for (const auto& entry : std::filesystem::directory_iterator(cwd / "workspace", ec)) {
            const std::u8string name = entry.path().filename().u8string();
            entries.items.push_back(string_value(std::string(name.begin(), name.end())));
        }
        fields.fields.emplace("workspace_entries", std::move(entries));
        record("initialize", std::move(fields));
    }

    static void ask_record_sections() {
        std::string items;
        for (const std::string_view section : kRecordSections)
            items +=
                std::string(items.empty() ? "" : ",") + R"({"section":")" + std::string(section) + R"("})";
        write_message(R"({"jsonrpc":"2.0","id":")" + std::string(kRecordConfigId) +
                      R"(","method":"workspace/configuration","params":{"items":[)" + items + "]}}");
    }

    void record_answer(const Json& message) {
        const Json* result = member(message, "result");
        if (text_of(member(message, "id")) != kRecordConfigId || result == nullptr ||
            result->items.size() != kRecordSections.size())
            return;
        Json answers = object();
        for (std::size_t i = 0; i < kRecordSections.size(); ++i)
            answers.fields.emplace(std::string(kRecordSections[i]), result->items[i]);
        Json fields = object();
        fields.fields.emplace("answers", std::move(answers));
        record("configuration", std::move(fields));
    }

    std::optional<int> initialize(const Json& message) {
        if (mode_ == "hang") {
            std::this_thread::sleep_for(kForever);
            return 0;
        }
        if (mode_ == "ask-config") {
            write_message(R"({"jsonrpc":"2.0","id":"cfg-1","method":"workspace/configuration",)"
                          R"("params":{"items":[{"section":"a"},{"section":"b"}]}})");
            const auto body = read_message();
            const auto answer = body ? lsp::parse_message(*body) : std::nullopt;
            const Json* result = answer ? member(*answer, "result") : nullptr;
            if (!answer || text_of(member(*answer, "id")) != "cfg-1" || result == nullptr ||
                result->kind != Json::Kind::Array || result->items.size() != 2)
                return kBadConfigReplyExitCode;
        }
        const std::string_view encoding = mode_.starts_with("utf16") ? "utf-16" : "utf-8";
        reply(message, R"({"capabilities":{"positionEncoding":")" + std::string(encoding) +
                           R"(","textDocumentSync":1,"completionProvider":{}}})");
        if (mode_ == "oversize")
            write_raw("Content-Length: " + std::to_string(lsp::kMaxBodyBytes + 1) + std::string(kHeaderEnd));
        if (mode_ == "deaf") {
            std::this_thread::sleep_for(kForever);
            return 0;
        }
        if (mode_ == "flood")
            std::thread(flood).detach();
        return std::nullopt;
    }

    // Toplu yazılır: okuyan istemciden hızlı olsun, boru hiç boşalmasın.
    static void flood() {
        std::string batch;
        for (int i = 0; i < kFloodFramesPerBatch; ++i)
            batch +=
                lsp::frame(R"({"jsonrpc":"2.0","id":)" + std::to_string(i) + R"(,"method":"flood/ping"})");
        while (true)
            write_raw(batch);
    }

    static void ask_configuration() {
        for (int i = 0; i < kSlowReaderRequests; ++i)
            write_message(R"({"jsonrpc":"2.0","id":)" + std::to_string(i) +
                          R"(,"method":"workspace/configuration","params":{"items":[{}]}})");
    }

    // Kapanışta sunucu stdin'i neredeyse hiç okumuyor: istemcinin her yazması toplam kapanış süresine
    // sığmalı.
    [[noreturn]] static void read_slowly() {
        std::string page(kSlowReadPage, '\0');
        while (true) {
            std::this_thread::sleep_for(kSlowReadInterval);
            if (std::fread(page.data(), 1, page.size(), stdin) == 0)
                std::this_thread::sleep_for(kForever);
        }
    }

    static void ask_bomb() {
        std::string items;
        for (int i = 0; i < kBombItems; ++i)
            items += i == 0 ? "{}" : ",{}";
        write_message(
            R"({"jsonrpc":"2.0","id":"bomb","method":"workspace/configuration","params":{"items":[)" + items +
            "]}}");
    }

    void document_changed(const std::string& method, const Json& message) {
        ++notifications_;
        const Json* params = member(message, "params");
        const Json* document = params == nullptr ? nullptr : member(*params, "textDocument");
        if (document == nullptr)
            return;
        const std::string uri = text_of(member(*document, "uri"));
        const Json* version_json = member(*document, "version");
        const std::int64_t version = version_json == nullptr ? 0 : std::stoll(version_json->text);
        std::string text;
        if (method == "textDocument/didOpen") {
            text = text_of(member(*document, "text"));
        } else if (const Json* changes = member(*params, "contentChanges");
                   changes != nullptr && !changes->items.empty()) {
            text = text_of(member(changes->items.back(), "text"));
        }
        const std::string first_line = text.substr(0, text.find('\n'));
        if (mode_ == "utf16") {
            const std::size_t at = first_line.find(kHostileWord);
            if (at == std::string::npos)
                return;
            const std::uint32_t start = utf16_units(first_line.substr(0, at));
            publish_list(
                uri, version,
                diagnostic_json(range_json(0, start, start + utf16_units(kHostileWord)), "bad") + "," +
                    diagnostic_json(range_json(0, kSecondCharacterStart, kSecondCharacterEnd), "second"));
            return;
        }
        if (mode_ == "utf16-heavy") {
            std::string list;
            for (int i = 0; i < kHeavyDiagnostics; ++i)
                list += (i == 0 ? "" : ",") +
                        diagnostic_json(range_json(0, kHugeCharacter, kHugeCharacter), "far");
            publish_list(uri, version, list);
            return;
        }
        if (mode_ == "diagnostic-flood") {
            for (int i = 0; i < kDiagnosticFloodCount; ++i)
                publish(uri, version, range_json(0, 0, 1), "flood " + std::to_string(i));
            return;
        }
        if (mode_ == "hostile") {
            const std::string range = range_json(0, 0, 1);
            publish("file:///unknown.py", version, range, "foreign");
            const std::size_t slash = uri.rfind('/');
            publish(uri.substr(0, slash) + "/sub/.." + uri.substr(slash), version, range, "foreign");
            publish_list(uri, version,
                         diagnostic_json(range_json(kHugeLine, 0, 1), "huge") + "," +
                             diagnostic_json(range_json(0, kReversedStart, kReversedEnd), "reversed"));
            return;
        }
        publish(uri, version, range_json(0, 0, static_cast<std::uint32_t>(first_line.size())),
                "count=" + std::to_string(notifications_) + " text=" + text);
    }

    void complete(const Json& message) {
        if (mode_ == "slow-completion")
            std::this_thread::sleep_for(kSlowCompletionDelay);
        const Json* params = member(message, "params");
        const Json* position = params == nullptr ? nullptr : member(*params, "position");
        const Json* line = position == nullptr ? nullptr : member(*position, "line");
        const Json* character = position == nullptr ? nullptr : member(*position, "character");
        const std::string label =
            "pos:" + (line ? line->text : "?") + ":" + (character ? character->text : "?");
        std::string items =
            R"([{"label":)" + json_string(label) + R"json(},{"label":"alpha","insertText":"alpha()"})json";
        if (mode_ == "hostile")
            items += R"(,{"label":"huge","insertText":")" + std::string(kHugeInsertBytes, 'x') +
                     R"("},{"label":")" + std::string(kHugeInsertBytes, 'y') + R"(","insertText":"y"})";
        if (!extra_label_.empty())
            items += R"(,{"label":)" + json_string(extra_label_) + "}";
        reply(message, items + "]");
    }

    std::vector<std::string> argv_;
    std::string mode_;
    std::string record_path_;
    std::string extra_label_;
    int notifications_ = 0;
};

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    // Metin kipi \n'yi \r\n'ye çevirip Content-Length'i bozardı.
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    if (argc < 2)
        return kBadMessageExitCode;
    const int code = Server(std::vector<std::string>(argv, argv + argc)).run();
    // Yağdıran iş parçacığı hâlâ yazıyor: exit()'in stdio temizliğiyle yarışmasın.
    if (std::string_view(argv[1]) == "flood")
        std::_Exit(code);
    return code;
}
