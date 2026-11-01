#include "gui/note_list_model.hpp"

#include "search/fuzzy.hpp"

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

void NoteListModel::refilter() {
    ++resets_;
    rows_.clear();
    if (content_) {
        // Sıralamayı ContentIndex verdi; yalnız klasör kapsamındaki (ve hâlâ listede olan) notlar kalır.
        std::map<core::NotePath, std::size_t> in_scope;
        for (std::size_t i = 0; i < scope_.size(); ++i)
            in_scope.emplace(scope_[i].path, i);
        for (const auto& result : *content_)
            if (const auto found = in_scope.find(result.path); found != in_scope.end())
                rows_.push_back({found->second, {}, result.hit_count});
        return;
    }
    for (auto& ranked : search::rank_notes(query_, scope_))
        rows_.push_back({ranked.index, std::move(ranked.match.positions), 0});
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
