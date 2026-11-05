#include "core/i18n.hpp"
#include "core/log.hpp"
#include "gui/view_internal.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <imgui.h>
#include <imgui_stdlib.h>
#include <iterator>
#include <mutex>
#include <optional>
#include <string_view>
#include <utility>

namespace noteapp::gui {

namespace {

const SDL_DialogFileFilter kPdfFilters[] = {{"PDF", "pdf"}};

// Yerel diyaloğun sonucu: SDL geri çağrısı başka iş parçacığından ve modal kapandıktan sonra da gelebilir.
struct NativeDialogResult {
    std::mutex mutex;
    std::optional<std::string> path;
};

void SDLCALL native_dialog_done(void* userdata, const char* const* files, int /*filter*/) {
    const std::unique_ptr<std::shared_ptr<NativeDialogResult>> result(
        static_cast<std::shared_ptr<NativeDialogResult>*>(userdata));
    if (files == nullptr) {  // portal/zenity yok: yol alanı açık kalır, kullanıcı elle yazar
        core::log_warn(std::string("event=file_dialog_failed detail=") + SDL_GetError());
    } else if (files[0] != nullptr) {
        const std::scoped_lock lock((*result)->mutex);
        (*result)->path = files[0];
    }
    wake_event_loop();
}

// Çoklu seçim: her yol kutuya; boş liste vazgeçmedir. Kutu paylaşılır: View kapanmış olsa da yazılabilir.
void SDLCALL import_dialog_done(void* userdata, const char* const* files, int /*filter*/) {
    const std::unique_ptr<std::shared_ptr<ImportInbox>> inbox(
        static_cast<std::shared_ptr<ImportInbox>*>(userdata));
    {
        const std::scoped_lock lock((*inbox)->mutex);
        if (files == nullptr)
            (*inbox)->error = SDL_GetError();
        else
            for (; *files != nullptr; ++files) {
                const std::string_view path(*files);
                (*inbox)->paths.emplace_back(std::u8string(path.begin(), path.end()));
            }
    }
    wake_event_loop();
}

// Sondaki etiket ya da düğme için yer bırakan form alanı genişliği.
float form_item_width(float trailing_width, float available) {
    const ImGuiStyle& style = ImGui::GetStyle();
    return std::clamp(available - trailing_width - style.ItemSpacing.x, ImGui::GetFontSize() * kMinFieldEms,
                      kFormWidth * style.FontScaleDpi);
}

// Bir kez konumlanan diyalog (kullanıcı da taşıyabilir) pencere küçülünce ya da içerik büyüyünce dışarı
// taşmasın.
void keep_window_in_viewport() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 size = ImGui::GetWindowSize();
    const ImVec2 position = ImGui::GetWindowPos();
    const float x =
        std::clamp(position.x, viewport->WorkPos.x,
                   std::max(viewport->WorkPos.x, viewport->WorkPos.x + viewport->WorkSize.x - size.x));
    const float y =
        std::clamp(position.y, viewport->WorkPos.y,
                   std::max(viewport->WorkPos.y, viewport->WorkPos.y + viewport->WorkSize.y - size.y));
    if (x != position.x || y != position.y)
        ImGui::SetWindowPos(ImVec2(x, y));
}

}  // namespace

void View::install_deciders() {
    state_.deciders.save_dialog = [this](const SaveDialogRequest& request) {
        return ask_save_details(request);
    };
    state_.deciders.conflict = [this](const std::string& path) {
        return ask_choice(make_conflict_box(path));
    };
    state_.deciders.close_unsaved = [this] { return ask_choice(make_close_box()); };
    state_.deciders.close_unsaved_settings = [this] { return ask_choice(make_close_settings_box()); };
    state_.deciders.confirm_delete = [this](const std::string& message) {
        return ask_choice(make_delete_box(message));
    };
    state_.deciders.folder_name = [this](const std::string& title, const std::string& initial) {
        return ask_folder_name(title, initial);
    };
    state_.deciders.link_target = [this](const std::string& title) {
        return ask_folder_name(title, {}, core::tr("gui.link_label"));
    };
    state_.deciders.palette = [this] { return ask_palette(); };
    state_.deciders.help = [this] { show_help_dialog(); };
    state_.deciders.export_path = [this](const std::string& suggested) { return ask_export_path(suggested); };
    state_.deciders.browse_dir = [this](const std::string& initial) {
        return ask_path(core::tr("gui.browse_title"), initial, PathKind::Folder);
    };
    state_.deciders.confirm_replace_origin = [this](const std::string& message) {
        // Güvenli varsayılan: Enter ve Esc vazgeçer.
        return ask_choice(
            ChoiceBox<bool>{core::tr("gui.replace_origin_title"),
                            message,
                            {{core::tr("gui.replace_origin"), true}, {core::tr("gui.cancel"), false}},
                            1,
                            1});
    };
    state_.deciders.info = [this](const std::string& message) {
        ask_choice(ChoiceBox<bool>{"noteapp", message, {{core::tr("gui.ok"), true}}, 0, 0});
    };
    state_.deciders.while_busy = [](const std::function<void()>& work) {
        // Arka uç imleci yalnız değişince kurar: önceki imleç elle geri konur, yoksa bekleme imleci takılı
        // kalırdı.
        const std::unique_ptr<SDL_Cursor, decltype(&SDL_DestroyCursor)> wait(
            SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_WAIT), &SDL_DestroyCursor);
        const struct Restore {
            SDL_Cursor* cursor;
            ~Restore() { SDL_SetCursor(cursor); }
        } restore{SDL_GetCursor()};
        SDL_SetCursor(wait.get());
        work();
    };
    state_.deciders.copy_text = [](const std::string& text) { return SDL_SetClipboardText(text.c_str()); };
    state_.deciders.paste_text = []() -> std::optional<std::string> {
        if (!SDL_HasClipboardText())
            return std::nullopt;
        const std::unique_ptr<char, decltype(&SDL_free)> text(SDL_GetClipboardText(), &SDL_free);
        if (!text || *text == '\0')  // SDL boş panoda da "" döner
            return std::nullopt;
        return std::string(text.get());
    };
}

