#include "core/lsp_protocol.hpp"

#include "core/json_text.hpp"

#include <algorithm>
#include <charconv>
#include <system_error>

namespace noteapp::core::lsp {

namespace {

constexpr std::string_view kHeaderEnd = "\r\n\r\n";
constexpr std::string_view kLineEnd = "\r\n";
constexpr std::string_view kContentLength = "content-length";
constexpr std::string_view kMessageStart = R"({"jsonrpc":"2.0")";
constexpr int kMethodNotFound = -32601;
constexpr std::size_t kUtf8AstralLength = 4;  // dört baytlık her dizi BMP dışıdır: UTF-16'da iki birim
constexpr std::uint32_t kSurrogatePairUnits = 2;
constexpr std::size_t kMaxUtf8Continuations = 3;
constexpr std::string_view kHexDigits = "0123456789ABCDEF";

char ascii_lower(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool equals_ignoring_case(std::string_view a, std::string_view b) {
    return std::ranges::equal(a, b, [](char x, char y) { return ascii_lower(x) == ascii_lower(y); });
}

std::string_view trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t");
    if (first == std::string_view::npos)
        return {};
    return text.substr(first, text.find_last_not_of(" \t") - first + 1);
}

// Başlık alanları "Ad: değer\r\n"; ad HTTP'deki gibi büyük/küçük harf duyarsız. Content-Type yok sayılır:
// UTF-8 tek tanımlı kodlamadır.
std::expected<std::size_t, FrameError> content_length(std::string_view header) {
    std::optional<std::size_t> length;
    while (!header.empty()) {
        const std::size_t line_end = header.find(kLineEnd);
        const std::string_view line = header.substr(0, line_end);
        header = line_end == std::string_view::npos ? std::string_view{}
                                                    : header.substr(line_end + kLineEnd.size());
        const std::size_t colon = line.find(':');
        if (colon == std::string_view::npos)
            return std::unexpected(FrameError::MalformedHeader);
        if (!equals_ignoring_case(trim(line.substr(0, colon)), kContentLength))
            continue;
        const std::string_view value = trim(line.substr(colon + 1));
        std::size_t parsed = 0;
        const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
        if (length || value.empty() || error != std::errc{} || end != value.data() + value.size())
            return std::unexpected(FrameError::BadContentLength);
        length = parsed;
    }
    if (!length)
        return std::unexpected(FrameError::MissingContentLength);
    return *length;
}

std::string json_quoted(std::string_view text) {
    std::string out;
    append_json_string(out, text);
    return out;
}

std::string notification(std::string_view method, std::string_view params) {
    std::string out(kMessageStart);
    out += R"(,"method":)" + json_quoted(method);
    if (!params.empty())
        out += R"(,"params":)" + std::string(params);
    out += '}';
    return out;
}

std::string request(std::int64_t id, std::string_view method, std::string_view params) {
    std::string out(kMessageStart);
    out += R"(,"id":)" + std::to_string(id) + R"(,"method":)" + json_quoted(method);
    if (!params.empty())
        out += R"(,"params":)" + std::string(params);
    out += '}';
    return out;
}

std::string text_document(std::string_view uri) {
    return R"({"uri":)" + json_quoted(uri) + '}';
}

const Json* field(const Json& object, const std::string& key, Json::Kind kind) {
    if (object.kind != Json::Kind::Object)
        return nullptr;
    const auto found = object.fields.find(key);
    return found != object.fields.end() && found->second.kind == kind ? &found->second : nullptr;
}

// Yalnız tamsayı belirteci: "1.5", "-1" (işaretsizde) ve "1e3" reddedilir.
template <class Integer> std::optional<Integer> integer_of(const Json* value) {
    if (value == nullptr || value->kind != Json::Kind::Number)
        return std::nullopt;
    Integer out{};
    const std::string& text = value->text;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), out);
    if (error != std::errc{} || end != text.data() + text.size())
        return std::nullopt;
    return out;
}

std::optional<Position> position_of(const Json* json) {
    if (json == nullptr)
        return std::nullopt;
    const auto line = integer_of<std::uint32_t>(field(*json, "line", Json::Kind::Number));
    const auto character = integer_of<std::uint32_t>(field(*json, "character", Json::Kind::Number));
    if (!line || !character)
        return std::nullopt;
    return Position{.line = *line, .character = *character};
}

