#pragma once

#include "app/tabs.hpp"
#include "core/limits.hpp"
#include "core/note_path.hpp"
#include "core/note_store.hpp"
#include "edit/editor_session.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace noteapp::tui {

// Sekme satırındaki bir sekme (SPEC-tui "Sekmeler").
struct TabLabel {
    std::string title;
    bool active = false;
    bool dirty = false;
    bool kept = false;  // korunmayan sekme, sonra açılan notla değişir: eğik yazılır (GUI gibi)
};

// TUI'nin çok belgeli editörü: her sekme kendi EditorSession'ını (tampon, imleç, kaydırma, geri alma) taşır.
// Kurallar GUI sekmeleriyle aynıdır (SPEC-gui "Sekmeler"); sıra, sınır, kalıcılık ve taşıma app/tabs'tadır.
// Oturumlar kendi geri çağrılarıyla kurulur (Factory): main hangi sekmeden geldiğini kimlikle bilir. Oturum
// kendi handle() çağrısının içinden kapanabilir: sekme orada silinmez, sweep() sonra toplar.
class EditorTabs {
public:
    using Factory = std::function<std::unique_ptr<EditorSession>(std::uint64_t id)>;

    EditorTabs(Factory make, core::Limit limit);

    // Etkin sekmenin oturumu; sekme yoksa kapalı yedek oturum (hiçbir şey açmaz).
    EditorSession& active();
    const EditorSession& active() const;
    std::uint64_t active_id() const { return tabs_.active_id(); }  // 0 = sekme yok
    bool empty() const { return tabs_.empty(); }
    std::size_t size() const { return tabs_.size(); }
    std::vector<TabLabel> labels() const;

    enum class Result { Done, Blocked, Limit };
    // Ayrılan sekmenin bekleyen düzenlemesi kendi notuna yazılır (GUI flush_for). Taslak kaydedilmeden
    // sekmesinde kalır. Kayıt olmadıysa (çakışma sorusu, hata) ya da sekmede soru açıksa false: geçiş yok.
    bool leave_active();
    // Not açılır: başka sekmede açıksa o öne gelir (imleci yerinde). Değilse etkin sekme korunmuyorsa (iğneli
    // değil, düzenlenmemiş, yazılmış taslak değil) onun yerine, korunuyorsa ya da `new_tab` ise yeni sekmede.
    // Yeni sekmede açılan sekme korunur; not zaten etkin sekmedeyse `new_tab` onu korur.
    Result open_note(const core::NotePath& path, std::size_t start_line, bool new_tab);
    // Taslak (`n`, eşleşmesiz Enter, Ctrl+T): `start` etkin oturumda taslağı başlatır. Yer seçimi open_note
    // gibidir; `new_tab` her zaman yeni sekme açar.
    Result begin_note(const std::function<void(EditorSession&)>& start, bool new_tab);
    // Sonraki (+1) ya da önceki (-1) sekme; ayrılan sekme önce kendi notuna yazılır. Sekme yoksa ya da geçiş
    // olmadıysa false.
    bool cycle(int delta);
    // Ctrl+W: etkin sekme mevcut akışla kapanır (temizse hemen, kirliyse Kaydet/At/Vazgeç). Kapanınca komşu
    // sekme öne gelir: sweep() `reopen` der.
    void close_active();

    struct Swept {
        bool active_closed = false;  // etkin sekme kapandı; komşusu (varsa) artık etkin
        bool reopen = false;         // Ctrl+W ile kapandı ve sekme kaldı: komşu editörde açılmalı
    };
    // Kapanmış oturumların sekmelerini toplar. Etkin sekme gittiyse sağdaki, yoksa soldaki öne gelir.
    Swept sweep();
    // Etkin sekmeyi gösterilebilir yapar: geri yüklenmiş sekmenin notu ilk etkinleşmede okunur.
    void load_active();

    struct Closed {
        std::uint64_t id = 0;
        core::NotePath path;
    };
    // Yeniden tarama: her sekme oturumu kendi notunu denetler (temiz tampon yenilenir; silinmiş not
    // kapanır, kirli tampon mevcut akışta kalır). Kapanan sekmelerin notu döner (ileti için).
    std::vector<Closed> on_listing(std::span<const core::NoteInfo> notes);
    // Liste eylemi notu ya da klasörü taşıdı: o yoldaki sekmeler yeni yolu izler.
    void follow_moved(const std::string& from, const std::string& to);
    // Etkin sekme düzenlendiyse korunur: sonra açılan not onun yerine geçmez.
    void mark_edited();

    // Durum dosyası için açık sekmeler; taslağın yolu yoktur, yazılmaz.
    app::SavedTabs saved() const;
    // Açılış: sekmeler geri gelir, içerik ilk etkinleşmede okunur. Sekme varsa çağrılmaz.
    void restore(const app::SavedTabs& saved, const std::function<bool(const core::NotePath&)>& exists);
    // Çıkış: her sekmenin kirli metni normal yoldan yazılır (EditorSession::flush_on_exit); iletiler.
    std::vector<std::string> flush_on_exit();

    const core::Limit& limit() const { return limit_; }

private:
    struct Tab {
        std::uint64_t id = 0;
        bool pinned = false;
        bool kept = false;
        std::unique_ptr<EditorSession> session;
        std::optional<core::NotePath> pending;  // geri yüklendi, henüz okunmadı
    };

    Tab* find_path(const core::NotePath& path);
    std::optional<core::NotePath> path_of(const Tab& tab) const;
    bool keeps(const Tab& tab) const;
    // Hedef sekme: yeni sekme açılır ya da etkin sekme yeniden kullanılır; sınırda nullptr.
    Tab* target(bool new_tab);
    Tab* push();  // sınırda nullptr
    void activate(Tab& tab);

    Factory make_;
    core::Limit limit_;
    app::TabList<Tab> tabs_;               // sıra, kimlik, etkin sekme, komşu (GUI ile ortak)
    std::uint64_t closing_ = 0;            // Ctrl+W ile kapanması beklenen sekme
    std::unique_ptr<EditorSession> idle_;  // sekme yokken: kapalı oturum
};

}  // namespace noteapp::tui