void View::run_modal(const std::string& title, const std::function<bool()>& body) {
    const bool was_editing = editor_active_;
    // Diyalog akışını istisnaya karşı sayaçla korur: modal_depth_ takılırsa kısayollar ölü kalırdı.
    struct Depth {
        int& depth;
        explicit Depth(int& d) : depth(d) { ++depth; }
        ~Depth() { --depth; }
    } depth(modal_depth_);
    const std::string id = title + "###modal";
    frames_left_ = kFramesAfterEvent;
    // Açılış kareleri milisaniyeler içinde biter: olay gelmezse karartma yarım kalırdı (kModalDimWake).
    std::optional<std::chrono::steady_clock::time_point> dim_at =
        std::chrono::steady_clock::now() + kModalDimWake;
    const auto wait_ms = [&] {
        const auto now = std::chrono::steady_clock::now();
        return shorten_wait(shorten_wait(kIdleWaitMs, tooltip_at_, now), dim_at, now);
    };
    // idle_step'teki zamanlayıcılar: diyaloğun içindeki ipucu da gecikmesi dolunca açılır.
    const auto timer_due = [&] {
        const auto now = std::chrono::steady_clock::now();
        bool due = false;
        if (dim_at && now >= *dim_at) {
            dim_at.reset();
            due = true;
        }
        if (tooltip_due(now)) {
            frames_left_ = kFramesAfterEvent;
            due = true;
        }
        return due;
    };
    for (bool finished = false; !finished;) {
        if (on_modal_frame)  // test kancası tuşları kendisi üretir: beklemek yalnız testi yavaşlatır
            on_modal_frame();
        else if (frames_left_ > 0)
            --frames_left_;
        else if (SDL_WaitEventTimeout(nullptr, wait_ms()))
            frames_left_ = kFramesAfterEvent;
        // Olay yok: kare yalnız süresi dolan zamanlayıcıda ya da metin alanının kırpma imlecinde (idle_step).
        else if (!timer_due() && !caret_blinks())
            continue;
        render([&] {
            if (!ImGui::IsPopupOpen(id.c_str()))
                ImGui::OpenPopup(id.c_str());
            const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
            ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5F, 0.5F));
            // Kısa pencerede içerik taşarsa diyalog büyümez, kaydırma çubuğu çıkar.
            const float margin = 2.0F * ImGui::GetStyle().WindowPadding.x;
            const ImVec2 work_size = ImGui::GetMainViewport()->WorkSize;
            // Negatif üst sınırı ImGui "kısıt yok" sayar: çok küçük pencerede diyalog taşardı.
            ImGui::SetNextWindowSizeConstraints(
                ImVec2(0.0F, 0.0F),
                ImVec2(std::max(0.0F, work_size.x - margin), std::max(0.0F, work_size.y - margin)));
            if (ImGui::BeginPopupModal(id.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                finished = body();
                keep_window_in_viewport();
                const ImVec2 modal_pos = ImGui::GetWindowPos();
                const ImVec2 modal_size = ImGui::GetWindowSize();
                remember_rect("modal", modal_pos,
                              ImVec2(modal_pos.x + modal_size.x, modal_pos.y + modal_size.y));
                if (finished)
                    ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
        });
    }
    frames_left_ = kFramesAfterEvent;
    // Diyalog odağı aldı: kullanıcı yazmaya kaldığı yerden devam edebilsin.
    refocus_editor_ = was_editing;
}