std::optional<Range> range_of(const Json* json) {
    if (json == nullptr)
        return std::nullopt;
    const auto start = position_of(field(*json, "start", Json::Kind::Object));
    const auto end = position_of(field(*json, "end", Json::Kind::Object));
    if (!start || !end)
        return std::nullopt;
    return Range{.start = *start, .end = *end};
}

bool is_utf8_continuation(char c) {
    constexpr unsigned kContinuationMask = 0xC0;
    constexpr unsigned kContinuationBits = 0x80;
    return (static_cast<unsigned char>(c) & kContinuationMask) == kContinuationBits;
}

// Satır sonları boşluk olur; sınırı aşan mesaj bir karakterin ortasından kesilmez.
std::string one_line(std::string_view text) {
    std::string out;
    for (std::size_t i = 0; i < text.size() && out.size() <= kMaxDiagnosticMessageBytes; ++i) {
        char c = text[i];
        if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n')
            ++i;
        if (c == '\r' || c == '\n')
            c = ' ';
        out += c;
    }
    if (out.size() > kMaxDiagnosticMessageBytes) {
        std::size_t cut = kMaxDiagnosticMessageBytes;
        for (std::size_t back = 0; back < kMaxUtf8Continuations && cut > 0 && is_utf8_continuation(out[cut]);
             ++back)
            --cut;
        out.resize(cut);
    }
    return out;
}

DiagnosticSeverity severity_of(const Json* json) {
    const auto value = integer_of<int>(json);
    if (!value || *value < static_cast<int>(DiagnosticSeverity::Error) ||
        *value > static_cast<int>(DiagnosticSeverity::Hint))
        return DiagnosticSeverity::Unspecified;
    return static_cast<DiagnosticSeverity>(*value);
}

std::optional<Diagnostic> diagnostic_of(const Json& json) {
    const auto range = range_of(field(json, "range", Json::Kind::Object));
    const Json* message = field(json, "message", Json::Kind::String);
    if (!range || message == nullptr)
        return std::nullopt;
    return Diagnostic{.range = *range,
                      .severity = severity_of(field(json, "severity", Json::Kind::Number)),
                      .message = one_line(message->text)};
}

struct Utf8Step {
    std::size_t bytes;
    std::uint32_t utf16_units;
};

Utf8Step step_at(std::string_view line, std::size_t at) {
    const std::size_t length = sequence_length(static_cast<unsigned char>(line[at]));
    if (at + length <= line.size() && is_valid_utf8(line.substr(at, length)))
        return {.bytes = length, .utf16_units = length == kUtf8AstralLength ? kSurrogatePairUnits : 1U};
    return {.bytes = 1, .utf16_units = 1};
}

bool is_uri_unreserved(unsigned char byte) {
    return (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') ||
           byte == '-' || byte == '.' || byte == '_' || byte == '~';
}

}  // namespace

void FrameReader::append(std::string_view bytes) {
    // Kaydırılan bayt hiçbir zaman silinenden fazla değil: bayt başına sabit maliyet.
    if (consumed_ > 0 && consumed_ >= buffer_.size() - consumed_) {
        buffer_.erase(0, consumed_);
        consumed_ = 0;
    }
    buffer_ += bytes;
}

std::expected<std::optional<std::string>, FrameError> FrameReader::next() {
    if (error_)
        return std::unexpected(*error_);
    const auto fail = [this](FrameError error) {
        error_ = error;
        return std::unexpected(error);
    };
    const std::string_view pending = std::string_view(buffer_).substr(consumed_);
    const std::string_view window = pending.substr(0, kMaxHeaderBytes);
    const std::size_t header_end = window.find(kHeaderEnd);
    if (header_end == std::string_view::npos) {
        if (pending.size() >= kMaxHeaderBytes)
            return fail(FrameError::HeaderTooLarge);
        return std::nullopt;
    }
    const auto length = content_length(window.substr(0, header_end));
    if (!length)
        return fail(length.error());
    // Gövde gelmeden reddedilir: sunucu 16 MiB'ı aşan belleği zorlayamaz.
    if (*length > kMaxBodyBytes)
        return fail(FrameError::BodyTooLarge);
    const std::size_t body_start = header_end + kHeaderEnd.size();
    if (pending.size() - body_start < *length)
        return std::nullopt;
    std::string body(pending.substr(body_start, *length));
    consumed_ += body_start + *length;
    return body;
}

std::string frame(std::string_view body) {
    return "Content-Length: " + std::to_string(body.size()) + std::string(kHeaderEnd) + std::string(body);
}

