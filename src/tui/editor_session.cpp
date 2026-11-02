#include "tui/editor_session.hpp"

#include "core/editor.hpp"
#include "core/i18n.hpp"
#include "core/text.hpp"
#include "tui/save_dialog.hpp"

#include <algorithm>
#include <memory>
#include <utility>

namespace noteapp::tui {

namespace {

constexpr std::string_view kBom = "\xEF\xBB\xBF";
constexpr std::string_view kReplacement = "\xEF\xBF\xBD";  // U+FFFD
constexpr char kDelete = 0x7F;
constexpr std::string_view kCopySuffix = " (copy)";  // GUI'nin kopya adıyla aynı
constexpr std::uintmax_t kBytesPerMebibyte = 1024 * 1024;

std::string to_utf8(const std::filesystem::path& p) {
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

std::size_t sequence_length(unsigned char lead) {
    if (lead >= 0xF0)
        return 4;
    if (lead >= 0xE0)
        return 3;
    return lead >= 0xC0 ? 2 : 1;
}

// Salt-okunur görünüm için: imleç hesabı geçerli UTF-8 ister. Bu metin yalnız gösterilir, asla yazılmaz.
std::string sanitized(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());
    for (std::size_t at = 0; at < raw.size();) {
        const auto lead = static_cast<unsigned char>(raw[at]);
        const std::size_t length = std::min(sequence_length(lead), raw.size() - at);
        const std::string_view sequence = raw.substr(at, length);
        if (lead != 0 && core::is_valid_utf8(sequence)) {
            out += sequence;
            at += length;
        } else {
            out += kReplacement;
            ++at;
        }
    }
    return out;
}

EditorSession::ReadOnly classify(std::string_view raw, std::uintmax_t max_bytes) {
    using ReadOnly = EditorSession::ReadOnly;
    if (raw.size() > max_bytes)
        return ReadOnly::TooLarge;
    if (core::looks_binary(raw))
        return ReadOnly::Binary;
    if (raw.find('\0') != std::string_view::npos)
        return ReadOnly::EmbeddedNul;
    return core::is_valid_utf8(raw) ? ReadOnly::No : ReadOnly::InvalidUtf8;
}

std::string read_only_reason(EditorSession::ReadOnly reason, std::uintmax_t max_bytes) {
    using ReadOnly = EditorSession::ReadOnly;
    switch (reason) {
    case ReadOnly::Binary:
        return core::tr("tui.editor_readonly_binary");
    case ReadOnly::InvalidUtf8:
        return core::tr("tui.editor_readonly_utf8");
    case ReadOnly::EmbeddedNul:
        return core::tr("tui.editor_readonly_nul");
    case ReadOnly::TooLarge:
        return core::tr("tui.editor_readonly_large",
                        {{"limit", std::to_string(max_bytes / kBytesPerMebibyte)}});
    case ReadOnly::No:
        break;
    }
    return {};
}

bool is_typable(std::string_view text) {
    return !text.empty() && static_cast<unsigned char>(text.front()) >= 0x20 && text.front() != kDelete &&
           core::is_valid_utf8(text);
}

void pop_code_point(std::string& s) {
    while (s.size() > 1 && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80)
        s.pop_back();
    if (!s.empty())
        s.pop_back();
}

struct Motion {
    Key key;
    void (*apply)(EditorBuffer&, int columns, int rows);
};

// Hareketler salt-okunurda da çalışır: "→ yine de kaydırmaya izin verir".
constexpr Motion kMotions[] = {
    {Key::Left, [](EditorBuffer& b, int, int) { b.move_left(false); }},
    {Key::Right, [](EditorBuffer& b, int, int) { b.move_right(false); }},
    {Key::Up, [](EditorBuffer& b, int c, int) { b.move_rows(-1, c, false); }},
    {Key::Down, [](EditorBuffer& b, int c, int) { b.move_rows(1, c, false); }},
    {Key::Home, [](EditorBuffer& b, int, int) { b.move_home(false); }},
    {Key::End, [](EditorBuffer& b, int, int) { b.move_end(false); }},
    {Key::PageUp, [](EditorBuffer& b, int c, int r) { b.move_rows(-std::max(r - 1, 1), c, false); }},
    {Key::PageDown, [](EditorBuffer& b, int c, int r) { b.move_rows(std::max(r - 1, 1), c, false); }},
    {Key::CtrlHome, [](EditorBuffer& b, int, int) { b.move_doc_start(false); }},
    {Key::CtrlEnd, [](EditorBuffer& b, int, int) { b.move_doc_end(false); }},
    {Key::ShiftLeft, [](EditorBuffer& b, int, int) { b.move_left(true); }},
    {Key::ShiftRight, [](EditorBuffer& b, int, int) { b.move_right(true); }},
    {Key::ShiftUp, [](EditorBuffer& b, int c, int) { b.move_rows(-1, c, true); }},
    {Key::ShiftDown, [](EditorBuffer& b, int c, int) { b.move_rows(1, c, true); }},
    {Key::ShiftHome, [](EditorBuffer& b, int, int) { b.move_home(true); }},
    {Key::ShiftEnd, [](EditorBuffer& b, int, int) { b.move_end(true); }},
};

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

// Taslakta detect yeniden çalışmadan önce değişmesi gereken pay (metnin 1/32'si). Küçük metinde her tuş,
// büyük metinde seyrek: toplam maliyet yazılan baytla doğrusal kalır.
constexpr unsigned kRedetectShift = 5;

constexpr std::size_t kCompletionRows = 8;
constexpr int kLeaveChoices = 3;
constexpr int kConflictChoices = 4;

}  // namespace

std::string sanitized_utf8(std::string_view raw) {
    return sanitized(raw);
}

std::expected<DraftName, core::Error> parse_draft_name(std::string_view input,
                                                       std::string_view default_folder,
                                                       std::chrono::system_clock::time_point now,
                                                       std::size_t slug_max_length) {
    DraftName name;
    name.folder = default_folder;
    std::string_view rest = input;
    if (const std::size_t slash = rest.rfind('/'); slash != std::string_view::npos) {
        name.folder = rest.substr(0, slash);
        rest.remove_prefix(slash + 1);
    }
    // Gizli ad NotePath'ten geçemez: slug ona sessizce başka bir ad verirdi (`.env` -> `env.md`).
    if (rest.starts_with('.'))
        if (auto hidden = core::NotePath::parse(rest); !hidden)
            return std::unexpected(hidden.error());
    const core::TypedName typed = core::split_typed_name(rest);
    name.type = typed.extension;
    rest = typed.stem;
    name.timestamp_name = rest.empty();
    if (!rest.empty())
        name.stem = core::slugify_stem(rest, now, slug_max_length);
    // create() ancak kayıtta denerdi: geçersiz yol istemde görünsün, yazılan metin sonradan reddedilmesin.
    const std::string probe_stem = name.stem.empty() ? "probe" : name.stem;
    const std::string probe_type = name.type.empty() ? "txt" : name.type;
    const std::string prefix = name.folder.empty() ? std::string{} : name.folder + "/";
    if (auto path = core::NotePath::parse(prefix + probe_stem + "." + probe_type); !path)
        return std::unexpected(path.error());
    return name;
}

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
    path_.reset();
    bom_ = false;
    read_only_ = ReadOnly::No;
    buffer_.emplace(std::string{}, host_.cell_width);
    find_matches_.clear();
    mode_ = Mode::Editing;
    show_info({});
    refresh_type();
}

