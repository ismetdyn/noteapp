// F4: not ve ilişki özellikleri (SPEC-graph "Özellikler"). Okuma, korumalı yazma (sorgu çubuğuyla aynı
// koruma), değer metninin gidiş-dönüşü, `prop:` araması için not listesi ve iki cihazın eşzamanlı yazması.
#include "core/config.hpp"
#include "core/note_props.hpp"
#include "core/note_query.hpp"
#include "core/note_store.hpp"
#include "core/note_store_impl.hpp"
#include "core/note_sync.hpp"
#include "support/note_seed.hpp"
#include "support/test_support.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <doctest/doctest.h>
#include <limits>
#include <string>
#include <thread>
#include <variant>
#include <vector>

using namespace noteapp::core;
using noteapp::test::seed_note;
using noteapp::test::TempDir;
namespace fs = std::filesystem;
namespace graph = noteapp::graph;

namespace {

NotePath np(const std::string& text) {
    return *NotePath::parse(text);
}

std::string arg_of(const Error& error, std::string_view name) {
    for (const auto& [key, value] : error.args)
        if (key == name)
            return value;
    return {};
}

const Property* find_prop(const std::vector<Property>& props, std::string_view key) {
    const auto found = std::ranges::find(props, key, &Property::key);
    return found == props.end() ? nullptr : &*found;
}

std::vector<Property> props_of(const NoteStore& store, const std::string& path) {
    auto props = note_properties(store, np(path));
    REQUIRE(props.has_value());
    return std::move(*props);
}

RelationId only_relation(const NoteStore& store, const std::string& from) {
    const auto links = store.links(np(from));
    REQUIRE(links.has_value());
    REQUIRE(links->related.size() == 1);
    return links->related.front().id;
}

void later() {  // eşzamanlı iki yazma farklı HLC alsın: LWW kazananı belirli olsun
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
}

struct Devices {
    TempDir logs;
    TempDir data_a;
    TempDir data_b;
    NoteStore a{data_a.path()};
    NoteStore b{data_b.path()};

    Devices() {
        REQUIRE(seed_note(a, "x.md", "x\n"));
        REQUIRE(seed_note(a, "y.md", "y\n"));
        send(a, b);
    }
    LogIngest send(NoteStore& from, NoteStore& to) {
        REQUIRE(export_note_log(from, logs.path()).has_value());
        const auto name = own_log_name(from);
        REQUIRE(name.has_value());
        const std::vector<fs::path> files{logs.path() / *name};
        auto ingested = ingest_note_logs(to, files);
        REQUIRE(ingested.has_value());
        return std::move(*ingested);
    }
};

}  // namespace

TEST_CASE("note properties: typed values are added, edited and removed one write at a time") {
    TempDir dir;
    NoteStore store(dir.path());
    REQUIRE(seed_note(store, "a.md", "a\n"));
    REQUIRE(set_note_property(store, np("a.md"), "status", graph::Value{std::string("draft")}).has_value());
    REQUIRE(set_note_property(store, np("a.md"), "priority", graph::Value{std::int64_t{3}}).has_value());
    REQUIRE(set_note_property(store, np("a.md"), "score", graph::Value{2.5}).has_value());
    REQUIRE(set_note_property(store, np("a.md"), "done", graph::Value{false}).has_value());

    auto props = props_of(store, "a.md");
    REQUIRE(find_prop(props, "status") != nullptr);
    CHECK(find_prop(props, "status")->value == graph::Value{std::string("draft")});
    CHECK_FALSE(find_prop(props, "status")->read_only);
    CHECK(find_prop(props, "priority")->value == graph::Value{std::int64_t{3}});
    CHECK(find_prop(props, "score")->value == graph::Value{2.5});
    CHECK(find_prop(props, "done")->value == graph::Value{false});
    CHECK(std::ranges::is_sorted(props, {}, &Property::key));

    REQUIRE(set_note_property(store, np("a.md"), "status", graph::Value{std::string("final")}).has_value());
    REQUIRE(set_note_property(store, np("a.md"), "done", graph::Value{}).has_value());
    props = props_of(store, "a.md");
    CHECK(find_prop(props, "status")->value == graph::Value{std::string("final")});
    CHECK(find_prop(props, "done") == nullptr);

    // Yeniden açılan depo aynı değerleri görür (başka süreç).
    NoteStore reopened(dir.path());
    CHECK(find_prop(props_of(reopened, "a.md"), "status")->value == graph::Value{std::string("final")});
}