std::optional<std::string> View::ask_folder_name(const std::string& title, const std::string& initial,
                                                 const std::string& field_label) {
    std::string name = initial;
    std::optional<std::string> result;
    bool first_frame = true;
    run_modal(title, [&]() -> bool {
        if (first_frame)
            ImGui::SetKeyboardFocusHere();
        first_frame = false;
        const std::string label = field_label.empty() ? core::tr("gui.folder_name") : field_label;
        ImGui::SetNextItemWidth(form_item_width(text_width(label), dialog_content_width()));
        bool submitted = ImGui::InputText("##folder_name", &name, ImGuiInputTextFlags_EnterReturnsTrue);
        remember_item_center("dialog_folder_name");
        ImGui::SameLine();
        ImGui::TextUnformatted(label.c_str());
        submitted |= button("dialog_ok", core::tr("gui.ok"));
        ImGui::SameLine();
        if (button("dialog_cancel", core::tr("gui.cancel")) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            return true;  // result boş: vazgeçti
        if (!submitted)
            return false;
        // Boş ad vazgeçmedir: hata gösterecek bir şey yok.
        const auto first = name.find_first_not_of(" \t");
        if (first != std::string::npos)
            result = name.substr(first, name.find_last_not_of(" \t") - first + 1);
        return true;
    });
    return result;
}

std::optional<std::string> View::ask_path(const std::string& title, const std::string& initial,
                                          PathKind kind) {
    std::string path = initial;
    std::optional<std::string> result;
    const auto native = std::make_shared<NativeDialogResult>();
    bool first_frame = true;
    run_modal(title, [&]() -> bool {
        {
            const std::scoped_lock lock(native->mutex);
            if (native->path)
                path = *std::exchange(native->path, std::nullopt);
        }
        if (first_frame)
            ImGui::SetKeyboardFocusHere();
        first_frame = false;
        const std::string browse_label = core::tr("gui.browse");
        ImGui::SetNextItemWidth(form_item_width(button_width(browse_label), dialog_content_width()));
        bool submitted = ImGui::InputText("##path", &path, ImGuiInputTextFlags_EnterReturnsTrue);
        remember_item_center("dialog_path");
        ImGui::SameLine();
        const bool browse_clicked = ImGui::Button(browse_label.c_str());
        remember_item_center("dialog_browse");
        if (browse_clicked) {
            auto userdata = std::make_unique<std::shared_ptr<NativeDialogResult>>(native);
            SDL_Window* window = platform_->window.get();
            // release: sahiplik SDL üzerinden native_dialog_done'a geçer, geri çağrı her durumda çalışır.
            if (kind == PathKind::Folder)
                SDL_ShowOpenFolderDialog(native_dialog_done, userdata.release(), window, path.c_str(), false);
            else
                SDL_ShowSaveFileDialog(native_dialog_done, userdata.release(), window, kPdfFilters, 1,
                                       path.c_str());
        }
        submitted |= button("dialog_ok", core::tr("gui.ok"));
        ImGui::SameLine();
        if (button("dialog_cancel", core::tr("gui.cancel")) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            return true;  // result boş: vazgeçti
        if (!submitted)
            return false;
        if (!path.empty())
            result = path;
        return true;
    });
    return result;
}

void View::import_pending() {
    const GuiState::ImportRequest request = state_.take_import_request();
    if (request != GuiState::ImportRequest::None) {
        // release: sahiplik SDL üzerinden import_dialog_done'a geçer, geri çağrı her durumda çalışır.
        auto userdata = std::make_unique<std::shared_ptr<ImportInbox>>(import_inbox_);
        SDL_Window* window = platform_->window.get();
        if (request == GuiState::ImportRequest::Folders)
            SDL_ShowOpenFolderDialog(import_dialog_done, userdata.release(), window, nullptr, true);
        else
            SDL_ShowOpenFileDialog(import_dialog_done, userdata.release(), window, nullptr, 0, nullptr, true);
    }
    std::vector<std::filesystem::path> paths = std::exchange(dropped_, {});
    std::string error;
    {
        const std::scoped_lock lock(import_inbox_->mutex);
        std::ranges::move(import_inbox_->paths, std::back_inserter(paths));
        import_inbox_->paths.clear();
        error = std::exchange(import_inbox_->error, {});
    }
    if (!error.empty()) {
        core::log_warn("event=import_dialog_failed detail=" + error);
        state_.import_dialog_failed(error);
        frames_left_ = kFramesAfterEvent;
    }
    if (paths.empty())
        return;
    state_.import_files(paths);
    frames_left_ = kFramesAfterEvent;
}

std::optional<std::string> View::ask_export_path(const std::string& suggested) {
    const char* documents = SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS);  // SDL sonda ayırıcıyı kendi koyar
    const std::string initial = documents != nullptr ? std::string(documents) + suggested : suggested;
    auto chosen = ask_path(core::tr("gui.export_dialog_title"), initial, PathKind::SaveFile);
    if (!chosen)
        return chosen;
    // Elle yazılan yol da var olan dosyayı sormadan ezmesin (yerel diyalog kendisi de sorar). symlink_status:
    // kırık bağ da var olan addır (TUI ile aynı kural).
    std::error_code ignored;
    if (!std::filesystem::exists(std::filesystem::symlink_status(
            std::filesystem::path(std::u8string(chosen->begin(), chosen->end())), ignored)))
        return chosen;
    const bool replace =
        ask_choice(ChoiceBox<bool>{core::tr("gui.export_dialog_title"),
                                   core::tr("gui.confirm_overwrite", {{"path", *chosen}}),
                                   {{core::tr("gui.overwrite"), true}, {core::tr("gui.cancel"), false}},
                                   1,
                                   1});
    return replace ? chosen : std::nullopt;
}

template <typename Choice> Choice View::ask_choice(const ChoiceBox<Choice>& box) {
    std::size_t chosen = box.escape_button;
    run_modal(box.title, [&]() -> bool {
        const float content_width = dialog_content_width();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + content_width);
        ImGui::TextUnformatted(box.message.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        const float row_start_x = ImGui::GetCursorScreenPos().x;
        bool answered = false;
        for (std::size_t i = 0; i < box.buttons.size(); ++i) {
            if (i > 0)
                flow_after_previous(button_width(box.buttons[i].first), row_start_x, content_width);
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Button(box.buttons[i].first.c_str())) {
                chosen = i;
                answered = true;
            }
            remember_item_center("choice:" + std::to_string(i));
            // Enter görünür gezinme imlecindeki düğmeyi etkinleştirir: imleç ilk karede varsayılan düğmeye
            // konur.
            if (i == box.default_button && ImGui::IsWindowAppearing()) {
                ImGui::SetItemDefaultFocus();
                ImGui::SetNavCursorVisible(true);
            }
            ImGui::PopID();
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            chosen = box.escape_button;
            answered = true;
        }
        return answered;
    });
    return box.buttons[chosen].second;
}

