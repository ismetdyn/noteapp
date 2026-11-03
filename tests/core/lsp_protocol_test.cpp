#include "core/json_text.hpp"
#include "core/lsp_protocol.hpp"

#include <chrono>
#include <doctest/doctest.h>
#include <string>
#include <string_view>

using namespace noteapp::core;
using namespace noteapp::core::lsp;

namespace {

Json parsed(std::string_view body) {
    auto json = parse_message(body);
    REQUIRE(json);
    return *json;
}

// Okuyucudan bir sonraki gövde; hata ya da eksik mesajda boş.
std::optional<std::string> next_body(FrameReader& reader) {
    auto next = reader.next();
    REQUIRE(next);
    return *next;
}

FrameError next_error(FrameReader& reader) {
    auto next = reader.next();
    REQUIRE_FALSE(next);
    return next.error();
}

}  // namespace

TEST_CASE("lsp framing: a message split across reads comes out once complete") {
    FrameReader reader;
    const std::string message = frame(R"({"a":1})");
    CHECK(message == "Content-Length: 7\r\n\r\n{\"a\":1}");
    reader.append(message.substr(0, 10));
    CHECK_FALSE(next_body(reader));
    reader.append(message.substr(10, 15));
    CHECK_FALSE(next_body(reader));
    reader.append(message.substr(25));
    CHECK(next_body(reader) == R"({"a":1})");
    CHECK_FALSE(next_body(reader));
}

TEST_CASE("lsp framing: several messages in one buffer come out in order; extra headers are ignored") {
    FrameReader reader;
    reader.append(frame("[1]") +
                  "content-length: 3\r\nContent-Type: application/vscode-jsonrpc; charset=utf-8\r\n\r\n[2]" +
                  frame("[3]").substr(0, 5));
    CHECK(next_body(reader) == "[1]");
    CHECK(next_body(reader) == "[2]");
    CHECK_FALSE(next_body(reader));
    reader.append(frame("[3]").substr(5));
    CHECK(next_body(reader) == "[3]");
}

TEST_CASE(
    "lsp framing: a body of exactly the limit is accepted, one byte more is refused before it arrives") {
    FrameReader at_limit;
    at_limit.append("Content-Length: " + std::to_string(kMaxBodyBytes) + "\r\n\r\n");
    CHECK_FALSE(next_body(at_limit));

    FrameReader over;
    over.append("Content-Length: " + std::to_string(kMaxBodyBytes + 1) + "\r\n\r\n");
    CHECK(next_error(over) == FrameError::BodyTooLarge);
}

TEST_CASE("lsp framing: a header block over 4 KiB is refused, with or without its end") {
    FrameReader unterminated;
    unterminated.append("Content-Length: 2\r\nX-Pad: " + std::string(kMaxHeaderBytes, 'a'));
    CHECK(next_error(unterminated) == FrameError::HeaderTooLarge);

    const std::string fits = "Content-Length: 2\r\nX: ";
    const std::string end = "\r\n\r\n";
    FrameReader exact;
    exact.append(fits + std::string(kMaxHeaderBytes - fits.size() - end.size(), 'a') + end + "{}");
    CHECK(next_body(exact) == "{}");

    FrameReader one_more;
    one_more.append(fits + std::string(kMaxHeaderBytes - fits.size() - end.size() + 1, 'a') + end + "{}");
    CHECK(next_error(one_more) == FrameError::HeaderTooLarge);
}

TEST_CASE(
    "lsp framing: missing or garbage Content-Length and malformed headers are typed errors that stick") {
    const auto error_of = [](std::string_view header) {
        FrameReader reader;
        reader.append(std::string(header) + "\r\n\r\n{}");
        return next_error(reader);
    };
    CHECK(error_of("Content-Type: x") == FrameError::MissingContentLength);
    CHECK(error_of("Content-Length: ") == FrameError::BadContentLength);
    CHECK(error_of("Content-Length: abc") == FrameError::BadContentLength);
    CHECK(error_of("Content-Length: -1") == FrameError::BadContentLength);
    CHECK(error_of("Content-Length: 2x") == FrameError::BadContentLength);
    CHECK(error_of("Content-Length: 99999999999999999999999") == FrameError::BadContentLength);
    CHECK(error_of("Content-Length: 2\r\nContent-Length: 2") == FrameError::BadContentLength);
    CHECK(error_of("garbage") == FrameError::MalformedHeader);

    FrameReader reader;
    reader.append("Content-Length: x\r\n\r\n");
    CHECK(next_error(reader) == FrameError::BadContentLength);
    reader.append(frame("{}"));
    CHECK(next_error(reader) == FrameError::BadContentLength);
}

