# Uygulama katmanı: TUI ve GUI aynı özelliğin iki ince arayüzüdür (plan-parity). Arayüzden bağımsız özellik
# mantığı (karar kuralları, durum geçişleri, ileti metni üretimi, depo işlemlerinin sırası) burada tek kez
# yazılır, iki arayüz onu çağırır. Çizim, tuş/fare eşlemesi ve widget durumu buraya girmez: FTXUI, ImGui ve SDL
# bağımlılığı yoktur. İki arayüzde de gereken bir yardımcı kopyalanmaz, buraya taşınır.
add_library(noteapp_app
    commands.cpp
    data_preview.cpp
    document.cpp
    graph_layout.cpp
    import_files.cpp
    ui_args.cpp
    item_ops.cpp
    label_fold.cpp
    listing.cpp
    map_relations.cpp
    note_map.cpp
    notices.cpp
    settings.cpp
    stray_notice.cpp
    sync_status_worker.cpp
    tabs.cpp
    trash_ops.cpp)
target_include_directories(noteapp_app PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(noteapp_app PUBLIC noteapp_core noteapp_search noteapp_sync noteapp_detect
    PRIVATE tomlplusplus::tomlplusplus)
# Sekmelerin `[[tabs]]` bloğu (tabs.cpp): ayrıştırma hatası parse_result ile döner (config.cpp ile aynı mod).
target_compile_definitions(noteapp_app PRIVATE TOML_EXCEPTIONS=0)
noteapp_target_defaults(noteapp_app)
