add_library(noteapp_sync
    git_runtime.cpp
    lock.cpp
    sync_service.cpp)
target_include_directories(noteapp_sync PUBLIC ${PROJECT_SOURCE_DIR}/src)
# libgit2'nin hedef adı libgit2package'dir (libgit2 adı iç OBJECT kütüphanesine ait).
target_link_libraries(noteapp_sync PUBLIC noteapp_core libgit2package keychain)
# toml++ yalnız durum dosyası için; core ile aynı istisnasız mod.
target_link_libraries(noteapp_sync PRIVATE tomlplusplus::tomlplusplus)
target_compile_definitions(noteapp_sync PRIVATE TOML_EXCEPTIONS=0)
noteapp_target_defaults(noteapp_sync)
