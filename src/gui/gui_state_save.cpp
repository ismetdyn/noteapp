#include "app/document.hpp"
#include "app/export.hpp"
#include "core/i18n.hpp"
#include "core/text.hpp"
#include "detect/detect.hpp"
#include "gui/gui_state.hpp"
#include "gui/gui_state_internal.hpp"

#include <cmath>
#include <filesystem>
#include <utility>

namespace noteapp::gui {

namespace {

constexpr int kPercent = 100;

std::filesystem::path path_from_utf8(const std::string& text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

}  // namespace

void GuiState::new_note(const std::string& first_line) {
    std::string folder = target_folder();  // kayıt seçimi değiştirmeden önce
    // Korunan sekme kalır (taslak kaydedilmeden, not kaydedilerek); taslak yeni sekmede açılır.
    if (keeps_active_tab() ? !push_tab() : !flush())
        return;
    claim_active_tab();
    close_trash();
    selected_folder_.reset();
    draft_folder_ = std::move(folder);
    new_note_ = true;
    review_ = false;  // taslak düzenlemede açılır; ilk kayıttan sonra da düzenlemede kalır
    shown_.reset();
    reload_links();
    current_row_ = -1;
    doc_.reset_draft();
    const std::string initial = first_line.empty() ? std::string{} : first_line + '\n';
    if (!first_line.empty())
        clear_query();  // new_note_ açıkken süzgeç yeniden seçim yapmaz
    set_editor(initial, true);
    editor_.move_to_end();
    request_focus(FocusRequest::Editor);
}

bool GuiState::flush() {
    if (new_note_) {
        if (!draft_needs_answer(editor_.text())) {  // boş taslakta kaybedilecek bir şey yok
            new_note_ = false;
            return true;
        }
        return save_new();
    }
    return dirty_ && shown_ ? save_existing() : true;
}

void GuiState::save_now() {
    if (new_note_)
        save_new();
    else if (dirty_ && shown_)
        save_existing();
}

std::expected<app::Document::Saved, core::Error> GuiState::try_write() {
    autosave_.cancel();
    const auto saved = doc_.save(store_, editor_.text());
    if (saved == app::Document::Saved::Written)
        wrote();
    return saved;
}

// Gösterilen not diske yazıldı (kayıt ya da bilerek ezme).
void GuiState::wrote() {
    ++writes_;
    changed();
    dirty_ = false;
    editor_.mark_saved();
    status_.clear();
    // ponytail: her otomatik kayıtta tam yeniden tarama (>= 1 sn boşta sonrası, 5.000 notta ~20 ms) kabul
    // edilebilir; yükseltme yolu: write()'ın döndürdüğü mtime ile yalnız ilgili model satırını güncellemek.
    rescan();  // listedeki mtime tazelenir: yoksa aynı not "dışarıdan değişmiş" sanılıp yeniden yüklenirdi
}

// Otomatik kayıt soru açmaz, not yaratmaz (TUI ile aynı): çakışma ya da silinen not durum çubuğunda kalır,
// karar açık kayıtta (Ctrl+S, not geçişi, kapanış).
void GuiState::autosave() {
    autosave_.cancel();
    const auto written = doc_.autosave(store_, editor_.text());
    if (!written)
        report(written.error());
    else if (*written)
        wrote();
    else
        status_ = doc_.disk_notice();
}

bool GuiState::save_existing() {
    const auto saved = try_write();
    if (!saved) {
        report(saved.error());
        return false;
    }
    switch (*saved) {
    case app::Document::Saved::Written:
        return true;
    case app::Document::Saved::Conflict:
        return resolve_conflict();
    case app::Document::Saved::Gone:
        break;
    }
    // Not dışarıdan silinmiş: tamponu yeni not olarak sakla, farklı kaydet diyaloğu eski yer ve adı önerir
    // (TUI ile aynı). Belge eski yolu, BOM'u ve satır sonunu korur: yeni not aynı baytlarla yazılır.
    const core::NoteInfo gone = shown_note();  // kopya: aşağıda sıfırlanır
    new_note_ = true;
    draft_folder_ = core::split_note_path(gone.path.string()).folder;
    shown_.reset();
    if (save_new())
        return true;
    // Vazgeçildi ya da yazılamadı: not taslağa dönmez, yine kendisidir (TUI ile aynı). Diske dönerse açık kayıt
    // çakışmayı sorar ve otomatik kayıt sürer; dönmezse sonraki kayıt yine sorar. Belge yeni nota geçtiyse
    // (yazıldı, okunamadı) dokunulmaz.
    if (new_note_ && doc_.path() == gone.path) {
        new_note_ = false;
        shown_ = gone;
    }
    return false;
}

bool GuiState::resolve_conflict() {
    switch (deciders.conflict(shown_note().path.string())) {
    case ConflictChoice::Reload: {
        auto text = doc_.reload(store_);
        if (!text) {
            report(text.error());
            return false;
        }
        set_document_text(std::move(*text));
        return true;
    }
    case ConflictChoice::Overwrite:
        // Diskteki güncel revizyon alınır; kullanıcı bilerek ezdi.
        if (const auto written = doc_.overwrite(store_, editor_.text()); !written) {
            report(written.error());
            return false;
        }
        wrote();
        return true;
    case ConflictChoice::SaveCopy:
        return save_copy();
    case ConflictChoice::Cancel:
        break;
    }
    return false;
}

bool GuiState::save_copy() {
    const auto created = doc_.create_beside(store_, editor_.text(), app::kCopySuffix);
    if (!created) {
        report(created.error());
        return false;
    }
    changed();
    const bool adopted = adopt_saved(*created);
    if (adopted) {
        status_ = core::tr("gui.saved_copy", {{"path", created->string()}});
        notify(app::NoticeKind::Success, status_);
    }
    return adopted;
}

bool GuiState::save_new() {
    autosave_.cancel();
    const std::string& content = editor_.text();
    const detect::Options options{config_.default_type, config_.tuning.min_confidence,
                                  config_.tuning.max_scan_bytes};
    const detect::Suggestion suggestion = detect::suggest(content, options);

    SaveDialogRequest request;
    request.initial = {app::suggested_stem(content, suggestion.type, std::chrono::system_clock::now(),
                                           config_.tuning.slug_max_length),
                       draft_folder_, suggestion.type};
    if (const auto& gone = doc_.path())  // diskte silinmiş not (taslağın belgesinin yolu yoktur)
        request.initial = move_request(*gone).initial;
    request.folders = folders_;
    request.types = config_.types;
    request.hint =
        suggestion.detected
            ? core::tr("gui.suggestion",
                       {{"type", suggestion.type},
                        {"percent", std::to_string(std::lround(suggestion.confidence * kPercent))}})
            : core::tr("gui.suggestion_default", {{"type", suggestion.type}});
    const auto chosen = deciders.save_dialog(request);
    if (!chosen)
        return false;  // vazgeç: tampon ve yeni-not durumu aynen kalır

    // Baytlar editördeki gibi (BOM ve CRLF dahil): create_exact, LF'ye çevirmez.
    const auto created =
        doc_.create(store_, core::NewNote{chosen->folder, chosen->name, chosen->type, {}}, editor_.text());
    if (!created) {
        report(created.error());
        return false;
    }
    changed();
    return adopt_saved(*created);
}

// Diske yazılmış notu "mevcut not" yapar: mtime alınır, liste tazelenir, not seçili kalır.
bool GuiState::adopt_saved(const core::NotePath& path) {
    const auto content = store_.read(path);
    if (!content) {
        report(content.error());  // not diskte, ama tampon kirli kalır: kullanıcı yeniden deneyebilir
        return false;
    }
    shown_ = core::NoteInfo{path, content->mtime, content->text.size(), content->revision};
    new_note_ = false;
    dirty_ = false;
    editor_.mark_saved();
    autosave_.cancel();
    status_.clear();
    // Liste tazelenirken seçim süzgeçteki başka nota kayıp editörü ezmesin: kaydedilen not gösterilir.
    const struct Restore {
        bool& flag;
        bool value;
        ~Restore() { flag = value; }
    } restore{in_show_, std::exchange(in_show_, true)};
    rescan();
    ensure_visible(path);
    return true;
}

bool GuiState::confirm_close() {
    if (new_note_) {
        if (!draft_needs_answer(editor_.text()))
            return true;
        switch (deciders.close_unsaved()) {
        case CloseChoice::Save:
            return save_new();
        case CloseChoice::Discard:
            return true;
        case CloseChoice::Cancel:
            return false;
        }
    }
    return dirty_ && shown_ ? save_existing() : true;
}

// Kapanmazsa Ayarlar sekmesi öne gelir: kullanıcı değişikliği ya da alanın altındaki hatayı görür.
bool GuiState::confirm_close_settings() {
    if (!settings_.has_changes())
        return true;
    switch (deciders.close_unsaved_settings()) {
    case CloseChoice::Save:
        settings_.save();
        if (!settings_.has_changes())
            return true;
        break;
    case CloseChoice::Discard:
        settings_.revert();
        settings_.token.clear();  // yazılmış ama kasaya gitmemiş token da atılır
        return true;
    case CloseChoice::Cancel:
        break;
    }
    show_settings_tab();
    return false;
}

bool GuiState::request_close() {
    // Dış editör açıkken kapanmaz: editör kapanınca yapılacak geri yazma (edit_in_temp_file) kaybolurdu.
    if (external_running_) {
        status_ = core::tr("gui.external_running");
        return false;
    }
    // Ayarlar önce: kayıtta notes_dir değişirse açık notu kendisi kaydeder (host flush), not sorusu boşa
    // çıkar.
    if (!confirm_close_settings() || !confirm_close() ||
        !confirm_close_drafts())  // vazgeçildi ya da kayıt başarısız: pencere ve tampon yerinde kalır
        return false;
    save_gui_state();
    // Kayıt akışından sonra: bekleyen değişiklik için son senkron, en çok exit_timeout_seconds.
    stop_sync(std::chrono::seconds(config_.tuning.exit_timeout_seconds));
    return true;
}

SaveDialogRequest GuiState::move_request(const core::NotePath& note) const {
    const core::NoteParts parts = core::split_note_path(note.string());
    SaveDialogRequest request;
    request.mode = SaveDialogRequest::Mode::Move;
    // `a.tar.gz`: gövde de uzantı gibi görünür; adın tamamı gösterilir, form onu aynen geri verir.
    const bool dotted_stem = !core::split_typed_name(parts.name).extension.empty();
    request.initial = {dotted_stem ? parts.name + "." + parts.type : parts.name, parts.folder, parts.type};
    request.folders = folders_;
    request.types = config_.types;
    return request;
}

void GuiState::save_as() {
    if (new_note_) {
        save_new();
        return;
    }
    if (!shown_)
        return;
    const core::NotePath note = shown_->path;
    if (!flush_for(note))
        return;
    if (const auto chosen = deciders.save_dialog(move_request(note)))
        move_saved(note, *chosen);
}

void GuiState::change_type(const std::string& type) {
    if (!shown_ || new_note_ || type.empty())
        return;
    const core::NotePath note = shown_->path;
    const core::NoteParts parts = core::split_note_path(note.string());
    if (type == parts.type)
        return;
    if (flush_for(note))
        move_saved(note, {parts.name, parts.folder, type});
}

// flush listeyi tazeler: not aramadan düşerse editör başka nota geçer ya da boşalır, seçim artık hedef
// değildir. İşlem flush'tan önce alınan nota uygulanır; o not yoksa (dışarıdan silinmiş) iletiyle durur.
bool GuiState::flush_for(const core::NotePath& note) {
    if (!flush())
        return false;
    const auto content = store_.read(note);
    if (!content)
        fail(content.error());
    return content.has_value();
}

// Bekleyen düzenleme önce kendi notuna yazılır (flush_for): kayıt listeyi tazeler, seçim kayıttan sonra
// değişirse düzenleme yanlış nota giderdi. false = kayıt olmadı ya da vazgeçildi.
bool GuiState::flush_shown() {
    if (!unsaved())
        return true;
    // Kopya: kayıt akışı shown_'ı sıfırlayabilir.
    const auto note = shown_ && !new_note_ ? std::optional(shown_->path) : std::nullopt;
    return note ? flush_for(*note) : flush();
}

// Tampon önceden kaydedilmiş olmalı (flush). Hedef varsa NoteStore::move AlreadyExists verir, hiçbir şey
// değişmez.
bool GuiState::move_saved(const core::NotePath& note, const SaveDetails& target) {
    const auto moved = app::move_note(store_, note, target.folder, target.name, target.type);
    if (!moved) {
        report(moved.error());
        return false;
    }
    if (*moved == note)
        return true;
    changed();
    if (shown_ && shown_->path == note) {
        shown_->path = *moved;
        if (const auto fresh = store_.read(*moved))  // taşıma mtime'ı korur; yine de güncel değeri al
            doc_.moved(*moved, fresh->revision);
    }
    status_.clear();
    rescan();
    ensure_visible(*moved);
    return true;
}

void GuiState::move_note() {
    if (new_note_ || !shown_)
        return;
    const core::NotePath note = shown_->path;
    if (!flush_for(note))
        return;
    const core::NoteParts parts = core::split_note_path(note.string());
    SaveDialogRequest request = move_request(note);
    request.folder_only = true;
    // Ad ve tip kilitli: karar ne dönerse dönsün yalnız klasör alınır.
    if (const auto chosen = deciders.save_dialog(request))
        move_saved(note, {parts.name, chosen->folder, parts.type});
}

void GuiState::export_pdf() {
    if (shown_ && binary())
        return;
    // Önce kaydet: PDF diskteki sürümden üretilir ve tampondaki düzenleme kaybolmaz. Kayıt başarısızsa ya da
    // vazgeçildiyse (tampon korunur) dışa aktarma da yapılmaz.
    std::optional<core::NotePath> note = shown_ ? std::optional(shown_->path) : std::nullopt;
    if (note ? !flush_for(*note) : (unsaved() && !flush()))
        return;
    if (!note && shown_)  // taslak: yolu ancak kayıttan sonra belli olur
        note = shown_->path;
    if (!note)  // gösterilen not yok ya da boş taslak atıldı
        return;
    const auto output = deciders.export_path(app::default_export_name(*note));
    if (!output)
        return;
    const auto content = store_.read(*note);
    if (!content) {
        fail(content.error());
        return;
    }
    // İkili denetimi diskteki baytlarda (CLI ve TUI ile aynı): salt-okunur nedeni "çok büyük" olan ikili not
    // binary() ile yakalanmaz.
    const auto exported = app::export_note_pdf(content->text, *note, path_from_utf8(*output));
    if (!exported) {
        fail(exported.error());
        return;
    }
    status_ = core::tr("app.exported", {{"path", *output}});
    // Göreli yolun klasörü boş olurdu ("Klasörü aç" file:/// kökünü açardı): mutlak yoldan alınır.
    std::error_code absolute_error;
    const std::filesystem::path written = std::filesystem::absolute(std::filesystem::path(*output), absolute_error);
    notify(app::NoticeKind::Success, status_,
           absolute_error ? std::nullopt : std::optional(written.parent_path().string()));
}

bool GuiState::can_export() const {
    // Boş taslakta export_pdf taslağı atar ve sessizce çıkar; ikili notu reddeder.
    if (new_note_)
        return draft_needs_answer(editor_.text());
    return shown_.has_value() && !binary();
}

}  // namespace noteapp::gui
