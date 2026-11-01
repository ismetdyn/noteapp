// Oplog güvenlik incelemesinin bulguları (G2b). Her test, düzeltmeden önce düşen senaryodur.
#include "graph/merge.hpp"
#include "graph/op.hpp"
#include "graph/oplog.hpp"
#include "graph/store.hpp"
#include "sync_test_support.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <system_error>
#include <vector>

using namespace noteapp::graph;
using namespace noteapp::graph::detail;
using noteapp::test::TempDir;
using test::Device;
using test::edge_named;
using test::node_named;
using test::write_log;

namespace fs = std::filesystem;

namespace {

const NodeId kN = node_named("n");
const NodeId kM = node_named("m");
const EdgeId kE = edge_named("e");
const DeviceId kD1{std::byte{1}};
const DeviceId kD2{std::byte{2}};

Frame frame_of(std::uint64_t seq, std::vector<Op> ops, std::vector<Dot> deps = {}) {
    Frame frame;
    frame.seq = seq;
    frame.hlc = seq + 1;
    frame.deps = std::move(deps);
    frame.ops = std::move(ops);
    return frame;
}

bool reports_reason(const MergeReport& report, std::string_view reason) {
    return std::ranges::any_of(report.bad_frames,
                               [&](const BadFrameReport& bad) { return bad.reason == reason; });
}

}  // namespace

// Bulgu 2, birinci yol: aynı çerçevede iki kimlik. Çerçeve kimseyi durduramaz, etkisi her cihazda aynıdır.
TEST_CASE("frame_with_two_identities_for_one_edge_does_not_fail_ingest") {
    const TempDir transit;
    const std::vector<std::string> frames{encode_frame(
        frame_of(1, {NodePut{kN}, NodePut{kM}, EdgePut{kE, kN, "A", kM}, EdgePut{kE, kN, "B", kM}}))};
    const fs::path log = write_log(transit.path(), kD1, frames);

    Device first;
    const MergeReport report = first.ingest({log});
    CHECK(report.stalled.empty());
    CHECK(reports_reason(report, "edge_identity_conflict"));
    Device second;
    second.ingest({log});
    CHECK(first.dump() == second.dump());
    CHECK(first.dump().find("edge ") != std::string::npos);
    CHECK(first.ingest({log}).bad_frames.empty());  // tekrar uygulama etkisiz
}

// İkinci yol: ilk görülen kimlik B, yeniden kurma satıra A'yı yazar; sonraki oturumda B gelir.
TEST_CASE("put_of_the_losing_identity_after_a_rebuild_does_not_fail_ingest") {
    const TempDir transit;
    const std::vector<std::string> frames_1{
        encode_frame(frame_of(1, {NodePut{kN}, NodePut{kM}, EdgePut{kE, kN, "B", kM}}))};
    const std::vector<std::string> frames_2{
        encode_frame(frame_of(1, {NodePut{kN}, NodePut{kM}, EdgePut{kE, kN, "A", kM}}))};
    const fs::path log_1 = write_log(transit.path() / "first", kD1, frames_1);
    const fs::path log_2 = write_log(transit.path() / "first", kD2, frames_2);

    Device device;
    device.ingest({log_1});
    device.ingest({log_2});

    const std::vector<std::string> frames_1_more{frames_1[0],
                                                 encode_frame(frame_of(2, {EdgePut{kE, kN, "B", kM}}))};
    const fs::path log_1_more = write_log(transit.path() / "second", kD1, frames_1_more);
    const MergeReport report = device.ingest({log_1_more});
    CHECK(report.stalled.empty());
    CHECK(reports_reason(report, "edge_identity_conflict"));

    Device reversed;
    reversed.ingest({log_2, log_1_more});
    CHECK(device.dump() == reversed.dump());
}