TEST_CASE("lsp framing: one 16 MiB read of tiny frames drains in linear time") {
    const std::string tiny = frame("");
    const std::size_t count = kMaxBodyBytes / tiny.size();
    std::string bytes;
    bytes.reserve(count * tiny.size());
    for (std::size_t i = 0; i < count; ++i)
        bytes += tiny;
    FrameReader reader;
    reader.append(bytes);
    // Doğrusal okuma Debug + sanitizer'da bile bunun çok altında; her çerçevede tamponu kaydıran kod burada
    // yalnız birkaç bin çerçeve okuyabilir. Süre aşılınca döngü kesilir: test asılı kalmaz, düşer.
    constexpr auto kBound = std::chrono::seconds(10);
    const auto start = std::chrono::steady_clock::now();
    std::size_t drained = 0;
    while (std::chrono::steady_clock::now() - start < kBound) {
        auto next = reader.next();
        REQUIRE(next);
        if (!*next)
            break;
        REQUIRE((*next)->empty());
        ++drained;
    }
    CHECK(drained == count);
    reader.append(frame("[1]"));
    CHECK(next_body(reader) == "[1]");
}

TEST_CASE("lsp initialize asks for utf-8 and utf-16 and the negotiated encoding is read back") {
    const Json request = parsed(initialize_request(1, 4242, "file:///tmp/ws"));
    CHECK(request.fields.at("jsonrpc").text == "2.0");
    CHECK(request.fields.at("id").text == "1");
    CHECK(request.fields.at("method").text == "initialize");
    const Json& params = request.fields.at("params");
    CHECK(params.fields.at("processId").text == "4242");
    CHECK(params.fields.at("rootUri").text == "file:///tmp/ws");
    const Json& capabilities = params.fields.at("capabilities");
    const Json& encodings = capabilities.fields.at("general").fields.at("positionEncodings");
    REQUIRE(encodings.items.size() == 2);
    CHECK(encodings.items[0].text == "utf-8");
    CHECK(encodings.items[1].text == "utf-16");
    CHECK(capabilities.fields.at("workspace").fields.at("configuration").text == "true");
    CHECK(capabilities.fields.at("textDocument").fields.contains("publishDiagnostics"));
    CHECK(capabilities.fields.at("textDocument").fields.contains("completion"));

    CHECK(negotiated_encoding(parsed(R"({"capabilities":{"positionEncoding":"utf-8"}})")) ==
          PositionEncoding::Utf8);
    CHECK(negotiated_encoding(parsed(R"({"capabilities":{"positionEncoding":"utf-16"}})")) ==
          PositionEncoding::Utf16);
    CHECK(negotiated_encoding(parsed(R"({"capabilities":{}})")) == PositionEncoding::Utf16);
    CHECK(negotiated_encoding(parsed(R"({"capabilities":{"positionEncoding":"utf-32"}})")) ==
          PositionEncoding::Utf16);
}

TEST_CASE("lsp document notifications carry uri, version and the full text") {
    const std::string text = "x = \"ç\"\n\ttab\x1b";
    const Json open = parsed(did_open_notification("file:///a.py", "python", 1, text));
    CHECK(open.fields.at("method").text == "textDocument/didOpen");
    CHECK_FALSE(open.fields.contains("id"));
    const Json& item = open.fields.at("params").fields.at("textDocument");
    CHECK(item.fields.at("uri").text == "file:///a.py");
    CHECK(item.fields.at("languageId").text == "python");
    CHECK(item.fields.at("version").text == "1");
    CHECK(item.fields.at("text").text == text);

    const Json change = parsed(did_change_notification("file:///a.py", 2, "y"));
    CHECK(change.fields.at("method").text == "textDocument/didChange");
    const Json& change_params = change.fields.at("params");
    CHECK(change_params.fields.at("textDocument").fields.at("version").text == "2");
    REQUIRE(change_params.fields.at("contentChanges").items.size() == 1);
    const Json& whole = change_params.fields.at("contentChanges").items[0];
    CHECK(whole.fields.at("text").text == "y");
    CHECK_FALSE(whole.fields.contains("range"));

    const Json close = parsed(did_close_notification("file:///a.py"));
    CHECK(close.fields.at("method").text == "textDocument/didClose");
    CHECK(close.fields.at("params").fields.at("textDocument").fields.at("uri").text == "file:///a.py");
}