std::optional<Json> parse_message(std::string_view body) {
    return JsonParser(body, kMessageLimits).parse();
}

PositionEncoding negotiated_encoding(const Json& initialize_result) {
    const Json* capabilities = field(initialize_result, "capabilities", Json::Kind::Object);
    const Json* encoding =
        capabilities == nullptr ? nullptr : field(*capabilities, "positionEncoding", Json::Kind::String);
    return encoding != nullptr && encoding->text == "utf-8" ? PositionEncoding::Utf8
                                                            : PositionEncoding::Utf16;
}

std::string initialize_request(std::int64_t id, std::int64_t process_id, std::string_view root_uri,
                               std::string_view initialization_options) {
    // Snippet desteği yok: öneri metni olduğu gibi eklenir.
    constexpr std::string_view kCapabilities =
        R"({"general":{"positionEncodings":["utf-8","utf-16"]},"workspace":{"configuration":true},)"
        R"("textDocument":{"synchronization":{"dynamicRegistration":false},"publishDiagnostics":{},)"
        R"("completion":{"completionItem":{"snippetSupport":false}}}})";
    std::string params = R"({"processId":)" + std::to_string(process_id) +
                         R"(,"clientInfo":{"name":"noteapp"},"rootUri":)" + json_quoted(root_uri) +
                         R"(,"capabilities":)" + std::string(kCapabilities);
    if (!initialization_options.empty())
        params += R"(,"initializationOptions":)" + std::string(initialization_options);
    return request(id, "initialize", params + '}');
}

std::string initialized_notification() {
    return notification("initialized", "{}");
}

std::string did_change_configuration_notification(std::string_view settings) {
    return notification("workspace/didChangeConfiguration", R"({"settings":)" + std::string(settings) + '}');
}

std::string bare_notification(std::string_view method) {
    return notification(method, {});
}

std::string did_open_notification(std::string_view uri, std::string_view language_id, std::int64_t version,
                                  std::string_view text) {
    return notification("textDocument/didOpen", R"({"textDocument":{"uri":)" + json_quoted(uri) +
                                                    R"(,"languageId":)" + json_quoted(language_id) +
                                                    R"(,"version":)" + std::to_string(version) +
                                                    R"(,"text":)" + json_quoted(text) + "}}");
}

std::string did_change_notification(std::string_view uri, std::int64_t version, std::string_view text) {
    return notification("textDocument/didChange", R"({"textDocument":{"uri":)" + json_quoted(uri) +
                                                      R"(,"version":)" + std::to_string(version) +
                                                      R"(},"contentChanges":[{"text":)" + json_quoted(text) +
                                                      "}]}");
}

std::string did_close_notification(std::string_view uri) {
    return notification("textDocument/didClose", R"({"textDocument":)" + text_document(uri) + '}');
}

std::string completion_request(std::int64_t id, std::string_view uri, Position position) {
    return request(id, "textDocument/completion",
                   R"({"textDocument":)" + text_document(uri) + R"(,"position":{"line":)" +
                       std::to_string(position.line) + R"(,"character":)" +
                       std::to_string(position.character) + "}}");
}

std::string shutdown_request(std::int64_t id) {
    return request(id, "shutdown", {});
}

std::string exit_notification() {
    return notification("exit", {});
}

namespace {

// VS Code'un bölüm anlamı: "a.b" -> settings.a.b. Ara adım nesne değilse ya da anahtar yoksa null.
const Json& configuration_section(const Json& settings, std::string_view section) {
    static const Json kNull{.kind = Json::Kind::Null};
    const Json* at = &settings;
    while (!section.empty()) {
        const std::size_t dot = section.find('.');
        const auto found = at->kind == Json::Kind::Object
                               ? at->fields.find(std::string(section.substr(0, dot)))
                               : at->fields.end();
        if (found == at->fields.end())
            return kNull;
        at = &found->second;
        section = dot == std::string_view::npos ? std::string_view{} : section.substr(dot + 1);
    }
    return *at;
}

}  // namespace

