# Görünüm kütüphanede: ekranı metne render eden testler (SPEC-tui kabul 1) terminal açmadan aynı kodu çağırır.
add_library(noteapp_tui
    view.cpp
    frame_diff.cpp
    markdown_view.cpp
    styled_view.cpp
    data_view.cpp
    commands.cpp
    inspector_keys.cpp
    link_panel.cpp
    map_view.cpp
    list_actions.cpp
    input_view.cpp
    editor_tabs.cpp
    editor_view.cpp
    termination.cpp
    pane_layout.cpp
    mouse.cpp
    modals.cpp
    settings.cpp
    sync_indicator.cpp
    save_dialog.cpp
    tui_state.cpp)
target_include_directories(noteapp_tui PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(noteapp_tui
    PUBLIC noteapp_edit noteapp_app noteapp_core noteapp_detect noteapp_search noteapp_sync noteapp_pdf ftxui::dom
    PRIVATE md4c)
noteapp_target_defaults(noteapp_tui)

add_executable(notes
    main.cpp)
target_link_libraries(notes PRIVATE noteapp_tui ftxui::component)
noteapp_target_defaults(notes)

install(TARGETS notes RUNTIME DESTINATION bin)
