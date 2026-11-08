add_library(noteapp_gui
    browser_rows.cpp
    gui_state.cpp
    gui_state_commands.cpp
    gui_state_browser.cpp
    gui_state_folders.cpp
    gui_state_inspector.cpp
    gui_state_map.cpp
    gui_state_marks.cpp
    gui_state_save.cpp
    gui_state_sync.cpp
    gui_state_tabs.cpp
    gui_state_trash.cpp
    icon_font.cpp
    markdown_preview.cpp
    note_editor.cpp
    note_list_model.cpp
    save_dialog.cpp
    settings_page.cpp
    theme.cpp
    view.cpp
    view_browser.cpp
    view_dialogs.cpp
    view_editor.cpp
    view_map.cpp
    view_note_editor.cpp
    view_palette.cpp
    view_settings.cpp
    view_tabs.cpp
    view_toolbar.cpp
    view_widgets.cpp)
# noteapp_embed_fonts pdf::embedded_font'u üretir ve noteapp_pdf'te zaten tanımlıdır; ikon fontu kendi işlevini alır ki
# `note` (noteapp_pdf'e bağlı) bu veriyi taşımasın.
noteapp_embed_files(noteapp_gui gui/icon_font.hpp noteapp::gui embedded_icon_font fonts/NoteappIcons.ttf)
# Ayarlar → Görünüm'deki seçilebilir aileler (OFL-1.1, scripts/fetch-ui-fonts.sh); DejaVu PDF ile ortak kalır.
noteapp_embed_files(noteapp_gui gui/icon_font.hpp noteapp::gui embedded_ui_font
    fonts/Inter-Regular.ttf
    fonts/Inter-Bold.ttf
    fonts/Inter-Italic.ttf
    fonts/Inter-BoldItalic.ttf
    fonts/JetBrainsMono-Regular.ttf)
noteapp_embed_files(noteapp_gui gui/icon_font.hpp noteapp::gui embedded_app_icon
    icons/hicolor/64x64/apps/noteapp.png
    icons/hicolor/256x256/apps/noteapp.png)
target_include_directories(noteapp_gui PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(noteapp_gui
    PUBLIC noteapp_app noteapp_edit noteapp_core noteapp_search noteapp_sync imgui
    PRIVATE noteapp_detect noteapp_pdf)
noteapp_target_defaults(noteapp_gui)

add_executable(notes-gui WIN32 main.cpp)
target_link_libraries(notes-gui PRIVATE noteapp_gui)
noteapp_target_defaults(notes-gui)

install(TARGETS notes-gui RUNTIME DESTINATION bin)
