add_library(noteapp_sync
    auth.cpp
    auto_sync.cpp
    git_runtime.cpp
    lock.cpp
    note_status.cpp
    sync_service.cpp
    text_merge.cpp)
target_include_directories(noteapp_sync PUBLIC ${PROJECT_SOURCE_DIR}/src)
# libgit2'nin hedef adı libgit2package'dir (libgit2 adı iç OBJECT kütüphanesine ait).
target_link_libraries(noteapp_sync PUBLIC noteapp_core noteapp_graph libgit2package)
if(WIN32)
    target_link_libraries(noteapp_sync PRIVATE advapi32)  # CredWriteW / CredReadW / CredDeleteW
else()
    target_link_libraries(noteapp_sync PRIVATE keychain)
endif()
# toml++ yalnız durum dosyası için; core ile aynı istisnasız mod.
target_link_libraries(noteapp_sync PRIVATE tomlplusplus::tomlplusplus)
target_compile_definitions(noteapp_sync PRIVATE TOML_EXCEPTIONS=0)
noteapp_target_defaults(noteapp_sync)
