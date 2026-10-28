#include "gui/note_list_model.hpp"

#include "search/fuzzy.hpp"

#include <QString>
#include <QVariant>
#include <algorithm>
#include <iterator>

namespace noteapp::gui {

bool same_listing(std::span<const core::NoteInfo> a, std::span<const core::NoteInfo> b) {
    return std::ranges::equal(a, b, [](const core::NoteInfo& x, const core::NoteInfo& y) {
        return x.path == y.path && x.mtime == y.mtime && x.size == y.size && x.binary == y.binary;
    });
}

bool NoteListModel::set_notes(std::vector<core::NoteInfo> notes) {
    if (same_listing(notes_, notes))
        return false;
    beginResetModel();
    notes_ = std::move(notes);
    rebuild_scope();
    refilter();
    endResetModel();
    return true;
}

void NoteListModel::set_query(std::string query) {
    if (query == query_)
        return;
    beginResetModel();
    query_ = std::move(query);
    refilter();
    endResetModel();
}

void NoteListModel::set_folder(std::string folder) {
    if (folder == folder_)
        return;
    beginResetModel();
    folder_ = std::move(folder);
    rebuild_scope();
    refilter();
    endResetModel();
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
    rows_.clear();
    for (auto& ranked : search::rank_notes(query_, scope_))
        rows_.push_back({ranked.index, std::move(ranked.match.positions)});
}

QModelIndex NoteListModel::index_of(const core::NotePath& path) const {
    for (int row = 0; row < rowCount(); ++row)
        if (note_at(row).path == path)
            return index(row);
    return {};
}

int NoteListModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

QVariant NoteListModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= rowCount())
        return {};
    const Row& row = rows_[static_cast<std::size_t>(index.row())];
    const std::string path = scope_[row.scope_index].path.string();
    switch (role) {
    case Qt::DisplayRole:
    case PathRole:
        return QString::fromUtf8(path.data(), static_cast<qsizetype>(path.size()));
    case HighlightRole: {
        // Bayt ofseti -> UTF-16 birimi: öneki çözüp uzunluğuna bakmak vekil çiftleri de doğru sayar.
        QList<int> units;
        for (const std::size_t offset : row.byte_positions) {
            const auto start =
                static_cast<int>(QString::fromUtf8(path.data(), static_cast<qsizetype>(offset)).size());
            units.append(start);
            // BMP dışı karakter (4 baytlık UTF-8) iki UTF-16 birimidir; ikisi de kalın çizilir.
            if (static_cast<unsigned char>(path[offset]) >= 0xF0)
                units.append(start + 1);
        }
        return QVariant::fromValue(units);
    }
    default:
        return {};
    }
}

}  // namespace noteapp::gui