// Bulgu 3: dosya başına küçük bir çerçeve, yerel yazmayı rehin alamaz.
TEST_CASE("flood_of_devices_does_not_block_local_commits_and_honest_devices_converge") {
    constexpr int kFlood = 1025;
    const TempDir transit;
    std::vector<fs::path> flood;
    for (int i = 0; i < kFlood; ++i) {
        DeviceId device{};
        device[0] = static_cast<std::byte>(0xF0);
        device[1] = static_cast<std::byte>(i & 0xFF);
        device[2] = static_cast<std::byte>(i >> 8);
        const std::vector<std::string> frames{encode_frame(frame_of(1, {NodePut{node_named("f")}}))};
        flood.push_back(write_log(transit.path(), device, frames));
    }

    Device a;
    Device b;
    a.commit([](WriteTxn& txn) { GRAPH_OK(txn.put_node(kN)); });
    b.ingest({a.exported()});
    b.commit([](WriteTxn& txn) { GRAPH_OK(txn.put_node(kM)); });
    a.ingest({b.exported()});

    const auto with_flood = [&](std::vector<fs::path> logs) {
        logs.insert(logs.end(), flood.begin(), flood.end());
        return logs;
    };
    const MergeReport report = a.ingest(with_flood({b.log_path()}));
    b.ingest(with_flood({a.log_path()}));
    CHECK(std::ranges::any_of(report.stalled, [](const StalledLog& s) { return s.reason == "peer_limit"; }));

    a.commit([](WriteTxn& txn) { GRAPH_OK(txn.set_property(kN, "x", std::int64_t{1})); });
    b.commit([](WriteTxn& txn) { GRAPH_OK(txn.set_property(kM, "y", std::int64_t{2})); });
    a.ingest(with_flood({b.exported(), a.exported()}));
    b.ingest(with_flood({a.log_path(), b.log_path()}));
    CHECK(a.dump() == b.dump());
    ReadTxn read = test::begin_read(a.graph());
    CHECK(*read.property(kN, "x") == Value{std::int64_t{1}});
    CHECK(*read.property(kM, "y") == Value{std::int64_t{2}});
}

// Bulgu 4: yeniden kurma batch'ler arasında ertelenir. Bayrak kalıcıdır: arada çökülürse sonraki ingest
// tamamlar.
TEST_CASE("interrupted_rebuild_finishes_on_the_next_ingest_even_with_no_new_frames") {
    Device a;
    a.commit([](WriteTxn& txn) { GRAPH_OK(txn.put_node(kN)); });
    const std::string before = a.dump();
    a.close();
    {
        Result<std::unique_ptr<Store>> opened = Store::open(a.db());
        REQUIRE(opened.has_value());
        GRAPH_OK((*opened)->begin_write());
        GRAPH_OK((*opened)->clear_graph());
        GRAPH_OK((*opened)->set_rebuild_pending(true));
        GRAPH_OK((*opened)->commit_write());
    }
    a.reopen();
    CHECK(a.dump() != before);
    CHECK(a.ingest({}).bad_frames.empty());
    CHECK(a.dump() == before);

    Result<std::unique_ptr<Store>> checked = Store::open(a.db());
    REQUIRE(checked.has_value());
    CHECK_FALSE(*(*checked)->rebuild_pending());
}

namespace {

// Wine ve yetkisiz Windows sembolik bağ kuramayabilir: test o ortamda boş geçmesin, görünür atlasın.
bool make_symlink(const fs::path& target, const fs::path& link) {
    std::error_code code;
    fs::create_symlink(target, link, code);
    if (code)
        MESSAGE("symlink unavailable here, scenario skipped: " << code.message());
    return !code;
}

// Bu cihazın dosya adı: ops boşken de bilinsin diye önce bir çerçeve yazılır ve export edilir.
fs::path log_with_one_frame(Device& device) {
    device.commit([](WriteTxn& txn) { GRAPH_OK(txn.put_node(kN)); });
    return device.exported();
}

fs::path only_entry_with_name_part(const fs::path& directory, std::string_view part) {
    for (const fs::directory_entry& entry : fs::directory_iterator(directory))
        if (entry.path().filename().string().find(part) != std::string::npos)
            return entry.path();
    return {};
}

}  // namespace