void EditorSession::refresh_type() {
    draft_changes_ = 0;
    std::string type = note_type();
    if (type == shown_type_)
        return;
    language_ = syntax_language_for(type);
    shown_type_ = std::move(type);
    syntax_cache_.clear();
}

void EditorSession::note_draft_edit(std::size_t size_before) {
    if (!draft_ || !draft_->type.empty())
        return;
    const std::size_t size = open_buffer().text().size();
    draft_changes_ += std::max<std::size_t>(size > size_before ? size - size_before : size_before - size, 1);
    if (draft_changes_ >= std::max<std::size_t>(size >> kRedetectShift, 1))
        refresh_type();
}

std::vector<SyntaxSpan> EditorSession::syntax_spans(std::size_t line) const {
    if (language_ == SyntaxLanguage::Plain || !buffer_ || buffer_->text().size() > kMaxHighlightBytes)
        return {};
    syntax_cache_.invalidate_from(buffer_->take_changed_line());
    const SyntaxState entry = syntax_cache_.entry_state(
        language_, line, [this](std::size_t index) { return buffer_->line_text(index); });
    std::vector<SyntaxSpan> spans;
    highlight_line(language_, buffer_->line_text(line), entry, &spans);
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
    const std::string type = note_type();
    name_line_.set_text(name_stem(type) + "." + type);
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
    if (path_)
        return path_->string();
    return draft_ && !draft_->stem.empty() ? draft_->stem : core::tr("tui.editor_untitled");
}