TEST_CASE("note properties: the app's keys are listed read-only and every write to them is refused") {
    TempDir dir;
    NoteStore store(dir.path());
    REQUIRE(seed_note(store, "a.md", "a\n"));
    const auto props = props_of(store, "a.md");
    for (const char* key : {"name", "type", "created_at", "updated_at"}) {
        INFO(key);
        REQUIRE(find_prop(props, key) != nullptr);
        CHECK(find_prop(props, key)->read_only);
    }
    for (const char* key :
         {"name", "type", "body", "created_at", "updated_at", "link_targets", "deleted_at"}) {
        INFO(key);
        const auto refused = set_note_property(store, np("a.md"), key, graph::Value{std::string("x")});
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().code == Error::Code::InvalidPath);
        CHECK(refused.error().message_key == "error.property_reserved");
        CHECK(arg_of(refused.error(), "key") == key);
        CHECK_FALSE(set_note_property(store, np("a.md"), key, graph::Value{}).has_value());
    }
    CHECK(store.read(np("a.md"))->text == "a\n");
    CHECK(store.list().front().path == np("a.md"));
}

TEST_CASE("note properties: keys follow the graph's name rule and values must be valid") {
    TempDir dir;
    NoteStore store(dir.path());
    REQUIRE(seed_note(store, "a.md", "a\n"));
    const std::pair<std::string, std::string> bad_keys[] = {
        {"", "empty_name"},
        {std::string(257, 'k'), "name_too_long"},
        {"\xFF", "name_not_utf8"},
        {std::string("a\0b", 3), "name_has_nul"},
    };
    for (const auto& [key, reason] : bad_keys) {
        INFO(reason);
        const auto refused = set_note_property(store, np("a.md"), key, graph::Value{std::string("x")});
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().message_key == "error.property_key");
        CHECK(arg_of(refused.error(), "reason") == reason);
    }
    CHECK(set_note_property(store, np("a.md"), std::string(256, 'k'), graph::Value{true}).has_value());
    const auto bad_text = set_note_property(store, np("a.md"), "k", graph::Value{std::string("\xC3")});
    REQUIRE_FALSE(bad_text.has_value());
    CHECK(bad_text.error().message_key == "error.property_value");

    const auto missing_key = set_note_property(store, np("a.md"), "never", graph::Value{});
    REQUIRE_FALSE(missing_key.has_value());
    CHECK(missing_key.error().code == Error::Code::NotFound);
    CHECK(missing_key.error().message_key == "error.property_not_found");
    const auto missing_note = set_note_property(store, np("zz.md"), "k", graph::Value{true});
    REQUIRE_FALSE(missing_note.has_value());
    CHECK(missing_note.error().code == Error::Code::NotFound);
    CHECK(note_properties(store, np("zz.md")).error().code == Error::Code::NotFound);
}

TEST_CASE("note properties: the inspector refuses exactly the keys the query bar refuses") {
    TempDir dir;
    NoteStore store(dir.path());
    REQUIRE(seed_note(store, "a.md", "a\n"));
    for (const char* key :
         {"name", "type", "body", "created_at", "updated_at", "origin", "deleted_at", "trashed_from",
          "body_parents", "link_targets", "status", "owner", "display", "query", "directed"}) {
        INFO(key);
        const std::string cypher = std::string("MATCH (n:Note) SET n.`") + key + "` = 'x'";
        const auto by_query = run_user_query(store, cypher, QueryAction::Preview, {});
        const auto by_inspector = set_note_property(store, np("a.md"), key, graph::Value{std::string("x")});
        CHECK(by_query.has_value() == by_inspector.has_value());
        if (!by_query) {
            CHECK(by_query.error().message_key == "error.query_protected_property");
            CHECK(by_inspector.error().message_key == "error.property_reserved");
        }
    }
}