// Bulgu 1: depodaki sembolik bağ ev dizinindeki dosyanın üzerine yazdıramaz.
TEST_CASE("symlinked_temp_name_does_not_redirect_a_log_rewrite") {
    Device device;
    const fs::path log = log_with_one_frame(device);
    const fs::path victim = device.dir() / "victim.txt";
    test::write_file(victim, "untouched");
    fs::path temporary = log;
    temporary += ".tmp";
    if (!make_symlink(victim, temporary))
        return;
    test::write_file(log, "garbage forces a rewrite");

    GRAPH_OK(device.graph().export_log(device.outbox()));
    CHECK(noteapp::test::slurp(victim) == "untouched");
    CHECK(noteapp::test::slurp(log).starts_with("NGOL"));
}

TEST_CASE("symlinked_log_is_neither_followed_nor_appended_to") {
    Device device;
    const fs::path log = log_with_one_frame(device);
    const std::string real_log = noteapp::test::slurp(log);
    const fs::path victim = device.dir() / "victim.txt";
    test::write_file(victim, "untouched");
    fs::remove(log);
    if (!make_symlink(victim, log))
        return;

    device.commit([](WriteTxn& txn) { GRAPH_OK(txn.add_label(kN, "L")); });
    const Result<ExportReport> exported = device.graph().export_log(device.outbox());
    REQUIRE(exported.has_value());
    CHECK(noteapp::test::slurp(victim) == "untouched");
    CHECK_FALSE(fs::is_symlink(log));
    CHECK(noteapp::test::slurp(log).starts_with(real_log));
    REQUIRE(exported->set_aside.has_value());
    CHECK(fs::is_symlink(*exported->set_aside));
}

TEST_CASE("ingest_refuses_a_symlinked_log") {
    Device source;
    const fs::path log = log_with_one_frame(source);
    Device target;
    const fs::path inbox = target.dir() / log.filename();
    if (!make_symlink(log, inbox))
        return;
    const MergeReport report = target.ingest({inbox});
    REQUIRE(report.bad_frames.size() == 1);
    CHECK(report.bad_frames[0].reason == "symlink_refused");
    CHECK(report.heads.empty());
    CHECK(target.dump().empty());
}

TEST_CASE("symlinked_log_directory_is_refused_for_export_and_ingest") {
    Device source;
    const fs::path log = log_with_one_frame(source);
    const fs::path real_directory = source.dir() / "elsewhere";
    fs::create_directories(real_directory);
    const fs::path linked = source.dir() / "graph";
    if (!make_symlink(real_directory, linked))
        return;
    const Result<ExportReport> exported = source.graph().export_log(linked);
    REQUIRE_FALSE(exported.has_value());
    CHECK(fs::is_empty(real_directory));

    fs::copy_file(log, real_directory / log.filename());
    Device target;
    const MergeReport report = target.ingest({linked / log.filename()});
    REQUIRE(report.bad_frames.size() == 1);
    CHECK(report.bad_frames[0].reason == "log_directory_symlink");
    CHECK(target.dump().empty());
}

// Bulgu 6: yarım kuyruk ve kısa başlık raporda görünür; sessiz takılma yok.
TEST_CASE("trailing_huge_length_is_reported_as_a_partial_tail") {
    const TempDir transit;
    const std::vector<std::string> frames{encode_frame(frame_of(1, {NodePut{kN}}))};
    const fs::path log = write_log(transit.path(), kD1, frames);
    const std::uintmax_t whole = fs::file_size(log);
    {
        std::ofstream out(log, std::ios::binary | std::ios::app);
        std::string length(4, '\0');
        const auto over = static_cast<std::uint32_t>(kMaxFrameBytes);
        for (std::size_t i = 0; i < length.size(); ++i)
            length[i] = static_cast<char>(over >> (i * 8U));
        out << length << std::string(kFrameDigestBytes, 'x');
        REQUIRE(out.good());
    }
    Device device;
    const MergeReport report = device.ingest({log});
    REQUIRE(report.partial_tails.size() == 1);
    CHECK(report.partial_tails[0].valid_bytes == whole);
    CHECK(report.partial_tails[0].file_bytes == fs::file_size(log));
    CHECK(report.partial_tails[0].device == kD1);
    CHECK(report.bad_frames.empty());
}