std::string EditorSession::find_label() const {
    if (find_matches_.empty())
        return {};
    return match_label(find_index_, find_matches_.size());
}

void EditorSession::open(const core::NotePath& path, std::size_t start_line) {
    auto content = host_.store.read(path);
    if (!content) {
        host_.on_error(content.error());
        return;
    }
    path_ = path;
    draft_.reset();
    buffer_.reset();
    apply_content(std::move(content->text), content->revision);
    open_buffer().move_to({start_line, 0});
    open_buffer().set_scroll_line(start_line);
    mode_ = Mode::Editing;
    host_.on_opened();
}

// mtime, write()'ın beklediği değerdir ve içerikle aynı anda okunmuş olmalıdır (GuiState::load_text ile
// aynı).
void EditorSession::apply_content(std::string raw, const core::Revision& revision) {
    read_only_ = classify(raw, host_.max_bytes);
    bom_ = false;
    if (read_only_ != ReadOnly::No) {
        if (read_only_ == ReadOnly::TooLarge)
            raw.resize(static_cast<std::size_t>(host_.max_bytes));
        raw = sanitized(raw);
    } else if (raw.starts_with(kBom)) {
        bom_ = true;
        raw.erase(0, kBom.size());
    }
    if (buffer_)
        buffer_->reload(std::move(raw));
    else
        buffer_.emplace(std::move(raw), host_.cell_width);
    revision_ = revision;
    deleted_on_disk_ = false;
    changed_on_disk_ = false;
    find_matches_.clear();
    ghost_.clear();
    show_info({});
    refresh_type();
}

void EditorSession::show_error(const core::Error& error) {
    message_ = core::tr("tui.editor_save_failed", {{"detail", core::format_error(error)}});
    message_is_error_ = true;
}

// Boş ileti "boşta" demektir: salt-okunur notta neden sürekli görünür kalır.
void EditorSession::show_info(std::string text) {
    message_ = text.empty() ? read_only_reason(read_only_, host_.max_bytes) : std::move(text);
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
    ghost_.clear();
    const std::size_t row = std::min(static_cast<std::size_t>(y), rows.size() - 1);
    buffer_->move_to_cell(rows[row], std::max(x - gutter_width(), 0));
}

void EditorSession::handle(Key key, std::string_view text, const SharedInputSources& sources) {
    if (!buffer_)
        return;
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
    const std::string ghost = std::exchange(ghost_, {});  // her tuş öneriyi düşürür; yazım yenisini üretir
    if (!ghost.empty() && (key == Key::Tab || key == Key::Right)) {
        const std::size_t size_before = open_buffer().text().size();
        open_buffer().paste(ghost);
        note_draft_edit(size_before);
        find_matches_.clear();
        return;
    }
    if (apply_motion(key))
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
        find_origin_ = open_buffer().cursor();
        find_line_.clear();
        find_line_.set_sources(word_input_sources(completion_candidates(
            open_buffer().text(), open_buffer().cursor(), shown_type_, current_word_table().get())));
        find_matches_.clear();
        return;
    case Key::F3:
        step_find(1);
        return;
    default:
        break;
    }
    if (read_only_ != ReadOnly::No)
        return;
    if (key == Key::CtrlSpace) {
        open_completion();
        return;
    }
    const std::size_t size_before = open_buffer().text().size();
    apply_edit(key, text);
    note_draft_edit(size_before);
    find_matches_.clear();  // ofsetler eski metne ait; F3 yeniden hesaplar
    if (key == Key::Char)
        update_ghost();
}