TEST_CASE("relation properties: user keys round-trip, name and direction stay read-only") {
    TempDir dir;
    NoteStore store(dir.path());
    REQUIRE(seed_note(store, "a.md", "a\n"));
    REQUIRE(seed_note(store, "b.md", "b\n"));
    REQUIRE(store.add_relation(np("a.md"), np("b.md"), {"uses", true}).has_value());
    const RelationId id = only_relation(store, "a.md");
    REQUIRE(set_relation_property(store, id, "since", graph::Value{std::int64_t{2024}}).has_value());
    REQUIRE(set_relation_property(store, id, "note", graph::Value{std::string("weekly")}).has_value());

    auto props = relation_properties(store, id);
    REQUIRE(props.has_value());
    CHECK(find_prop(*props, "since")->value == graph::Value{std::int64_t{2024}});
    REQUIRE(find_prop(*props, "name") != nullptr);
    CHECK(find_prop(*props, "name")->read_only);
    CHECK(find_prop(*props, "directed")->read_only);
    CHECK_FALSE(find_prop(*props, "note")->read_only);

    for (const char* key : {"name", "directed"}) {
        const auto refused = set_relation_property(store, id, key, graph::Value{std::string("x")});
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().message_key == "error.property_relation_reserved");
        CHECK_FALSE(set_relation_property(store, id, key, graph::Value{}).has_value());
    }
    CHECK(store.links(np("a.md"))->related.front().name == "uses");

    // Yön değişince kenar yeniden kurulur: özellikler yeni kenara taşınır.
    REQUIRE(store.set_relation_direction(id, np("b.md")).has_value());
    const RelationId flipped = only_relation(store, "a.md");
    CHECK(flipped != id);
    props = relation_properties(store, flipped);
    REQUIRE(props.has_value());
    CHECK(find_prop(*props, "since")->value == graph::Value{std::int64_t{2024}});
    CHECK(find_prop(*props, "note")->value == graph::Value{std::string("weekly")});

    REQUIRE(set_relation_property(store, flipped, "note", graph::Value{}).has_value());
    CHECK(find_prop(*relation_properties(store, flipped), "note") == nullptr);
    REQUIRE(store.remove_relation(flipped).has_value());
    CHECK(relation_properties(store, flipped).error().message_key == "error.relation_not_found");
    CHECK(set_relation_property(store, flipped, "k", graph::Value{true}).error().code ==
          Error::Code::NotFound);
}

