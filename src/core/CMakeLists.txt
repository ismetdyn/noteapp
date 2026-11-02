add_library(noteapp_core
    atomic_file.cpp
    i18n.cpp
    log.cpp
    config.cpp
    console.cpp
    editor.cpp
    file_icons.cpp
    file_stat.cpp
    migration.cpp
    note_path.cpp
    note_snapshot.cpp
    note_store.cpp
    note_sync.cpp
    note_write.cpp
    paths.cpp
    process.cpp
    text.cpp
    version.cpp)
target_include_directories(noteapp_core PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(noteapp_core PRIVATE NOTEAPP_VERSION="${PROJECT_VERSION}")
# toml++ yalnız config.cpp'de kullanılır; istisnasız mod: ayrıştırma hatası parse_result ile döner.
target_link_libraries(noteapp_core PRIVATE tomlplusplus::tomlplusplus)
# Notlar graph.db'dedir; başlıkta yalnız note_store_impl.hpp ve note_sync.hpp motor türü taşır, SQLite görünmez.
target_link_libraries(noteapp_core PUBLIC noteapp_graph)
target_compile_definitions(noteapp_core PRIVATE TOML_EXCEPTIONS=0)
noteapp_embed_resources(noteapp_core defaults.toml i18n/en.toml i18n/tr.toml)
noteapp_target_defaults(noteapp_core)