std::optional<SaveDetails> View::ask_save_details(const SaveDialogRequest& request) {
    SaveForm form(request);
    std::optional<SaveDetails> result;
    bool first_frame = true;
    const std::string title = core::tr(
        request.mode == SaveDialogRequest::Mode::Move ? "gui.move_dialog_title" : "gui.save_dialog_title");
    run_modal(title, [&]() -> bool {
        const float trailing_width =
            std::max({text_width(core::tr("gui.field_name")), text_width(core::tr("gui.field_folder")),
                      text_width(core::tr("gui.field_type"))});
        ImGui::PushItemWidth(form_item_width(trailing_width, dialog_content_width()));
        bool submitted = false;

        ImGui::BeginDisabled(request.folder_only);
        if (first_frame && !request.folder_only)
            ImGui::SetKeyboardFocusHere();
        submitted |= ImGui::InputText("##name", &form.fields.name, ImGuiInputTextFlags_EnterReturnsTrue);
        remember_item_center("save_name");
        ImGui::SameLine();
        ImGui::TextUnformatted(core::tr("gui.field_name").c_str());
        ImGui::EndDisabled();

        if (first_frame && request.folder_only)
            ImGui::SetKeyboardFocusHere();
        const std::string root_hint = core::tr("gui.folder_root");
        submitted |= combo_field("folder", core::tr("gui.field_folder"), form.fields.folder, request.folders,
                                 true, root_hint.c_str());
        remember_item_center("save_folder");

        ImGui::BeginDisabled(request.folder_only);
        submitted |= combo_field("type", core::tr("gui.field_type"), form.fields.type, request.types, false);
        remember_item_center("save_type");
        ImGui::EndDisabled();
        ImGui::PopItemWidth();

        if (!request.hint.empty())
            ImGui::TextUnformatted(request.hint.c_str());
        draw_error(form.error);
        first_frame = false;

        submitted |= button("dialog_ok", core::tr("gui.ok"));
        ImGui::SameLine();
        if (button("dialog_cancel", core::tr("gui.cancel")) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            return true;  // result boş kalır: vazgeçti
        // Tek satırlı alanda Enter EnterReturnsTrue ile gelir; alan etkin değilken de Enter kabul sayılır.
        submitted |= !ImGui::IsAnyItemActive() && ImGui::IsKeyPressed(ImGuiKey_Enter, false);
        if (submitted && form.try_accept()) {
            result = form.details();
            return true;
        }
        return false;
    });
    return result;
}

}  // namespace noteapp::gui