TEST_CASE("property values: typed input parses, and the edit text round-trips every type") {
    CHECK(parse_property("abc") == graph::Value{std::string("abc")});
    CHECK(parse_property("42") == graph::Value{std::int64_t{42}});
    CHECK(parse_property("-7") == graph::Value{std::int64_t{-7}});
    CHECK(parse_property("3.5") == graph::Value{3.5});
    CHECK(parse_property("1e3") == graph::Value{1000.0});
    CHECK(parse_property("true") == graph::Value{true});
    CHECK(parse_property("false") == graph::Value{false});
    CHECK(parse_property("True") == graph::Value{std::string("True")});
    CHECK(parse_property(" 42") == graph::Value{std::string(" 42")});
    CHECK(parse_property("inf") == graph::Value{std::string("inf")});
    CHECK(parse_property("") == graph::Value{std::string()});
    CHECK(parse_property("text:42") == graph::Value{std::string("42")});
    CHECK(parse_property("number:5") == graph::Value{std::int64_t{5}});
    CHECK(parse_property("bool:true") == graph::Value{true});
    CHECK(parse_property("42", PropertyType::Text) == graph::Value{std::string("42")});
    CHECK(parse_property("number:5", PropertyType::Text) == graph::Value{std::string("number:5")});
    CHECK(parse_property("7", PropertyType::Number) == graph::Value{std::int64_t{7}});
    CHECK(arg_of(parse_property("abc", PropertyType::Number).error(), "reason") == "not_number");
    CHECK(arg_of(parse_property("inf", PropertyType::Number).error(), "reason") == "not_number");
    CHECK(arg_of(parse_property("yes", PropertyType::Bool).error(), "reason") == "not_bool");
    CHECK(arg_of(parse_property("number:x").error(), "reason") == "not_number");

    const graph::Value values[] = {
        graph::Value{std::string("plain")},
        graph::Value{std::string("42")},
        graph::Value{std::string("true")},
        graph::Value{std::string("text:x")},
        graph::Value{std::string("")},
        graph::Value{std::string("1.5")},
        graph::Value{std::int64_t{0}},
        graph::Value{std::int64_t{-12}},
        graph::Value{1.0},
        graph::Value{-0.25},
        graph::Value{1e300},
        graph::Value{true},
        graph::Value{false},
    };
    for (const graph::Value& value : values) {
        INFO(property_input(value));
        CHECK(parse_property(property_input(value)) == value);
        CHECK(property_type(value).has_value());
    }
    CHECK(property_text(graph::Value{1.0}) == "1.0");
    CHECK(property_text(graph::Value{std::int64_t{5}}) == "5");
    CHECK(property_text(graph::Value{graph::Bytes{"abc"}}) == "<bytes:3>");
    CHECK(property_text(graph::Value{graph::Timestamp{0}}) == "1970-01-01 00:00:00.000");
    CHECK_FALSE(property_type(graph::Value{graph::Timestamp{0}}).has_value());
    CHECK(property_type_named("number") == PropertyType::Number);
    CHECK_FALSE(property_type_named("date").has_value());
}