TEST_CASE("lsp completion, shutdown, initialized and exit messages") {
    const Json completion =
        parsed(completion_request(7, "file:///a.py", Position{.line = 3, .character = 5}));
    CHECK(completion.fields.at("id").text == "7");
    CHECK(completion.fields.at("method").text == "textDocument/completion");
    const Json& position = completion.fields.at("params").fields.at("position");
    CHECK(position.fields.at("line").text == "3");
    CHECK(position.fields.at("character").text == "5");

    const Json shutdown = parsed(shutdown_request(8));
    CHECK(shutdown.fields.at("method").text == "shutdown");
    CHECK(shutdown.fields.at("id").text == "8");
    CHECK(parsed(initialized_notification()).fields.at("method").text == "initialized");
    const Json exit = parsed(exit_notification());
    CHECK(exit.fields.at("method").text == "exit");
    CHECK_FALSE(exit.fields.contains("id"));
}

TEST_CASE("json_text writes a parsed value back so it parses to the same value") {
    const Json value = parsed(R"({"a":[1,-2.5e3,true,null,"q\"\\\nç"],"b":{},"c":[],"d":{"e":{"f":false}}})");
    CHECK(json_text(value) == R"({"a":[1,-2.5e3,true,null,"q\"\\\nç"],"b":{},"c":[],"d":{"e":{"f":false}}})");
    CHECK(parsed(json_text(value)) == value);
}

TEST_CASE("lsp initialize carries initializationOptions only when given") {
    const Json with = parsed(initialize_request(1, 1, "file:///ws", R"({"cargo":{"noDeps":true}})"));
    const Json& options = with.fields.at("params").fields.at("initializationOptions");
    CHECK(options.fields.at("cargo").fields.at("noDeps").text == "true");
    CHECK_FALSE(parsed(initialize_request(1, 1, "file:///ws"))
                    .fields.at("params")
                    .fields.contains("initializationOptions"));
}

TEST_CASE("lsp didChangeConfiguration and a parameterless notification") {
    const Json change = parsed(did_change_configuration_notification(R"({"yaml":{"schemas":{}}})"));
    CHECK(change.fields.at("method").text == "workspace/didChangeConfiguration");
    CHECK(change.fields.at("params").fields.at("settings").fields.at("yaml").kind == Json::Kind::Object);
    const Json bare = parsed(bare_notification("yaml/registerContentRequest"));
    CHECK(bare.fields.at("method").text == "yaml/registerContentRequest");
    CHECK_FALSE(bare.fields.contains("params"));
    CHECK_FALSE(bare.fields.contains("id"));
}