bool EditorSession::apply_motion(Key key) {
    const auto* motion = std::ranges::find(kMotions, key, &Motion::key);
    if (motion == std::ranges::end(kMotions))
        return false;
    motion->apply(open_buffer(), columns_, rows_);
    return true;
}

void EditorSession::apply_edit(Key key, std::string_view text) {
    switch (key) {
    case Key::Char:
        if (is_typable(text))
            open_buffer().type_text(text);
        break;
    case Key::Backspace:
        open_buffer().backspace();
        break;
    case Key::Delete:
        open_buffer().delete_forward();
        break;
    case Key::Enter:
        open_buffer().newline();
        break;
    case Key::Tab:
        open_buffer().type_text("\t");
        break;
    case Key::CtrlX:
        copy_to_clipboard(open_buffer().cut());
        break;
    case Key::CtrlV:
        paste_from_clipboard();
        break;
    case Key::CtrlZ:
        open_buffer().undo();
        break;
    case Key::CtrlY:
        open_buffer().redo();
        break;
    default:
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

// Yalnız satır sonunda: metnin ortasında soluk yazı ardındaki metni örterdi.
void EditorSession::update_ghost() {
    ghost_.clear();
    const std::string& text = open_buffer().text();
    const std::size_t cursor = open_buffer().cursor();
    const bool at_line_end = cursor == text.size() || text[cursor] == '\n' || text[cursor] == '\r';
    if (read_only_ != ReadOnly::No || !at_line_end || open_buffer().selection())
        return;
    const auto table = current_word_table();
    ghost_ = suggest_completion(text, cursor, shown_type_, table.get()).value_or(std::string{});
}

void EditorSession::open_completion() {
    const std::string& text = open_buffer().text();
    const std::size_t cursor = open_buffer().cursor();
    const auto table = current_word_table();
    completion_candidates_ = completion_candidates(text, cursor, shown_type_, table.get());
    completion_query_ = std::string(word_prefix_at(text, cursor));
    completion_selected_ = 0;
    refresh_completion();
    mode_ = Mode::Complete;
}

void EditorSession::refresh_completion() {
    completion_items_ = filter_candidates(completion_candidates_, completion_query_, kCompletionRows);
    completion_selected_ =
        std::min(completion_selected_, completion_items_.empty() ? 0 : completion_items_.size() - 1);
}

void EditorSession::insert_completion() {
    if (completion_items_.empty())
        return;
    const std::size_t cursor = open_buffer().cursor();
    const std::size_t typed = word_prefix_at(open_buffer().text(), cursor).size();
    const std::size_t size_before = open_buffer().text().size();
    open_buffer().select_range({cursor - typed, cursor});
    open_buffer().paste(completion_items_[completion_selected_]);
    note_draft_edit(size_before);
    find_matches_.clear();
}

void EditorSession::handle_complete(Key key, std::string_view text) {
    switch (key) {
    case Key::Char:
        if (is_typable(text)) {
            completion_query_.append(text);
            refresh_completion();
        }
        break;
    case Key::Backspace:
        pop_code_point(completion_query_);
        refresh_completion();
        break;
    case Key::Up:
    case Key::CtrlP:
        completion_selected_ -= completion_selected_ > 0 ? 1 : 0;
        break;
    case Key::Down:
    case Key::CtrlN:
        completion_selected_ += completion_selected_ + 1 < completion_items_.size() ? 1 : 0;
        break;
    case Key::Enter:
        insert_completion();
        mode_ = Mode::Editing;
        break;
    case Key::Escape:
    case Key::CtrlSpace:
        mode_ = Mode::Editing;
        break;
    default:
        break;
    }
}

// Eşleşmeleri yeniden hesaplar; geçerli eşleşme `from`dan sonraki ilkidir, yoksa başa sarar.
void EditorSession::recompute_find(std::size_t from) {
    find_matches_ = find_folded(open_buffer().text(), find_line_.text());
    const auto next =
        std::ranges::find_if(find_matches_, [from](const ByteRange& m) { return m.begin >= from; });
    find_index_ = next == find_matches_.end() ? 0 : static_cast<std::size_t>(next - find_matches_.begin());
}

void EditorSession::select_current_match() {
    if (find_matches_.empty()) {
        if (!find_line_.text().empty())
            show_info(core::tr("tui.editor_not_found"));
        return;
    }
    show_info({});
    open_buffer().select_range(find_matches_[find_index_]);
}

void EditorSession::update_find() {
    open_buffer().select_range({find_origin_, find_origin_});
    show_info({});
    recompute_find(find_origin_);
    select_current_match();
}

void EditorSession::step_find(int delta) {
    if (find_line_.text().empty())
        return;
    if (find_matches_.empty()) {
        // Düzenleme eşleşmeleri düşürdü: imleçten sonraki ilk eşleşmeye gidilir, atlanmaz.
        recompute_find(open_buffer().cursor());
    } else {
        const std::size_t count = find_matches_.size();
        find_index_ = (find_index_ + count + (delta > 0 ? 1 : count - 1)) % count;
    }
    select_current_match();
}

void EditorSession::handle_find(Key key, std::string_view text) {
    const InputLine::Result result = find_line_.handle(key, text);
    if ((key == Key::Backspace || result == InputLine::Result::Edited) && !find_line_.popup())
        update_find();
    if (result != InputLine::Result::Ignored)
        return;
    switch (key) {
    case Key::Enter:
    case Key::Down:
    case Key::CtrlN:
    case Key::F3:
        step_find(1);
        break;
    case Key::Up:
    case Key::CtrlP:
        step_find(-1);
        break;
    case Key::Escape:
        mode_ = Mode::Editing;
        find_matches_.clear();
        show_info({});
        break;
    case Key::F9:
        mode_ = Mode::Editing;
        find_matches_.clear();
        request_leave(AfterLeave::Settings);
        break;
    default:
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
        path_ && buffer_ ? std::optional(buffer_->scroll_top().line) : std::nullopt;
    buffer_.reset();
    path_.reset();
    draft_.reset();
    find_matches_.clear();
    ghost_.clear();
    mode_ = Mode::Editing;
    leave_pending_ = false;
    after_leave_ = AfterLeave::List;
    read_only_ = ReadOnly::No;
    deleted_on_disk_ = false;
    changed_on_disk_ = false;
    message_.clear();
    message_is_error_ = false;
    host_.on_closed(line);
    if (after == AfterLeave::Settings)
        host_.open_settings();
}

std::string EditorSession::body() const {
    return (bom_ ? std::string(kBom) : std::string{}) + open_buffer().text();
}

void EditorSession::adopt_saved(const core::Revision& revision) {
    revision_ = revision;
    open_buffer().mark_saved();
    deleted_on_disk_ = false;
    changed_on_disk_ = false;
    host_.on_saved(note_path());
}

EditorSession::SaveResult EditorSession::save() {
    if (read_only_ != ReadOnly::No)
        return SaveResult::Failed;
    if (deleted_on_disk_)
        return save_as_new_note({}, "tui.editor_saved_new");
    if (!open_buffer().dirty()) {
        show_info(core::tr("tui.editor_no_changes"));
        return SaveResult::Saved;
    }
    if (draft_)
        return draft_->ask_name ? open_name_prompt() : save_draft();
    const auto written = [&] {
        const std::scoped_lock lock(host_.store_mutex);
        return host_.store.write(*path_, body(), revision_);
    }();
    if (written) {
        adopt_saved(*written);
        show_info(core::tr("tui.editor_saved"));
        return SaveResult::Saved;
    }
    switch (written.error().code) {
    case core::Error::Code::ModifiedExternally:
        mode_ = Mode::ConflictPrompt;
        conflict_choice_ = ConflictChoice::SaveCopy;
        return SaveResult::Pending;
    case core::Error::Code::NotFound:
        return save_as_new_note({}, "tui.editor_saved_new");
    default:
        show_error(written.error());
        return SaveResult::Failed;
    }
}

// Yazılan bayt tamponla aynı kalsın; başarısızlıkta dosya kalmaz, yol sahiplenilmez: sonraki kayıt aynı adı
// dener.
std::expected<core::NotePath, core::Error> EditorSession::create_and_adopt(core::NewNote note) {
    auto created = [&] {
        const std::scoped_lock lock(host_.store_mutex);
        return host_.store.create_exact(note);
    }();
    if (!created)
        return std::unexpected(created.error());
    path_ = created->path;
    draft_.reset();
    refresh_type();
    adopt_saved(created->revision);  // kilit bırakıldı: on_saved listeyi aynı kilitle yeniler
    return created->path;
}

std::expected<core::NotePath, core::Error> EditorSession::create_note(std::string_view name_suffix) {
    const std::filesystem::path& file = note_path().value();
    return create_and_adopt(core::NewNote{initial_folder(&note_path(), ""),
                                          to_utf8(file.stem()) + std::string(name_suffix), note_type(),
                                          body()});
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
    return create_and_adopt(core::NewNote{draft.folder, stem, type, body()});
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
    const auto fresh = host_.store.read(note_path());
    if (!fresh) {
        show_error(fresh.error());
        return false;
    }
    const auto written = [&] {
        const std::scoped_lock lock(host_.store_mutex);
        return host_.store.write(*path_, body(), fresh->revision);  // kullanıcı bilerek ezdi
    }();
    if (!written) {
        show_error(written.error());
        return false;
    }
    adopt_saved(*written);
    show_info(core::tr("tui.editor_saved"));
    return true;
}

bool EditorSession::load_disk() {
    auto fresh = host_.store.read(note_path());
    if (!fresh) {
        show_error(fresh.error());
        return false;
    }
    apply_content(std::move(fresh->text), fresh->revision);
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
            resolved = save_as_new_note(kCopySuffix, "tui.editor_saved_copy") == SaveResult::Saved;
            break;
        case ConflictChoice::LoadDisk:
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
    if (!buffer_ || !path_)
        return;  // taslağın diskte karşılığı yok
    const auto found = std::ranges::find(notes, *path_, &core::NoteInfo::path);
    if (found != notes.end() && found->revision == revision_ && !deleted_on_disk_)
        return;
    // Liste bizim kaydımızdan önce alınmış olabilir: karar için disk yeniden okunur.
    auto content = host_.store.read(*path_);
    if (!content) {
        if (content.error().code != core::Error::Code::NotFound)
            return;
        if (buffer_->dirty()) {
            deleted_on_disk_ = true;
            changed_on_disk_ = false;
            return;
        }
        path_.reset();  // Önizlenecek dosya kalmadı: Listeye dönülür
        close();
        host_.on_error(content.error());
        return;
    }
    deleted_on_disk_ = false;
    if (content->revision == revision_)
        return;
    if (buffer_->dirty())
        changed_on_disk_ = true;
    else
        apply_content(std::move(content->text), content->revision);
}

// Çıkışta soru sorulamaz: taslak oluşturulur, çakışmada varsayılan kopya, diskteki sürüm asla ezilmez.
std::expected<core::NotePath, core::Error> EditorSession::flush_write() {
    if (draft_)
        return create_draft();
    core::NotePath path = note_path();
    const auto written = [&] {
        const std::scoped_lock lock(host_.store_mutex);
        return host_.store.write(path, body(), revision_);
    }();
    if (written) {
        adopt_saved(*written);
        return path;
    }
    switch (written.error().code) {
    case core::Error::Code::ModifiedExternally:
        return create_note(kCopySuffix);
    case core::Error::Code::NotFound:
        return create_note({});
    default:
        return std::unexpected(written.error());
    }
}

std::string EditorSession::flush_on_exit() {
    if (!buffer_ || read_only_ != ReadOnly::No || !buffer_->dirty())
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