// İnceleme: `prop:` önbelleği görüntü ömrünce sınırsız büyüyordu (her kısmi anahtar bir girdi) ve büyük
// değeri tümüyle yüklüyordu. Önbellek en son kullanılan limits.search.property_keys anahtarı tutar; büyük
// değer yüklenmeden boyutuyla eşlenir.
TEST_CASE("prop search source: the cache keeps the most recent keys and large values are not loaded") {
    TempDir dir;
    NoteStore store(dir.path());
    REQUIRE(seed_note(store, "a.md", "a\n"));
    const std::string big(100'000, 'x');
    REQUIRE(set_note_property(store, np("a.md"), "blob", graph::Value{big}).has_value());
    CHECK(**notes_with_property(store, "blob") == PropertyNotes{{np("a.md"), "<100000 bytes>"}});

    store.impl().property_cache_keys = 2;
    for (const char* key : {"s", "st", "sta", "st", "stat"})
        REQUIRE(notes_with_property(store, key).has_value());
    const auto& cached = store.impl().snapshot->property_notes;
    CHECK(cached.size() == 2);
    CHECK(cached.contains("stat"));
    CHECK(cached.contains("st"));  // yeniden kullanılan anahtar en yeni sayılır
    store.impl().property_cache_keys = SIZE_MAX;
    for (const char* key : {"k1", "k2", "k3", "k4"})
        REQUIRE(notes_with_property(store, key).has_value());
    CHECK(store.impl().snapshot->property_notes.size() == 6);
    const auto defaults = parse_config("");
    CHECK(defaults.config.limits.search.property_keys.cap() == kDefaultPropertyCacheKeys);
}

// İnceleme: uç zaman damgası (senkronla gelebilir). std::format fırlatmıyor ama taşıyordu (INT64_MIN
// "10850-06-21 …" yazıyordu); 0000–9999 dışı ham milisaniye yazılır.
TEST_CASE("property values: extreme timestamps format without throwing") {
    for (const std::int64_t ms :
         {std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::int64_t>::max(),
          std::int64_t{-62'167'219'200'001}, std::int64_t{253'402'300'800'000}}) {
        INFO(ms);
        std::string text;
        CHECK_NOTHROW(text = property_text(graph::Value{graph::Timestamp{ms}}));
        CHECK(text == "<time:" + std::to_string(ms) + ">");
    }
    CHECK(property_text(graph::Value{graph::Timestamp{-62'167'219'200'000}}) == "0000-01-01 00:00:00.000");
    CHECK(property_text(graph::Value{graph::Timestamp{253'402'300'799'999}}) == "9999-12-31 23:59:59.999");
}

TEST_CASE("prop search source: live notes carrying a key, with value text, refreshed after a write") {
    TempDir dir;
    NoteStore store(dir.path());
    REQUIRE(seed_note(store, "b.md", "b\n"));
    REQUIRE(seed_note(store, "a.md", "a\n"));
    REQUIRE(seed_note(store, "c.md", "c\n"));
    REQUIRE(set_note_property(store, np("b.md"), "status", graph::Value{std::string("Done")}).has_value());
    REQUIRE(set_note_property(store, np("a.md"), "status", graph::Value{std::int64_t{2}}).has_value());

    auto found = notes_with_property(store, "status");
    REQUIRE(found.has_value());
    CHECK(**found == PropertyNotes{{np("a.md"), "2"}, {np("b.md"), "Done"}});
    CHECK(*notes_with_property(store, "status") == *found);  // aynı görüntüde paylaşılır

    REQUIRE(set_note_property(store, np("c.md"), "status", graph::Value{true}).has_value());
    REQUIRE(store.trash(np("a.md")).has_value());
    CHECK(**notes_with_property(store, "status") ==
          PropertyNotes{{np("b.md"), "Done"}, {np("c.md"), "true"}});
    CHECK((*notes_with_property(store, "type"))->empty());  // uygulamanın anahtarı aranmaz
    CHECK((*notes_with_property(store, "missing"))->empty());

    TempDir empty;
    CHECK((*notes_with_property(NoteStore(empty.path()), "status"))->empty());
}

TEST_CASE("concurrent edits of the same property converge to one winner on both devices and are reported") {
    Devices d;
    REQUIRE(set_note_property(d.a, np("x.md"), "status", graph::Value{std::string("open")}).has_value());
    d.send(d.a, d.b);
    REQUIRE(set_note_property(d.a, np("x.md"), "status", graph::Value{std::string("doing")}).has_value());
    later();
    REQUIRE(set_note_property(d.b, np("x.md"), "status", graph::Value{std::string("done")}).has_value());

    const LogIngest at_b = d.send(d.a, d.b);
    const LogIngest at_a = d.send(d.b, d.a);
    const auto reports = [](const LogIngest& ingest) {
        return std::ranges::any_of(ingest.report.conflicts, [](const graph::MergeConflict& conflict) {
            return std::holds_alternative<graph::NodeId>(conflict.owner) && conflict.key == "status";
        });
    };
    CHECK(reports(at_b));
    CHECK(reports(at_a));
    const graph::Value want{std::string("done")};
    CHECK(find_prop(props_of(d.a, "x.md"), "status")->value == want);
    CHECK(find_prop(props_of(d.b, "x.md"), "status")->value == want);

    // İlişki kenarında da aynı kural.
    REQUIRE(d.a.add_relation(np("x.md"), np("y.md"), {"uses", false}).has_value());
    d.send(d.a, d.b);
    REQUIRE(set_relation_property(d.a, only_relation(d.a, "x.md"), "weight", graph::Value{std::int64_t{1}})
                .has_value());
    later();
    REQUIRE(set_relation_property(d.b, only_relation(d.b, "x.md"), "weight", graph::Value{std::int64_t{2}})
                .has_value());
    const LogIngest edge_at_b = d.send(d.a, d.b);
    d.send(d.b, d.a);
    CHECK(std::ranges::any_of(edge_at_b.report.conflicts, [](const graph::MergeConflict& conflict) {
        return std::holds_alternative<graph::EdgeId>(conflict.owner) && conflict.key == "weight";
    }));
    CHECK(find_prop(*relation_properties(d.a, only_relation(d.a, "x.md")), "weight")->value ==
          graph::Value{std::int64_t{2}});
    CHECK(find_prop(*relation_properties(d.b, only_relation(d.b, "x.md")), "weight")->value ==
          graph::Value{std::int64_t{2}});
}
