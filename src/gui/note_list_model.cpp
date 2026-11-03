#include "gui/note_list_model.hpp"

#include "core/i18n.hpp"

#include <algorithm>

namespace noteapp::gui {

bool NoteListModel::set_notes(std::vector<core::NoteInfo> notes) {
    if (!listing_.set_notes(std::move(notes)))
        return false;
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
    refilter();
}

void NoteListModel::set_mode(app::SearchMode mode) {
    if (mode == mode_)
        return;
    mode_ = mode;
    refilter();
}

// Bozuk sorguda Listing son geçerli sorgunun satırlarını bu kapsamdan yeniden kurar: kapsam değiştiyse eski
// satırın konumu taşmaz.
void NoteListModel::refilter() {
    app::SearchResult result = listing_.search(query_, mode_, folder_);
    rows_ = std::move(result.rows);
    query_error_ = result.error ? std::optional(core::format_error(result.error->error)) : std::nullopt;
    ++resets_;
}

const core::NoteInfo& NoteListModel::note_at(int row) const {
    return *listing_.note_of(this->row(row).path);
}

std::string NoteListModel::display(int row) const {
    const app::ListRow& shown = this->row(row);
    std::string path = shown.path.string();
    if (mode_ != app::SearchMode::Name && shown.hit_count > 0)
        path += "  (" + std::to_string(shown.hit_count) + ")";
    return path;
}

int NoteListModel::index_of(const core::NotePath& path) const {
    const auto found = std::ranges::find(rows_, path, &app::ListRow::path);
    return found == rows_.end() ? -1 : static_cast<int>(found - rows_.begin());
}

}  // namespace noteapp::gui