std::optional<std::string> reply_to_server_request(const Json& message, const Json& settings,
                                                   std::size_t max_bytes) {
    const Json* method = field(message, "method", Json::Kind::String);
    if (method == nullptr)
        return std::nullopt;
    std::string id;
    // Sayı kimliği dilbilgisinden geçmiş ham belirteçtir: olduğu gibi geri yazılır.
    if (const Json* number = field(message, "id", Json::Kind::Number))
        id = number->text;
    else if (const Json* text = field(message, "id", Json::Kind::String))
        id = json_quoted(text->text);
    else
        return std::nullopt;
    std::string out(kMessageStart);
    out += R"(,"id":)" + id;
    if (method->text == "workspace/configuration") {
        const Json* params = field(message, "params", Json::Kind::Object);
        const Json* items = params == nullptr ? nullptr : field(*params, "items", Json::Kind::Array);
        out += R"(,"result":[)";
        for (std::size_t i = 0; items != nullptr && i < items->items.size(); ++i) {
            if (out.size() > max_bytes)
                return out;  // geçersiz ve sınırı aşan: çağıran göndermez
            if (i != 0)
                out += ',';
            const Json* section = field(items->items[i], "section", Json::Kind::String);
            append_json(out, configuration_section(settings, section == nullptr ? "" : section->text));
        }
        out += "]}";
        return out;
    }
    out +=
        R"(,"error":{"code":)" + std::to_string(kMethodNotFound) + R"(,"message":"method not supported"}})";
    return out;
}

std::optional<PublishedDiagnostics> parse_publish_diagnostics(const Json& params) {
    const Json* uri = field(params, "uri", Json::Kind::String);
    const Json* list = field(params, "diagnostics", Json::Kind::Array);
    if (uri == nullptr || list == nullptr)
        return std::nullopt;
    PublishedDiagnostics out{.uri = uri->text,
                             .version =
                                 integer_of<std::int64_t>(field(params, "version", Json::Kind::Number)),
                             .diagnostics = {}};
    for (const Json& json : list->items) {
        if (out.diagnostics.size() == kMaxDiagnostics)
            break;
        if (auto diagnostic = diagnostic_of(json))
            out.diagnostics.push_back(std::move(*diagnostic));
    }
    return out;
}

std::vector<CompletionItem> parse_completion_result(const Json& result) {
    const Json* list = result.kind == Json::Kind::Array ? &result : field(result, "items", Json::Kind::Array);
    if (list == nullptr)
        return {};
    std::vector<CompletionItem> out;
    for (const Json& item : list->items) {
        if (out.size() == kMaxCompletionItems)
            break;
        const Json* label = field(item, "label", Json::Kind::String);
        if (label == nullptr)
            continue;
        const Json* edit = field(item, "textEdit", Json::Kind::Object);
        const Json* new_text = edit == nullptr ? nullptr : field(*edit, "newText", Json::Kind::String);
        const Json* insert_text = field(item, "insertText", Json::Kind::String);
        const Json* chosen = new_text != nullptr ? new_text : insert_text != nullptr ? insert_text : label;
        out.push_back(
            {.label = label->text,
             .insert_text = chosen->text,
             .kind = integer_of<std::uint32_t>(field(item, "kind", Json::Kind::Number)).value_or(0)});
    }
    return out;
}

std::size_t utf16_to_byte_column(std::string_view line, std::uint32_t utf16_column) {
    std::size_t at = 0;
    std::uint32_t units = 0;
    while (at < line.size()) {
        const Utf8Step step = step_at(line, at);
        if (units + step.utf16_units > utf16_column)
            break;
        units += step.utf16_units;
        at += step.bytes;
    }
    return at;
}

std::uint32_t byte_to_utf16_column(std::string_view line, std::size_t byte_column) {
    const std::size_t limit = std::min(byte_column, line.size());
    std::size_t at = 0;
    std::uint32_t units = 0;
    while (at < limit) {
        const Utf8Step step = step_at(line, at);
        if (at + step.bytes > limit)
            break;
        units += step.utf16_units;
        at += step.bytes;
    }
    return units;
}

std::string file_uri(const std::filesystem::path& absolute_path) {
    constexpr unsigned kNibbleBits = 4;
    constexpr unsigned kNibbleMask = 0xF;
    const std::u8string generic = absolute_path.generic_u8string();
    std::string out = "file://";
    // Windows yolu sürücüyle başlar ("C:/..."): yetki kısmı boş kalsın diye bir eğik çizgi daha.
    if (!generic.starts_with(u8'/'))
        out += '/';
    for (const char8_t unit : generic) {
        const auto byte = static_cast<unsigned char>(unit);
        if (is_uri_unreserved(byte) || byte == '/' || byte == ':') {
            out += static_cast<char>(byte);
            continue;
        }
        out += '%';
        out += kHexDigits[byte >> kNibbleBits];
        out += kHexDigits[byte & kNibbleMask];
    }
    return out;
}

}  // namespace noteapp::core::lsp