TEST_CASE("short_header_is_reported_as_a_partial_tail") {
    Device device;
    const fs::path log = device.dir() / log_file_name(kD1);
    test::write_file(log, "NGO");
    const MergeReport report = device.ingest({log});
    REQUIRE(report.partial_tails.size() == 1);
    CHECK(report.partial_tails[0].valid_bytes == 0);
    CHECK(report.partial_tails[0].file_bytes == 3);
}

TEST_CASE("heads_expose_a_rolled_back_device") {
    const TempDir transit;
    const std::vector<std::string> frames{encode_frame(frame_of(1, {NodePut{kN}})),
                                          encode_frame(frame_of(2, {NodePut{kM}}))};
    const fs::path full = write_log(transit.path() / "full", kD1, frames);
    const fs::path older = write_log(transit.path() / "older", kD1, std::span(frames).first(1));

    Device device;
    const MergeReport caught_up = device.ingest({full});
    REQUIRE(caught_up.heads.size() == 1);
    CHECK(caught_up.heads[0].device == kD1);
    CHECK(caught_up.heads[0].log_seq == 2);
    CHECK(caught_up.heads[0].applied_seq == 2);

    const MergeReport rolled_back = device.ingest({older});
    REQUIRE(rolled_back.heads.size() == 1);
    CHECK(rolled_back.heads[0].log_seq == 1);
    CHECK(rolled_back.heads[0].applied_seq == 2);
}

// Bulgu 7: yerel günlükteki sahtecilik yerel yazmayı ve export'u durduramaz.
TEST_CASE("forged_frames_in_the_local_log_are_set_aside_and_export_continues") {
    const auto forge_and_export = [](Device& device, const std::string& forged_log, std::string_view reason) {
        const fs::path log = device.log_path();
        test::write_file(log, forged_log);
        const Result<ExportReport> exported = device.graph().export_log(device.outbox());
        REQUIRE(exported.has_value());
        REQUIRE(exported->set_aside.has_value());
        CHECK(exported->reason == reason);
        CHECK(noteapp::test::slurp(*exported->set_aside) == forged_log);
        Device fresh;
        CHECK(fresh.ingest({log}).bad_frames.empty());
        CHECK(fresh.dump() == device.dump());
    };
    Device device;
    device.commit([](WriteTxn& txn) { GRAPH_OK(txn.put_node(kN)); });
    device.commit([](WriteTxn& txn) { GRAPH_OK(txn.add_label(kN, "L")); });
    device.commit([](WriteTxn& txn) { GRAPH_OK(txn.add_label(kN, "M")); });
    const std::string real = noteapp::test::slurp(device.exported());
    const DeviceId self = [&] {
        DeviceId id{};
        std::memcpy(id.data(), real.data() + 5, id.size());
        return id;
    }();
    const std::string header = encode_log_header(self);
    const std::string forged_payload = encode_frame(frame_of(2, {NodePut{kM}}));
    const std::vector<std::string> real_frames = [&] {
        std::vector<std::string> out;
        std::size_t at = kLogHeaderBytes;
        while (at < real.size()) {
            std::uint32_t size = 0;
            std::memcpy(&size, real.data() + at, sizeof size);
            out.push_back(real.substr(at + kFrameEnvelopeBytes, size));
            at += kFrameEnvelopeBytes + size;
        }
        return out;
    }();
    REQUIRE(real_frames.size() == 3);

    SUBCASE("forged_last_frame") {
        forge_and_export(device,
                         header + encode_log_frame(real_frames[0]) + encode_log_frame(real_frames[1]) +
                             encode_log_frame(encode_frame(frame_of(3, {NodePut{kM}}))),
                         "device_forked");
    }
    SUBCASE("forged_middle_frame_with_a_real_last_frame") {
        forge_and_export(device,
                         header + encode_log_frame(real_frames[0]) + encode_log_frame(forged_payload) +
                             encode_log_frame(real_frames[2]),
                         "device_forked");
    }
    SUBCASE("forged_frame_ahead_of_the_database") {
        forge_and_export(device, real + encode_log_frame(encode_frame(frame_of(4, {NodePut{kM}}))),
                         "log_ahead_of_store");
    }
    SUBCASE("frames_of_another_device") {
        forge_and_export(device, encode_log_header(kD2) + encode_log_frame(forged_payload), "foreign_device");
    }
    // Yerel yazma ve sonraki export sürer.
    device.commit([](WriteTxn& txn) { GRAPH_OK(txn.add_label(kN, "After")); });
    Device fresh;
    CHECK(fresh.ingest({device.exported()}).bad_frames.empty());
    CHECK(fresh.dump() == device.dump());
    CHECK_FALSE(only_entry_with_name_part(device.outbox(), ".aside-").empty());
}

