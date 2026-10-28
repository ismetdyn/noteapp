#pragma once

#include "core/note_path.hpp"
#include "core/note_store.hpp"

#include <QAbstractListModel>
#include <QList>
#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace noteapp::gui {

// İki listeleme aynı notları (yol, mtime, boyut, ikili) aynı sırada mı içeriyor? Yeniden tarama "değişti mi"
// kararı için (TUI'deki same_listing ile aynı anlam; TUI kütüphanesine bağlanmamak için burada ayrı).
bool same_listing(std::span<const core::NoteInfo> a, std::span<const core::NoteInfo> b);

// Süzülmüş not listesi: klasör kapsamı + ad modunda fuzzy sıralama (boş sorguda mtime azalan).
class NoteListModel : public QAbstractListModel {
public:
    enum Role {
        PathRole = Qt::UserRole + 1,  // QString: göreli yol
        HighlightRole,                // QList<int>: DisplayRole metnindeki vurgulanacak UTF-16 birimleri
    };

    explicit NoteListModel(QObject* parent = nullptr) : QAbstractListModel(parent) {}

    // Listeleme değiştiyse modeli sıfırlar ve true döner; aynıysa dokunmaz.
    bool set_notes(std::vector<core::NoteInfo> notes);
    void set_query(std::string query);
    // Göreli klasör yolu ('/' ayırıcılı); "" = tümü. Yalnız alt ağaçtaki notlar kalır.
    void set_folder(std::string folder);

    const core::NoteInfo& note_at(int row) const {
        return scope_[rows_[static_cast<std::size_t>(row)].scope_index];
    }
    // Yol listede yoksa geçersiz indeks.
    QModelIndex index_of(const core::NotePath& path) const;
    std::size_t total() const { return notes_.size(); }

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;

private:
    struct Row {
        std::size_t scope_index;
        std::vector<std::size_t> byte_positions;  // yol UTF-8'inde bayt ofseti
    };

    void rebuild_scope();
    void refilter();

    std::vector<core::NoteInfo> notes_;
    std::vector<core::NoteInfo> scope_;  // klasör süzgecinden geçenler; tuş başına kopyalamamak için önbellek
    std::vector<Row> rows_;
    std::string query_;
    std::string folder_;
};

}  // namespace noteapp::gui
