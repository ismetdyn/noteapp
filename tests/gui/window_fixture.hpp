#pragma once

#include "core/config.hpp"
#include "core/i18n.hpp"
#include "core/paths.hpp"
#include "gui/gui_state.hpp"
#include "gui/view.hpp"
#include "support/note_seed.hpp"
#include "support/test_support.hpp"

#include <chrono>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <imgui.h>
#include <imgui_internal.h>
#include <memory>
#include <string>
#include <string_view>

namespace gui_test {

namespace fs = std::filesystem;
using noteapp::gui::GuiState;
using noteapp::gui::View;

// Geçici NOTEAPP_HOME + notes_dir; testler gerçek kullanıcı dizinine dokunmaz. SDL sürücüleri CTest
// ortamından gelir (tests/gui/CMakeLists.txt): ekran ve Xvfb gerekmez.
struct WindowFixture {
    noteapp::test::TempDir home;
    noteapp::test::ScopedEnv home_env{"NOTEAPP_HOME"};
    noteapp::core::Config config;

    WindowFixture() {
        home_env.set(noteapp::test::utf8(home.path()));
        noteapp::core::init_i18n("en");
        config = noteapp::core::load_config().config;
        config.notes_dir = home.path() / "notes";
        fs::create_directories(config.notes_dir);
    }

    fs::path full_path(const std::string& relative) const {
        return config.notes_dir / fs::path(std::u8string(relative.begin(), relative.end()));
    }

    static constexpr std::string_view kTrashPrefix = ".trash/";
    fs::path store_dir() const { return noteapp::core::data_dir(); }

    // age_minutes büyük = eski; sıralama güncellenme zamanına bağlı olduğundan açıkça ayarlanır.
    // ".trash/" öneki: çöpteki not (G4'te dizin değil Note.deleted_at).
    void add_note(const std::string& relative, const std::string& content, int age_minutes = 0) const {
        const auto when = std::chrono::system_clock::now() - std::chrono::minutes(age_minutes);
        if (relative.starts_with(kTrashPrefix)) {
            noteapp::core::NoteStore store(store_dir());
            const noteapp::core::ImportedNote item{
                noteapp::core::NotePath::parse(relative.substr(kTrashPrefix.size())).value(), content, when,
                when, when};
            REQUIRE(store.import_notes(std::span(&item, 1)).has_value());
            return;
        }
        noteapp::test::upsert_note(store_dir(), relative, content, when);
    }

    bool note_exists(const std::string& relative) const {
        return relative.starts_with(kTrashPrefix)
                   ? noteapp::test::note_in_trash(store_dir(), relative.substr(kTrashPrefix.size()))
                   : noteapp::test::note_exists(store_dir(), relative);
    }
    std::string read_note(const std::string& relative) const {
        if (!relative.starts_with(kTrashPrefix))
            return noteapp::test::note_body(store_dir(), relative);
        const auto content =
            noteapp::core::NoteStore(store_dir())
                .read_trash(noteapp::core::NotePath::parse(relative.substr(kTrashPrefix.size())).value());
        return content ? content->text : "<missing>";
    }
    void remove_note(const std::string& relative) const { noteapp::test::erase_note(store_dir(), relative); }