// Bulgu 5: toplam bayt bütçesi. Aşınca kalan günlük durur ve bildirilir; sonraki ingest kaldığı yerden sürer.
TEST_CASE("ingest_byte_budget_stops_and_the_next_ingest_continues") {
    const TempDir transit;
    std::vector<std::string> frames;
    frames.push_back(encode_frame(frame_of(1, {NodePut{kN}})));
    for (std::uint64_t seq = 2; seq <= 5; ++seq)
        frames.push_back(encode_frame(frame_of(seq, {PropSet{kN, "x", Bytes{std::string(1000, 'x')}}})));
    const fs::path log = write_log(transit.path(), kD1, frames);

    Device device;
    device.close();
    Result<std::unique_ptr<Store>> opened = Store::open(device.db());
    REQUIRE(opened.has_value());
    const std::vector<fs::path> logs{log};
    const IngestLimits small{2200};
    const Result<MergeReport> limited = detail::ingest_logs(**opened, logs, small);
    REQUIRE(limited.has_value());
    REQUIRE(limited->stalled.size() == 1);
    CHECK(limited->stalled[0].reason == "ingest_budget");
    CHECK(limited->stalled[0].applied_seq >= 1);
    CHECK(limited->stalled[0].applied_seq < 5);

    const Result<MergeReport> rest = detail::ingest_logs(**opened, logs, IngestLimits{});
    REQUIRE(rest.has_value());
    CHECK(rest->stalled.empty());
    CHECK(*(*opened)->applied_seq(kD1) == 5);
}

namespace {

Result<MergeReport> ingest_with(Store& store, const fs::path& log, const IngestLimits& limits) {
    const std::vector<fs::path> logs{log};
    return detail::ingest_logs(store, logs, limits);
}

std::unique_ptr<Store> open_store(Device& device) {
    device.close();
    Result<std::unique_ptr<Store>> opened = Store::open(device.db());
    REQUIRE(opened.has_value());
    return std::move(*opened);
}

}  // namespace

