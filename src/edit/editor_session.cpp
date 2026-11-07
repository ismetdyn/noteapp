#include "edit/editor_session.hpp"

#include "app/document.hpp"
#include "core/editor.hpp"
#include "core/folder_tree.hpp"
#include "core/i18n.hpp"
#include "core/text.hpp"
#include "edit/display_text.hpp"

#include <algorithm>
#include <memory>
#include <string_view>
#include <utility>

namespace noteapp::tui {

namespace {

std::string to_utf8(const std::filesystem::path& p) {
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

template <typename Choice> Choice cycled(Choice current, int count, int delta) {
    return static_cast<Choice>((static_cast<int>(current) + delta + count) % count);
}

// Soru pencerelerinde gezinme yönü; gezinme tuşu değilse 0.
int choice_delta(Key key) {
    switch (key) {
    case Key::Left:
    case Key::Up:
    case Key::ShiftTab:
        return -1;
    case Key::Right:
    case Key::Down:
    case Key::Tab:
        return 1;
    default:
        return 0;
    }
}

constexpr int kLeaveChoices = 3;
constexpr int kConflictChoices = 4;

}  // namespace

void EditorSession::begin_new_note(std::string default_folder) {
    name_folder_ = default_folder;
    name_line_.clear();
    DraftName name;
    name.folder = std::move(default_folder);
    name.ask_name = true;
    start_draft(std::move(name));
    host_.on_opened();
}

void EditorSession::begin_draft(std::string_view first_text, std::string folder) {
    start_draft(DraftName{std::move(folder), {}, {}, false});
    open_buffer().paste(first_text);
    refresh_type();
    host_.on_opened();
}

void EditorSession::start_draft(DraftName name) {
    draft_ = std::move(name);
    doc_.reset_draft();
    buffer_.emplace(std::string{}, host_.cell_width, host_.undo_limit_bytes);
    ++buffer_generation_;
    modes_.clear_find_matches();
    mode_ = Mode::Editing;
    show_info({});
    refresh_type();
}

void EditorSession::refresh_type() {
    draft_type_.reset();
    std::string type = note_type();
    if (type == shown_type_)
        return;
    language_ = syntax_language_for(type);
    shown_type_ = std::move(type);
    syntax_cache_.clear();
}

bool EditorSession::park() {
    if (!is_open())
        return true;
    if (mode_ != Mode::Editing)
        return false;  // soru ya da girdi açık: önce o bitsin
    if (!dirty() || is_draft())
        return true;
    handle(Key::CtrlS);  // çakışmada soru açılır, hata iletide kalır
    return is_open() && !dirty() && mode_ == Mode::Editing;
}

void EditorSession::note_draft_edit(std::size_t size_before) {
    if (!draft_ || !draft_->type.empty())
        return;
    if (draft_type_.edited(size_before, open_buffer().text().size()))
        refresh_type();
}

std::vector<SyntaxSpan> EditorSession::syntax_spans(std::size_t line) const {
    if (language_ == SyntaxLanguage::Plain || !buffer_ || buffer_->text().size() > host_.syntax.note_bytes)
        return {};
    syntax_cache_.invalidate_from(buffer_->take_changed_line());
    const SyntaxState entry = syntax_cache_.entry_state(
        language_, line, [this](std::size_t index) { return buffer_->line_text(index); },
        host_.syntax.line_bytes);
    std::vector<SyntaxSpan> spans;
    highlight_line(language_, buffer_->line_text(line), entry, &spans, host_.syntax.line_bytes);
    return spans;
}

void EditorSession::handle_name(Key key, std::string_view text) {
    const InputLine::Result result = name_line_.handle(key, text);
    if (clears_input_error(key, result))
        message_.clear();
    if (result != InputLine::Result::Ignored)
        return;
    if (key == Key::Enter) {
        submit_name();
    } else if (key == Key::Escape) {
        mode_ = Mode::Editing;  // hiçbir şey yazılmaz; `Kaydet` ile gelindiyse çıkış da iptal
        // Silinmiş notun taslağı (ask_new_place) yalnız istem sürer: not yine kendisidir. Diske dönerse açık
        // kayıt ve otomatik kayıt onun kuralıyla sürer (GUI ile aynı); dönmezse sonraki kayıt yine sorar, çıkış
        // onu yeniden yaratır (flush_write).
        if (doc_.path()) {
            draft_.reset();
            refresh_type();
        }
        cancel_pending_leave();
        show_info({});
    }
}

void EditorSession::submit_name() {
    auto name = parse_draft_name(name_line_.text(), name_folder_, std::chrono::system_clock::now(),
                                 host_.slug_max_length);
    if (!name) {
        message_ = core::format_error(name.error());
        message_is_error_ = true;
        return;
    }
    name->ask_name = true;  // yazma başarısızsa sonraki Ctrl+S yine sorar
    draft_ = std::move(*name);
    const auto created = create_draft();
    if (!created) {
        show_error(created.error());
        return;  // istem açık kalır: metin ve ad kaybolmaz
    }
    mode_ = Mode::Editing;
    show_info(core::tr("tui.saved", {{"path", created->string()}}));
    finish_pending_leave();
}

EditorSession::SaveResult EditorSession::open_name_prompt() {
    if (doc_.path()) {  // diskte silinmiş not: eski adı önerilir
        name_line_.set_text(core::last_component(doc_.path()->string()));
    } else {
        const std::string type = note_type();
        name_line_.set_text(name_stem(type) + "." + type);
    }
    message_.clear();
    message_is_error_ = false;
    mode_ = Mode::NamePrompt;
    return SaveResult::Pending;
}

std::string EditorSession::name_stem(const std::string& type) const {
    return core::slugify(core::note_name_source(open_buffer().text(), type), std::chrono::system_clock::now(),
                         host_.slug_max_length);
}

std::string EditorSession::display_name() const {
    if (doc_.path())
        return doc_.path()->string();
    return draft_ && !draft_->stem.empty() ? draft_->stem : core::tr("tui.editor_untitled");
}

void EditorSession::open(const core::NotePath& path, std::size_t start_line) {
    auto text = doc_.open(host_.store, path);  // hatada belge değişmez
    if (!text) {
        host_.on_error(text.error());
        return;
    }
    draft_.reset();
    buffer_.reset();
    apply_text(std::move(*text));
    open_buffer().move_to({start_line, 0});
    open_buffer().set_scroll_line(start_line);
    mode_ = Mode::Editing;
    host_.on_opened();
}

// Belgenin (doc_) çözdüğü metin tampona girer: açılış, diskten yükleme ve temiz tamponun yenilenmesi.
void EditorSession::apply_text(std::string text) {
    if (autosave_)
        autosave_->cancel();  // tampon diskle aynı
    if (buffer_) {
        buffer_->reload(std::move(text));
    } else {
        buffer_.emplace(std::move(text), host_.cell_width, host_.undo_limit_bytes);
        ++buffer_generation_;
    }
    modes_.clear_find_matches();
    modes_.clear_ghost();
    show_info({});
    refresh_type();
}

void EditorSession::show_error(const core::Error& error) {
    message_ = core::tr("tui.editor_save_failed", {{"detail", core::format_error(error)}});
    message_is_error_ = true;
}

// Boş ileti "boşta" demektir: salt-okunur notta neden sürekli görünür kalır.
void EditorSession::show_info(std::string text) {
    message_ = text.empty() ? doc_.read_only_reason() : std::move(text);
    message_is_error_ = false;
}

int EditorSession::gutter_width() const {
    if (!buffer_)
        return 0;
    constexpr int kMinDigits = 2;  // ilk satırlarda sütun zıplamasın
    constexpr int kDecimalBase = 10;
    constexpr int kNumberGap = 1;
    int digits = 1;
    for (std::size_t lines = buffer_->line_count(); lines >= kDecimalBase; lines /= kDecimalBase)
        ++digits;
    return std::max(digits, kMinDigits) + kNumberGap;
}

void EditorSession::layout(int columns, int rows) {
    columns_ = std::max(columns - gutter_width(), EditorBuffer::kMinColumns);
    rows_ = rows;
    if (buffer_)
        buffer_->scroll_to_cursor(columns_, rows);
}

void EditorSession::click_at(int x, int y) {
    if (!buffer_ || mode_ != Mode::Editing)
        return;
    const std::vector<VisualRow> rows = buffer_->visible_rows(columns_, rows_);
    if (rows.empty() || x < 0 || y < 0)
        return;
    modes_.clear_ghost();
    const std::size_t row = std::min(static_cast<std::size_t>(y), rows.size() - 1);
    buffer_->move_to_cell(rows[row], std::max(x - gutter_width(), 0));
}

void EditorSession::handle(Key key, std::string_view text, const SharedInputSources& sources) {
    if (!buffer_)
        return;
    const std::uint64_t generation = buffer_generation_;
    const std::uint64_t edits = buffer_->edit_count();
    dispatch(key, text, sources);
    if (buffer_ && generation == buffer_generation_ && buffer_->edit_count() != edits)
        restart_autosave();
}

// Her düzenleme süreyi yeniden başlatır: ardışık tuşlar tek yazmaya birleşir. Temize dönen tampon beklemez.
void EditorSession::restart_autosave() {
    if (!autosave_)
        return;
    if (open_buffer().dirty() && doc_.autosaves() && !draft_)
        autosave_->edited(app::Autosave::Clock::now());
    else
        autosave_->cancel();
}

bool EditorSession::tick(std::chrono::steady_clock::time_point now) {
    // Soru ya da girdi açıkken beklenir: kayıt kipe dönünce yapılır.
    if (!autosave_ || !autosave_->due(now) || !buffer_ || mode_ != Mode::Editing)
        return false;
    autosave_->cancel();
    if (!buffer_->dirty() || !doc_.autosaves() || draft_)
        return false;
    const auto written = [&] {
        const std::scoped_lock lock(host_.store_mutex);
        return doc_.autosave(host_.store, open_buffer().text());
    }();
    if (!written)
        show_error(written.error());
    else if (*written)
        adopt_saved();
    return true;
}

void EditorSession::dispatch(Key key, std::string_view text, const SharedInputSources& sources) {
    if (sources)
        name_line_.set_sources(sources);
    switch (mode_) {
    case Mode::Editing:
        handle_editing(key, text);
        break;
    case Mode::Find:
        handle_find(key, text);
        break;
    case Mode::LeavePrompt:
        handle_leave(key);
        break;
    case Mode::ConflictPrompt:
        handle_conflict(key);
        break;
    case Mode::NamePrompt:
        handle_name(key, text);
        break;
    case Mode::Complete:
        handle_complete(key, text);
        break;
    }
}

void EditorSession::handle_editing(Key key, std::string_view text) {
    show_info({});
    const std::size_t size_before = open_buffer().text().size();
    if (modes_.accept_ghost(open_buffer(), key, doc_.writable())) {  // her tuş öneriyi düşürür
        note_draft_edit(size_before);
        return;
    }
    if (apply_motion_key(open_buffer(), key, columns_, rows_))
        return;
    switch (key) {
    case Key::CtrlQ:
        request_leave(AfterLeave::List);
        return;
    case Key::Escape:
        open_buffer().clear_selection();  // kullanıcı kararı: Esc tek başına çıkmaz ve hiçbir şey atmaz
        return;
    case Key::F9:
        request_leave(AfterLeave::Settings);
        return;
    case Key::CtrlS:
        save();
        return;
    case Key::CtrlC:
        copy_to_clipboard(open_buffer().copy());
        return;
    case Key::CtrlF:
        mode_ = Mode::Find;
        modes_.open_find(open_buffer(), shown_type_, current_word_table().get());
        return;
    case Key::F3:
        if (modes_.step_find(open_buffer(), 1))
            show_find_result();
        return;
    default:
        break;
    }
    if (!doc_.writable())
        return;
    if (key == Key::CtrlSpace) {
        modes_.open_completion(open_buffer(), shown_type_, current_word_table().get());
        mode_ = Mode::Complete;
        return;
    }
    apply_edit(key, text);
    note_draft_edit(size_before);
    modes_.clear_find_matches();
    if (key == Key::Char)
        modes_.update_ghost(open_buffer(), doc_.writable(), shown_type_, current_word_table().get());
}

void EditorSession::apply_edit(Key key, std::string_view text) {
    switch (key) {
    case Key::CtrlX:
        copy_to_clipboard(open_buffer().cut());
        break;
    case Key::CtrlV:
        paste_from_clipboard();
        break;
    case Key::Paste:
        open_buffer().paste(text);
        break;
    default:
        apply_edit_key(open_buffer(), key, text);
        break;
    }
}

void EditorSession::copy_to_clipboard(std::string text) {
    if (text.empty())
        return;
    host_.clipboard = std::move(text);
    if (host_.system_copy)
        show_info(host_.system_copy(host_.clipboard));
}

void EditorSession::paste_from_clipboard() {
    if (!host_.system_paste) {
        open_buffer().paste(host_.clipboard);
        return;
    }
    PasteChoice choice = host_.system_paste(host_.clipboard);
    if (!choice.text.empty())
        open_buffer().paste(choice.text);
    if (!choice.message.empty())
        show_info(std::move(choice.message));
}

std::shared_ptr<const WordTable> EditorSession::current_word_table() const {
    return host_.word_table ? host_.word_table() : nullptr;
}

void EditorSession::add_completion_items(std::uint64_t generation,
                                         std::span<const core::lsp::CompletionItem> items) {
    if (mode_ == Mode::Complete)
        modes_.add_completion_items(generation, items);
}

const DiagnosticMark* EditorSession::diagnostic_at_cursor() const {
    return buffer_ ? modes_.diagnostic_at_cursor(*buffer_) : nullptr;
}

void EditorSession::handle_complete(Key key, std::string_view text) {
    const std::size_t size_before = open_buffer().text().size();
    switch (modes_.handle_complete(open_buffer(), key, text)) {
    case EditorModes::CompleteResult::Open:
        return;
    case EditorModes::CompleteResult::Inserted:
        note_draft_edit(size_before);
        break;
    case EditorModes::CompleteResult::Closed:
        break;
    }
    mode_ = Mode::Editing;
}

// Arama koştu: eşleşme yoksa (sorgu dolu) durum satırında söylenir, varsa ileti temizlenir.
void EditorSession::show_find_result() {
    show_info(modes_.find_missed() ? core::tr("tui.editor_not_found") : std::string{});
}

void EditorSession::handle_find(Key key, std::string_view text) {
    switch (modes_.handle_find(open_buffer(), key, text)) {
    case EditorModes::FindResult::Searched:
        show_find_result();
        break;
    case EditorModes::FindResult::Closed:
        mode_ = Mode::Editing;
        show_info({});
        break;
    case EditorModes::FindResult::Handled:
        break;
    case EditorModes::FindResult::Ignored:
        if (key == Key::F9) {
            mode_ = Mode::Editing;
            modes_.close_find();
            request_leave(AfterLeave::Settings);
        }
        break;
    }
}

void EditorSession::request_leave(AfterLeave after) {
    after_leave_ = after;
    if (!open_buffer().dirty()) {
        close();
        return;
    }
    mode_ = Mode::LeavePrompt;
    leave_choice_ = LeaveChoice::Save;
}

void EditorSession::handle_leave(Key key) {
    if (const int delta = choice_delta(key); delta != 0) {
        leave_choice_ = cycled(leave_choice_, kLeaveChoices, delta);
        return;
    }
    if (key == Key::Escape || (key == Key::Enter && leave_choice_ == LeaveChoice::Cancel)) {
        mode_ = Mode::Editing;
        return;
    }
    if (key != Key::Enter)
        return;
    if (leave_choice_ == LeaveChoice::Discard) {
        close();
        return;
    }
    mode_ = Mode::Editing;
    leave_pending_ = true;
    switch (save()) {
    case SaveResult::Saved:
        finish_pending_leave();
        break;
    case SaveResult::Pending:
        break;  // çakışma diyaloğu sonucu belirler
    case SaveResult::Failed:
        cancel_pending_leave();  // metin kaybolmasın diye çıkılmaz; hata durum satırında
        break;
    }
}

void EditorSession::finish_pending_leave() {
    if (leave_pending_)
        close();
}

void EditorSession::cancel_pending_leave() {
    leave_pending_ = false;
}

void EditorSession::close() {
    const AfterLeave after = after_leave_;
    // Var olan not Önizlemede aynı yerden sürer; taslağın gösterilecek dosyası yok.
    const std::optional<std::size_t> line =
        doc_.path() && buffer_ ? std::optional(buffer_->scroll_top().line) : std::nullopt;
    buffer_.reset();
    doc_.reset_draft();
    draft_.reset();
    if (autosave_)
        autosave_->cancel();
    modes_.clear_find_matches();
    modes_.clear_ghost();
    modes_.set_diagnostics({});
    mode_ = Mode::Editing;
    leave_pending_ = false;
    after_leave_ = AfterLeave::List;
    message_.clear();
    message_is_error_ = false;
    host_.on_closed(line);
    if (after == AfterLeave::Settings)
        host_.open_settings();
}

std::string EditorSession::body() const {
    return doc_.encode(open_buffer().text());
}

// Belge yeni revizyonu zaten aldı: tampon temizlenir, dış dünya haberdar edilir.
void EditorSession::adopt_saved() {
    if (autosave_)
        autosave_->cancel();
    open_buffer().mark_saved();
    host_.on_saved(note_path());
}

EditorSession::SaveResult EditorSession::save() {
    if (!doc_.writable())
        return SaveResult::Failed;
    if (doc_.deleted_on_disk())
        return ask_new_place();
    if (!open_buffer().dirty()) {
        show_info(core::tr("tui.editor_no_changes"));
        return SaveResult::Saved;
    }
    if (draft_)
        return draft_->ask_name ? open_name_prompt() : save_draft();
    const auto saved = [&] {
        const std::scoped_lock lock(host_.store_mutex);
        return doc_.save(host_.store, open_buffer().text());
    }();
    if (!saved) {
        show_error(saved.error());
        return SaveResult::Failed;
    }
    switch (*saved) {
    case app::Document::Saved::Written:
        adopt_saved();
        show_info(core::tr("tui.editor_saved"));
        return SaveResult::Saved;
    case app::Document::Saved::Conflict:
        mode_ = Mode::ConflictPrompt;
        conflict_choice_ = app::kDefaultConflictChoice;
        return SaveResult::Pending;
    case app::Document::Saved::Gone:
        break;
    }
    return ask_new_place();
}

// Not diskte yok: GUI gibi farklı kaydet. Tampon eski yer ve adla taslağa döner (çıkışta soru sorulamazsa
// oraya yazılır); istem aynı yeri önerir, Enter notu yeniden yaratır, başka ad seçilebilir. Belge eski yolu
// ve baytları (BOM, satır sonu) korur.
EditorSession::SaveResult EditorSession::ask_new_place() {
    const core::NoteParts parts = core::split_note_path(note_path().string());
    name_folder_ = parts.folder;
    draft_ = DraftName{parts.folder, parts.name, parts.type, false, true};
    refresh_type();
    return open_name_prompt();
}

// Yazılan bayt tamponla aynı kalsın; başarısızlıkta dosya kalmaz, yol sahiplenilmez: sonraki kayıt aynı adı
// dener. Kilit bırakıldıktan sonra çağrılır: on_saved listeyi aynı kilitle yeniler.
std::expected<core::NotePath, core::Error>
EditorSession::adopt_created(std::expected<core::NotePath, core::Error> created) {
    if (!created)
        return created;
    draft_.reset();
    refresh_type();
    adopt_saved();
    return created;
}

std::expected<core::NotePath, core::Error> EditorSession::create_note(std::string_view name_suffix) {
    return adopt_created([&] {
        const std::scoped_lock lock(host_.store_mutex);
        return doc_.create_beside(host_.store, open_buffer().text(), name_suffix);
    }());
}

std::expected<core::NotePath, core::Error> EditorSession::create_draft() {
    const std::string type = note_type();
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- çağıranların hepsi draft_'ı denetler
    const DraftName& draft = *draft_;
    std::string stem = draft.stem;
    if (draft.timestamp_name)
        stem = core::slugify("", std::chrono::system_clock::now(), host_.slug_max_length);
    else if (stem.empty() && draft.ask_name)
        stem = name_stem(type);  // ad sorulmadan çıkış: istemde önerilecek adın aynısı
    return adopt_created([&] {
        const std::scoped_lock lock(host_.store_mutex);
        return doc_.create(host_.store, core::NewNote{draft.folder, stem, type, {}}, open_buffer().text());
    }());
}

// Taslakta açık uzantı kazanır, yoksa içerikten detect; var olan notta dosyanın uzantısı.
std::string EditorSession::note_type() const {
    if (draft_)
        return draft_->type.empty() ? host_.suggest_type(open_buffer().text()) : draft_->type;
    std::string type = to_utf8(note_path().value().extension());
    if (!type.empty())
        type.erase(0, 1);
    return type;
}

EditorSession::SaveResult EditorSession::save_draft() {
    const auto created = create_draft();
    if (!created) {
        show_error(created.error());
        return SaveResult::Failed;
    }
    show_info(core::tr("tui.saved", {{"path", created->string()}}));
    return SaveResult::Saved;
}

EditorSession::SaveResult EditorSession::save_as_new_note(std::string_view name_suffix,
                                                          const char* message_key) {
    const auto created = create_note(name_suffix);
    if (!created) {
        show_error(created.error());
        return SaveResult::Failed;
    }
    show_info(core::tr(message_key, {{"path", created->string()}}));
    return SaveResult::Saved;
}

bool EditorSession::overwrite_disk() {
    const auto written = [&] {
        const std::scoped_lock lock(host_.store_mutex);
        return doc_.overwrite(host_.store, open_buffer().text());  // kullanıcı bilerek ezdi
    }();
    if (!written) {
        show_error(written.error());
        return false;
    }
    adopt_saved();
    show_info(core::tr("tui.editor_saved"));
    return true;
}

bool EditorSession::load_disk() {
    auto text = doc_.reload(host_.store);
    if (!text) {
        show_error(text.error());
        return false;
    }
    apply_text(std::move(*text));
    return true;
}

void EditorSession::handle_conflict(Key key) {
    if (const int delta = choice_delta(key); delta != 0) {
        conflict_choice_ = cycled(conflict_choice_, kConflictChoices, delta);
        return;
    }
    if (key != Key::Enter && key != Key::Escape)
        return;
    mode_ = Mode::Editing;
    bool resolved = false;
    if (key == Key::Enter) {
        switch (conflict_choice_) {
        case ConflictChoice::SaveCopy:
            resolved = save_as_new_note(app::kCopySuffix, "tui.editor_saved_copy") == SaveResult::Saved;
            break;
        case ConflictChoice::Reload:
            resolved = load_disk();
            break;
        case ConflictChoice::Overwrite:
            resolved = overwrite_disk();
            break;
        case ConflictChoice::Cancel:
            break;
        }
    }
    if (resolved)
        finish_pending_leave();
    else
        cancel_pending_leave();
}

void EditorSession::on_listing(std::span<const core::NoteInfo> notes) {
    if (!buffer_ || !doc_.path())
        return;  // taslağın diskte karşılığı yok
    const core::NotePath path = *doc_.path();
    const auto found = std::ranges::find(notes, path, &core::NoteInfo::path);
    if (found != notes.end() && found->revision == doc_.revision() && !doc_.deleted_on_disk())
        return;
    // Liste bizim kaydımızdan önce alınmış olabilir: karar için disk yeniden okunur.
    auto content = host_.store.read(path);
    if (!content && content.error().code != core::Error::Code::NotFound)
        return;
    const app::DiskChange change = app::disk_change(content ? std::optional(content->revision) : std::nullopt,
                                                    doc_.revision(), buffer_->dirty());
    switch (change) {
    case app::DiskChange::Close:
        doc_.reset_draft();  // Önizlenecek dosya kalmadı: Listeye dönülür
        close();
        host_.on_error(content.error());
        return;
    case app::DiskChange::Reload:
        apply_text(doc_.load(std::move(*content)));
        return;
    case app::DiskChange::None:
    case app::DiskChange::Changed:
    case app::DiskChange::Deleted:
        doc_.note_disk_change(change);
        return;
    }
}

// Çıkışta soru sorulamaz: taslak oluşturulur, çakışmada varsayılan kopya, diskteki sürüm asla ezilmez.
std::expected<core::NotePath, core::Error> EditorSession::flush_write() {
    if (draft_)
        return create_draft();
    const auto saved = [&] {
        const std::scoped_lock lock(host_.store_mutex);
        return doc_.save(host_.store, open_buffer().text());
    }();
    if (!saved)
        return std::unexpected(saved.error());
    switch (*saved) {
    case app::Document::Saved::Written:
        adopt_saved();
        return note_path();
    case app::Document::Saved::Conflict:
        return create_note(app::kCopySuffix);
    case app::Document::Saved::Gone:
        break;
    }
    return create_note({});
}

std::string EditorSession::flush_on_exit() {
    if (!buffer_ || !doc_.writable() || !buffer_->dirty())
        return {};
    const std::string shown_path = display_name();
    const auto saved = flush_write();
    if (saved)
        return core::tr("tui.editor_exit_saved", {{"path", saved->string()}});
    const auto temp = core::create_private_temp_file(note_type(), body());
    if (!temp)
        return core::tr("tui.editor_exit_lost",
                        {{"path", shown_path}, {"detail", core::format_error(temp.error())}});
    return core::tr(
        "tui.editor_exit_kept",
        {{"path", shown_path}, {"detail", core::format_error(saved.error())}, {"temp", to_utf8(*temp)}});
}

}  // namespace noteapp::tui