TEST_CASE("lsp configuration answers each item from the settings by dotted section") {
    const Json settings =
        parsed(R"({"rust-analyzer":{"cargo":{"noDeps":true}},"http":{"proxy":"p"},"[yaml]":{}})");
    const auto reply = reply_to_server_request(
        parsed(R"({"jsonrpc":"2.0","id":5,"method":"workspace/configuration","params":{"items":[)"
               R"({"section":"rust-analyzer"},{"section":"http.proxy"},{"section":"missing"},)"
               R"({"section":"http.proxy.deeper"},{"section":"[yaml]"},{"scopeUri":"file:///x"}]}})"),
        settings);
    REQUIRE(reply);
    const Json result = parsed(*reply).fields.at("result");
    REQUIRE(result.items.size() == 6);
    CHECK(result.items[0] == settings.fields.at("rust-analyzer"));
    CHECK(result.items[1].text == "p");
    CHECK(result.items[2].kind == Json::Kind::Null);
    CHECK(result.items[3].kind == Json::Kind::Null);
    CHECK(result.items[4].kind == Json::Kind::Object);
    CHECK(result.items[5] == settings);  // bölümsüz öğe: ayarların tamamı
}

TEST_CASE("lsp configuration reply stops growing past the pending reply limit") {
    Json settings = parsed(R"({"big":""})");
    settings.fields.at("big").text = std::string(1024, 'x');
    std::string items;
    for (int i = 0; i < 10'000; ++i)
        items += i == 0 ? R"({"section":"big"})" : R"(,{"section":"big"})";
    constexpr std::size_t kLimit = 64U * 1024U;
    const auto reply = reply_to_server_request(
        parsed(R"({"jsonrpc":"2.0","id":1,"method":"workspace/configuration","params":{"items":[)" + items +
               "]}}"),
        settings, kLimit);
    REQUIRE(reply);
    CHECK(reply->size() > kLimit);          // sınırı aştı: çağıran sunucuyu durdurur
    CHECK(reply->size() < kLimit + 2048U);  // ama 10 MiB'ı kurmadan kesildi
}

TEST_CASE("lsp server requests: configuration gets one null per item, anything else -32601, same id back") {
    const auto reply = reply_to_server_request(parsed(
        R"({"jsonrpc":"2.0","id":"c-1","method":"workspace/configuration","params":{"items":[{"section":"a"},{}]}})"));
    REQUIRE(reply);
    const Json configuration = parsed(*reply);
    CHECK(configuration.fields.at("id").kind == Json::Kind::String);
    CHECK(configuration.fields.at("id").text == "c-1");
    const Json& result = configuration.fields.at("result");
    REQUIRE(result.items.size() == 2);
    CHECK(result.items[0].kind == Json::Kind::Null);
    CHECK(result.items[1].kind == Json::Kind::Null);

    const auto other = reply_to_server_request(
        parsed(R"({"jsonrpc":"2.0","id":12,"method":"window/workDoneProgress/create","params":{}})"));
    REQUIRE(other);
    const Json error = parsed(*other);
    CHECK(error.fields.at("id").kind == Json::Kind::Number);
    CHECK(error.fields.at("id").text == "12");
    CHECK(error.fields.at("error").fields.at("code").text == "-32601");
    CHECK_FALSE(error.fields.contains("result"));

    CHECK_FALSE(
        reply_to_server_request(parsed(R"({"jsonrpc":"2.0","method":"window/logMessage","params":{}})")));
    CHECK_FALSE(reply_to_server_request(parsed(R"({"jsonrpc":"2.0","id":3,"result":null})")));
}

TEST_CASE("lsp diagnostics: range, severity, one-line message cut at 512 bytes, escape bytes kept raw") {
    const Json params = parsed(R"({"uri":"file:///a.py","version":4,"diagnostics":[
        {"range":{"start":{"line":1,"character":2},"end":{"line":1,"character":6}},"severity":2,
         "message":"first\nsecond\r\nthird\u001b[31m"},
        {"range":{"start":{"line":0}},"message":"no end"},
        {"range":{"start":{"line":0,"character":0},"end":{"line":0,"character":1}},"message":"no severity"}]})");
    const auto published = parse_publish_diagnostics(params);
    REQUIRE(published);
    CHECK(published->uri == "file:///a.py");
    CHECK(published->version == 4);
    REQUIRE(published->diagnostics.size() == 2);
    const Diagnostic& first = published->diagnostics[0];
    CHECK(first.range.start.line == 1);
    CHECK(first.range.start.character == 2);
    CHECK(first.range.end.character == 6);
    CHECK(first.severity == DiagnosticSeverity::Warning);
    CHECK(first.message == "first second third\x1b[31m");
    CHECK(published->diagnostics[1].severity == DiagnosticSeverity::Unspecified);

    CHECK_FALSE(parse_publish_diagnostics(parsed(R"({"diagnostics":[]})")));
    CHECK_FALSE(parse_publish_diagnostics(parsed(R"({"uri":"file:///a.py"})")));
}

TEST_CASE("lsp diagnostics: at most 1000, message at most 512 bytes without splitting a character") {
    const std::string one =
        R"({"range":{"start":{"line":0,"character":0},"end":{"line":0,"character":1}},"message":"m"})";
    std::string list = R"({"uri":"file:///a","diagnostics":[)";
    for (std::size_t i = 0; i < kMaxDiagnostics + 5; ++i)
        list += (i == 0 ? "" : ",") + one;
    list += "]}";
    const auto capped = parse_publish_diagnostics(parsed(list));
    REQUIRE(capped);
    CHECK(capped->diagnostics.size() == kMaxDiagnostics);

    // 511 bayt "a" ve 2 baytlık "ç": kesim karakteri bölmez, 511'de durur.
    const std::string body = std::string(kMaxDiagnosticMessageBytes - 1, 'a') + "\xC3\xA7" + "tail";
    const auto published = parse_publish_diagnostics(parsed(
        R"({"uri":"file:///a","diagnostics":[{"range":{"start":{"line":0,"character":0},"end":{"line":0,"character":1}},"message":")" +
        body + R"("}]})"));
    REQUIRE(published);
    REQUIRE(published->diagnostics.size() == 1);
    CHECK(published->diagnostics[0].message == std::string(kMaxDiagnosticMessageBytes - 1, 'a'));
}

TEST_CASE("lsp completion results: item array, CompletionList, null; textEdit wins over insertText") {
    const auto items = parse_completion_result(parsed(R"json([
        {"label":"print","kind":3,"insertText":"print(","textEdit":{"range":{"start":{"line":0,"character":0},"end":{"line":0,"character":2}},"newText":"print()"}},
        {"label":"pass","insertText":"pass"},
        {"label":"lambda","textEdit":{"insert":{"start":{"line":0,"character":0},"end":{"line":0,"character":1}},"replace":{"start":{"line":0,"character":0},"end":{"line":0,"character":2}},"newText":"lambda:"}},
        {"label":"plain\u001b"},
        {"kind":1}])json"));
    REQUIRE(items.size() == 4);
    CHECK(items[0].label == "print");
    CHECK(items[0].insert_text == "print()");
    CHECK(items[0].kind == 3);
    CHECK(items[1].insert_text == "pass");
    CHECK(items[1].kind == 0);
    CHECK(items[2].insert_text == "lambda:");
    CHECK(items[3].label == "plain\x1b");
    CHECK(items[3].insert_text == "plain\x1b");

    const auto list = parse_completion_result(parsed(R"({"isIncomplete":true,"items":[{"label":"a"}]})"));
    REQUIRE(list.size() == 1);
    CHECK(list[0].label == "a");
    CHECK(parse_completion_result(parsed("null")).empty());
    CHECK(parse_completion_result(parsed(R"({"items":3})")).empty());
}

TEST_CASE("lsp completion results are capped") {
    std::string many = "[";
    for (std::size_t i = 0; i < kMaxCompletionItems + 10; ++i)
        many += std::string(i == 0 ? "" : ",") + R"({"label":"x"})";
    many += "]";
    CHECK(parse_completion_result(parsed(many)).size() == kMaxCompletionItems);
}

TEST_CASE("lsp utf-16 columns: astral characters count two units and are never split") {
    // a, ç (2 bayt), € (3 bayt), 😀 (4 bayt, 2 birim), b
    const std::string line = "a\xC3\xA7\xE2\x82\xAC\xF0\x9F\x98\x80"
                             "b";
    CHECK(utf16_to_byte_column(line, 0) == 0);
    CHECK(utf16_to_byte_column(line, 1) == 1);
    CHECK(utf16_to_byte_column(line, 2) == 3);
    CHECK(utf16_to_byte_column(line, 3) == 6);
    CHECK(utf16_to_byte_column(line, 4) == 6);  // surrogate çiftinin ortası: karakterin başı
    CHECK(utf16_to_byte_column(line, 5) == 10);
    CHECK(utf16_to_byte_column(line, 6) == 11);
    CHECK(utf16_to_byte_column(line, 99) == 11);

    CHECK(byte_to_utf16_column(line, 0) == 0);
    CHECK(byte_to_utf16_column(line, 3) == 2);
    CHECK(byte_to_utf16_column(line, 6) == 3);
    CHECK(byte_to_utf16_column(line, 8) == 3);  // 😀'nun ortası: karakterin başı
    CHECK(byte_to_utf16_column(line, 10) == 5);
    CHECK(byte_to_utf16_column(line, 11) == 6);
    CHECK(byte_to_utf16_column(line, 99) == 6);

    // Geçersiz bayt bir birim.
    CHECK(utf16_to_byte_column("\xFF"
                               "a",
                               1) == 1);
    CHECK(byte_to_utf16_column("\xFF"
                               "a",
                               2) == 2);
}

TEST_CASE("lsp file_uri percent-encodes spaces, non-ASCII, # and ? and keeps the drive form") {
    CHECK(file_uri("/tmp/a b/\xC3\xA7#?.md") == "file:///tmp/a%20b/%C3%A7%23%3F.md");
    CHECK(file_uri("/x/A-Z_a.z~0/%") == "file:///x/A-Z_a.z~0/%25");
    CHECK(file_uri("C:/Users/x y/n.py") == "file:///C:/Users/x%20y/n.py");
#ifdef _WIN32
    CHECK(file_uri("C:\\Users\\x y\\n.py") == "file:///C:/Users/x%20y/n.py");
#endif
}