// Her ingest başlıktan okur: uygulanmış çerçeveler bütçeyi yerse en küçük kimlikli büyük günlük ötekileri
// sonsuza dek bekletirdi.
TEST_CASE("frames already applied do not eat the ingest budget") {
    const TempDir transit;
    std::vector<std::string> frames;
    frames.push_back(encode_frame(frame_of(1, {NodePut{kN}})));
    for (std::uint64_t seq = 2; seq <= 5; ++seq)
        frames.push_back(encode_frame(frame_of(seq, {PropSet{kN, "x", Bytes{std::string(1000, 'x')}}})));
    const fs::path log = write_log(transit.path(), kD1, frames);

    Device device;
    const auto store = open_store(device);
    REQUIRE(ingest_with(*store, log, IngestLimits{})->stalled.empty());

    frames.push_back(encode_frame(frame_of(6, {PropSet{kN, "x", Bytes{std::string(1000, 'y')}}})));
    write_log(transit.path(), kD1, frames);
    const auto report = ingest_with(*store, log, IngestLimits{2000});
    REQUIRE(report.has_value());
    CHECK(report->stalled.empty());
    CHECK(*store->applied_seq(kD1) == 6);
}

// Sırayla tekrar ops'taki baytlarla sınırlıdır; aynı çerçeveyi yineleyen günlük bedava okutamaz.
TEST_CASE("a frame repeated inside a log still counts against the ingest budget") {
    const TempDir transit;
    const std::vector<std::string> first{encode_frame(frame_of(1, {NodePut{kN}}))};
    const std::vector<std::string> repeated(5, first.front());
    const fs::path log = write_log(transit.path(), kD1, first);
    Device device;
    const auto store = open_store(device);
    REQUIRE(ingest_with(*store, log, IngestLimits{})->stalled.empty());
    write_log(transit.path(), kD1, repeated);
    const auto report = ingest_with(*store, log, IngestLimits{first.front().size() * 2});
    REQUIRE(report.has_value());
    REQUIRE(report->stalled.size() == 1);
    CHECK(report->stalled[0].reason == "ingest_budget");
}

// Merger sahip başına durum tutar: sınırsız yeni kimlik belleği bitirirdi.
TEST_CASE("the merge owner limit stops the log with a report, also when stored history is already over it") {
    const TempDir transit;
    const auto node = [](int n) { return Op{NodePut{node_named("n" + std::to_string(n))}}; };
    std::vector<std::string> frames{encode_frame(frame_of(1, {node(1), node(2), node(3)})),
                                    encode_frame(frame_of(2, {node(4), node(5), node(6)}))};
    const fs::path log = write_log(transit.path(), kD1, frames);
    Device device;
    const auto store = open_store(device);

    const auto limited = ingest_with(*store, log, IngestLimits{.max_merge_owners = 4});
    REQUIRE(limited.has_value());
    REQUIRE(limited->stalled.size() == 1);
    CHECK(limited->stalled[0].reason == "merge_limit");
    CHECK(*store->applied_seq(kD1) == 1);

    REQUIRE(ingest_with(*store, log, IngestLimits{})->stalled.empty());
    CHECK(*store->applied_seq(kD1) == 2);

    frames.push_back(encode_frame(frame_of(3, {node(7)})));
    write_log(transit.path(), kD1, frames);
    const auto over = ingest_with(*store, log, IngestLimits{.max_merge_owners = 4});
    REQUIRE(over.has_value());
    REQUIRE(over->stalled.size() == 1);
    CHECK(over->stalled[0].reason == "merge_limit");
    CHECK(*store->applied_seq(kD1) == 2);
}

TEST_CASE("set-aside logs are capped: the newest stay, older copies go") {
    Device device;
    device.commit([](WriteTxn& txn) { GRAPH_OK(txn.put_node(kN)); });
    const fs::path log = device.exported();
    fs::path newest;
    for (int i = 0; i < 6; ++i) {
        std::ofstream(log, std::ios::binary) << std::string(64, 'z') << i;
        const auto exported = device.graph().export_log(device.outbox());
        REQUIRE(exported.has_value());
        REQUIRE(exported->set_aside.has_value());
        newest = *exported->set_aside;
    }
    std::size_t kept = 0;
    for (const fs::directory_entry& entry : fs::directory_iterator(device.outbox()))
        kept += entry.path().filename().string().find(".aside-") != std::string::npos ? 1 : 0;
    CHECK(kept == 3);
    CHECK(fs::exists(newest));
}
