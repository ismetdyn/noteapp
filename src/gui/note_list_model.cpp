#include "gui/note_list_model.hpp"

#include "core/i18n.hpp"
#include "search/fuzzy.hpp"
#include "search/query.hpp"

#include <algorithm>
#include <iterator>
#include <map>

namespace noteapp::gui {

bool same_listing(std::span<const core::NoteInfo> a, std::span<const core::NoteInfo> b) {
    return std::ranges::equal(a, b, [](const core::NoteInfo& x, const core::NoteInfo& y) {
        return x.path == y.path && x.revision == y.revision && x.mtime == y.mtime && x.size == y.size;
    });
}

bool NoteListModel::set_notes(std::vector<core::NoteInfo> notes) {
    if (same_listing(notes_, notes))
        return false;
    notes_ = std::move(notes);
    rebuild_scope();
    refilter();
    return true;
}

void NoteListModel::set_query(std::string query) {
    if (query == query_)
        return;
    query_ = std::move(query);
    refilter();
}

void NoteListModel::set_folder(std::string folder) {
    if (folder == folder_)
        return;
    folder_ = std::move(folder);
    rebuild_scope();
    refilter();
}

void NoteListModel::rebuild_scope() {
    scope_.clear();
    if (folder_.empty()) {
        scope_ = notes_;
        return;
    }
    // "a" klasörü "a/x.md"yi alır, "ab/x.md"yi almaz: ayırıcıyla birlikte önek.
    const std::string prefix = folder_ + '/';
    std::ranges::copy_if(notes_, std::back_inserter(scope_),
                         [&prefix](const core::NoteInfo& n) { return n.path.string().starts_with(prefix); });
}

// TUI kuralı: bozuk değer metin sayılmaz, liste son geçerli sorguyla kalır. Satırlar yine de her seferinde o
// sorguyla bu kapsamdan kurulur: kapsam değiştiyse (senkron, klasör) eski satırın konumu taşardı.
void NoteListModel::refilter() {
    if (auto parsed = search::parse_query(query_, views_)) {
        query_error_.reset();
        valid_query_ = std::move(*parsed);
    } else {
        query_error_ = core::format_error(parsed.error().error);
    }
    const search::ParsedQuery* parsed = &valid_query_;
    ++resets_;
    rows_.clear();
    // Argümansız sorguda tuş başına NoteMeta kopyası yok.
    std::vector<core::NoteMeta> metas;
    std::vector<std::size_t> allowed;
    if (parsed->filters.empty() && !parsed->sort) {
        allowed.resize(scope_.size());
        for (std::size_t i = 0; i < allowed.size(); ++i)
            allowed[i] = i;
    } else {
        metas.reserve(scope_.size());
        for (const core::NoteInfo& note : scope_)
            metas.push_back(core::meta_of(note));
        allowed = search::apply(parsed->filters, std::nullopt, metas);
    }
    if (content_) {
        // Sıralamayı ContentIndex verdi; yalnız kapsamdaki, argümandan geçen (ve hâlâ listede olan) notlar
        // kalır.
        std::map<core::NotePath, std::size_t> in_scope;
        for (const std::size_t i : allowed)
            in_scope.emplace(scope_[i].path, i);
        for (const auto& result : *content_)
            if (const auto found = in_scope.find(result.path); found != in_scope.end())
                rows_.push_back({found->second, {}, result.hit_count});
    } else {
        std::vector<core::NoteInfo> subset;
        subset.reserve(allowed.size());
        for (const std::size_t i : allowed)
            subset.push_back(scope_[i]);
        for (auto& ranked : search::rank_notes(parsed->text, subset))
            rows_.push_back({allowed[ranked.index], std::move(ranked.match.positions), 0});
    }
    if (parsed->sort)
        sort_rows(*parsed->sort, metas);
}

void NoteListModel::sort_rows(const search::SortSpec& sort, std::span<const core::NoteMeta> metas) {
    std::vector<std::size_t> order;
    order.reserve(rows_.size());
    std::map<std::size_t, Row> by_scope;
    for (Row& row : rows_) {
        order.push_back(row.scope_index);
        by_scope.emplace(row.scope_index, std::move(row));
    }
    search::sort_notes(order, sort, metas);
    rows_.clear();
    for (const std::size_t index : order)
        rows_.push_back(std::move(by_scope.at(index)));
}

void NoteListModel::set_content_results(std::optional<std::vector<search::ContentResult>> results) {
    if (!content_ && !results)  // ad modunda tuş başına gereksiz ikinci fuzzy sıralama olmasın
        return;
    content_ = std::move(results);
    refilter();
}

std::string NoteListModel::display(int row) const {
    std::string path = note_at(row).path.string();
    if (content_)
        path += "  (" + std::to_string(rows_[static_cast<std::size_t>(row)].hits) + ")";
    return path;
}

int NoteListModel::index_of(const core::NotePath& path) const {
    for (int row = 0; row < row_count(); ++row)
        if (note_at(row).path == path)
            return row;
    return -1;
}

}  // namespace noteapp::gui