    std::unique_ptr<GuiState> make_state() const { return std::make_unique<GuiState>(config); }
};

// Gerçek çizim katmanını başsız koşan testler için.
inline std::unique_ptr<View> make_view(GuiState& state) {
    auto view = View::create(state);
    if (!view)
        FAIL(view.error());
    (*view)->draw_frame();  // ilk kare: pencere ve odak yerleşir
    (*view)->draw_frame();
    return std::move(*view);
}

// Odak isteği birkaç karede oturur: istek → etkinleşme → ImGui'nin kısayol rotası (bir sonraki karede
// geçerli). Daha erken gelen Enter yok sayılır; insan elinde bu pencere birkaç karedir, testte tuş hemen
// gelir.
inline void settle(View& view) {
    for (int frame = 0; frame < 4; ++frame)
        view.draw_frame();
}

// Tuş ayrı karelerde basılıp bırakılır: ImGui olay kuyruğu aynı karede bas-bırak'ı birleştirmez.
inline void press(View& view, ImGuiKeyChord chord) {
    ImGuiIO& io = ImGui::GetIO();
    const auto key = static_cast<ImGuiKey>(chord & ~ImGuiMod_Mask_);
    const bool ctrl = (chord & ImGuiMod_Ctrl) != 0;
    const bool shift = (chord & ImGuiMod_Shift) != 0;
    const bool alt = (chord & ImGuiMod_Alt) != 0;
    if (ctrl)
        io.AddKeyEvent(ImGuiMod_Ctrl, true);
    if (shift)
        io.AddKeyEvent(ImGuiMod_Shift, true);
    if (alt)
        io.AddKeyEvent(ImGuiMod_Alt, true);
    io.AddKeyEvent(key, true);
    view.draw_frame();
    ImGuiIO& after = ImGui::GetIO();
    after.AddKeyEvent(key, false);
    if (alt)
        after.AddKeyEvent(ImGuiMod_Alt, false);
    if (shift)
        after.AddKeyEvent(ImGuiMod_Shift, false);
    if (ctrl)
        after.AddKeyEvent(ImGuiMod_Ctrl, false);
    view.draw_frame();
    view.draw_frame();  // odak isteği bir sonraki karede uygulanır
}

// Diyalog yanıtı. Bas ve bırak birlikte kuyruğa girer, ImGui ikisini ardışık karelere dağıtır: basış diyaloğu
// kapatsa da bırakma kaybolmaz. Kaybolsaydı tuş basılı kalır, KeyRepeatDelay (gerçek zaman) dolunca sonraki
// diyaloğu da yanıtlardı.
inline void tap_key(ImGuiKey key) {
    ImGuiIO& io = ImGui::GetIO();
    io.AddKeyEvent(key, true);
    io.AddKeyEvent(key, false);
}

// Gerçek fare tıklaması; ad View::button_center kimliğidir.
inline void click_button(View& view, const std::string& name) {
    const auto [x, y] = view.button_center(name);
    ImGui::GetIO().AddMousePosEvent(x, y);
    view.draw_frame();
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    view.draw_frame();
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    view.draw_frame();
    view.draw_frame();
}

// Nesne yaşarken çizilen karelerin metni, ImGui'nin kendi günlük kanalından. Parola alanları bu kanala
// yazılmaz; böylece testler çizim katmanına kanca eklemeden etiketi ve maskeyi görür. View'dan sonra
// tanımlanmalı.
class RenderedText {
public:
    RenderedText() {
        ImGuiContextHook begin;
        begin.Type = ImGuiContextHookType_NewFramePost;
        begin.Callback = [](ImGuiContext*, ImGuiContextHook*) { ImGui::LogToBuffer(); };
        begin_hook_ = ImGui::AddContextHook(ImGui::GetCurrentContext(), &begin);
        ImGuiContextHook end;
        end.Type = ImGuiContextHookType_EndFramePre;
        end.UserData = this;
        end.Callback = [](ImGuiContext* context, ImGuiContextHook* hook) {
            static_cast<RenderedText*>(hook->UserData)->text_ += context->LogBuffer.c_str();
            ImGui::LogFinish();
        };
        end_hook_ = ImGui::AddContextHook(ImGui::GetCurrentContext(), &end);
    }
    ~RenderedText() {
        ImGui::RemoveContextHook(ImGui::GetCurrentContext(), begin_hook_);
        ImGui::RemoveContextHook(ImGui::GetCurrentContext(), end_hook_);
    }
    RenderedText(const RenderedText&) = delete;
    RenderedText& operator=(const RenderedText&) = delete;

    bool contains(std::string_view text) const { return text_.find(text) != std::string::npos; }

private:
    std::string text_;
    ImGuiID begin_hook_ = 0;
    ImGuiID end_hook_ = 0;
};

inline void type_text(View& view, const char* utf8) {
    ImGui::GetIO().AddInputCharactersUTF8(utf8);
    view.draw_frame();
    view.draw_frame();
}

}  // namespace gui_test
