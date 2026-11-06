#include "app/inspector.hpp"

#include "app/map_relations.hpp"
#include "core/i18n.hpp"

#include <algorithm>
#include <iterator>
#include <utility>
#include <variant>

namespace noteapp::app {

namespace {

std::string type_of(const core::Property& property) {
    if (property.large)
        return "large";
    if (const auto type = core::property_type(property.value))
        return std::string(core::property_type_name(*type));
    return std::holds_alternative<graph::Timestamp>(property.value) ? "time" : "bytes";
}

std::string tag_without_hash(std::string_view tag) {
    if (tag.starts_with('#'))
        tag.remove_prefix(1);
    return core::tag_key(tag);
}

core::Error not_a_note() {
    return core::Error{core::Error::Code::InvalidPath, "inspector.labels_on_notes", {}};
}

// Satırın kimliği (tür, anahtar); seçim yeniden okumada bununla bulunur.
using RowIdentity = std::optional<std::pair<InspectorRow::Kind, std::string>>;

RowIdentity identity_of(const InspectorRow* row) {
    if (row == nullptr)
        return std::nullopt;
    return std::pair(row->kind, row->key);
}

// Aynı kimlikli satır; yoksa (anahtar kalktı) eski sıra, satırların içine kısılır.
std::size_t selection_after(const std::vector<InspectorRow>& rows, const RowIdentity& kept,
                            std::size_t previous) {
    if (rows.empty())
        return 0;
    const auto found =
        kept ? std::ranges::find_if(rows,
                                    [&](const InspectorRow& row) {
                                        return row.kind == kept->first && row.key == kept->second;
                                    })
             : rows.end();
    return found != rows.end() ? static_cast<std::size_t>(found - rows.begin())
                               : std::min(previous, rows.size() - 1);
}

std::string path_text(const std::optional<core::NotePath>& path) {
    return path ? path->string() : std::string("?");
}

}  // namespace

void Inspector::inspect(InspectTarget target) {
    if (target_ == target)
        return;
    target_ = std::move(target);
    rows_.clear();
    error_.clear();
    selected_ = 0;
}

void Inspector::clear() {
    target_.reset();
    rows_.clear();
    error_.clear();
    selected_ = 0;
}

void Inspector::reload(const core::NoteStore& store) {
    // Seçim anahtarıyla korunur: senkron önüne satır eklerse düzenleme ve silme başka anahtara gitmesin.
    const RowIdentity kept = identity_of(selected_row());
    rows_.clear();
    error_.clear();
    if (!target_)
        return;
    const bool note = target_->kind == InspectTarget::Kind::Note;
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- not konusunda yol hep dolu (of_note)
    auto properties = note ? core::note_properties(store, *target_->note)
                           : core::relation_properties(store, target_->relation);
    if (!properties) {
        error_ = core::format_error(properties.error());
        selected_ = 0;
        return;
    }
    std::vector<InspectorRow> fixed;
    for (const core::Property& property : *properties) {
        InspectorRow row{.kind = InspectorRow::Kind::Property,
                         .key = property.key,
                         .key_text = cell_text(property.key, cell_bytes_),
                         .value_text = property.large
                                           ? "<" + std::to_string(property.size) + " bytes>"
                                           : cell_text(core::property_text(property.value), cell_bytes_),
                         .type = type_of(property),
                         .input = {},
                         .value_type = property.large ? std::nullopt : core::property_type(property.value),
                         .read_only = property.read_only || property.large};
        if (!row.read_only && row.value_type)
            row.input = core::property_input(property.value);
        else
            row.read_only = true;  // zaman ve bayt denetçinin türü değildir: sorgu çubuğuyla değişir
        (row.read_only ? fixed : rows_).push_back(std::move(row));
    }
    if (note) {
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- not konusunda yol hep dolu (of_note)
        const auto links = store.links(*target_->note);
        if (!links) {
            rows_.clear();
            error_ = core::format_error(links.error());
            selected_ = 0;
            return;
        }
        for (const std::string& tag : links->tags)
            rows_.push_back(InspectorRow{.kind = InspectorRow::Kind::Label,
                                         .key = tag,
                                         .key_text = cell_text("#" + tag, cell_bytes_),
                                         .value_text = {},
                                         .type = "label"});
    }
    std::ranges::move(fixed, std::back_inserter(rows_));
    selected_ = selection_after(rows_, kept, selected_);
}

void Inspector::move(int delta) {
    if (rows_.empty())
        return;
    const auto last = static_cast<long long>(rows_.size()) - 1;
    selected_ = static_cast<std::size_t>(std::clamp(static_cast<long long>(selected_) + delta, 0LL, last));
}

void Inspector::select(std::size_t row) {
    if (row < rows_.size())
        selected_ = row;
}

std::expected<std::string, core::Error> Inspector::set(core::NoteStore& store, std::string_view key,
                                                       std::string_view input,
                                                       std::optional<core::PropertyType> type) {
    if (!target_)
        return std::unexpected(core::Error{core::Error::Code::NotFound, "inspector.nothing", {}});
    auto value = core::parse_property(input, type);
    if (!value)
        return std::unexpected(value.error());
    const auto written = target_->kind == InspectTarget::Kind::Note
                             // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- of_note: hep dolu
                             ? core::set_note_property(store, *target_->note, key, *value)
                             : core::set_relation_property(store, target_->relation, key, *value);
    if (!written)
        return std::unexpected(written.error());
    reload(store);
    return core::tr("inspector.set", {{"key", std::string(key)}, {"value", core::property_text(*value)}});
}

std::expected<std::string, core::Error> Inspector::remove(core::NoteStore& store, std::string_view key) {
    if (!target_)
        return std::unexpected(core::Error{core::Error::Code::NotFound, "inspector.nothing", {}});
    const auto removed = target_->kind == InspectTarget::Kind::Note
                             // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- of_note: hep dolu
                             ? core::set_note_property(store, *target_->note, key, graph::Value{})
                             : core::set_relation_property(store, target_->relation, key, graph::Value{});
    if (!removed)
        return std::unexpected(removed.error());
    reload(store);
    return core::tr("inspector.removed", {{"key", std::string(key)}});
}

std::expected<std::string, core::Error> Inspector::add_label(core::NoteStore& store, std::string_view tag) {
    if (!target_ || target_->kind != InspectTarget::Kind::Note)
        return std::unexpected(not_a_note());
    auto done = connect_map_nodes(store, {MapNodeKind::Note, path_text(target_->note)},
                                  {MapNodeKind::Tag, tag_without_hash(tag)}, MapEdgeKind::Tag);
    if (done)
        reload(store);
    return done;
}

std::expected<std::string, core::Error> Inspector::remove_label(core::NoteStore& store,
                                                                std::string_view tag) {
    if (!target_ || target_->kind != InspectTarget::Kind::Note)
        return std::unexpected(not_a_note());
    auto done = disconnect_map_nodes(store, {MapNodeKind::Note, path_text(target_->note)},
                                     {MapNodeKind::Tag, tag_without_hash(tag)}, MapEdgeKind::Tag);
    if (done)
        reload(store);
    return done;
}

std::string Inspector::confirm_remove_text(const InspectorRow& row) const {
    if (row.kind == InspectorRow::Kind::Label)
        return core::tr("inspector.confirm_remove_label",
                        {{"tag", row.key}, {"path", target_ ? path_text(target_->note) : std::string()}});
    return core::tr("inspector.confirm_remove", {{"key", row.key}, {"value", row.value_text}});
}

std::string Inspector::title() const {
    if (!target_)
        return {};
    if (target_->kind == InspectTarget::Kind::Note)
        return path_text(target_->note);
    std::string text =
        path_text(target_->note) + (target_->directed ? " → " : " — ") + path_text(target_->other);
    if (!target_->name.empty())
        text += " '" + target_->name + "'";
    return text;
}

}  // namespace noteapp::app
