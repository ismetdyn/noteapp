add_library(noteapp_sync
    git_runtime.cpp)
target_include_directories(noteapp_sync PUBLIC ${PROJECT_SOURCE_DIR}/src)
# libgit2'nin hedef adı libgit2package'dir (libgit2 adı iç OBJECT kütüphanesine ait).
target_link_libraries(noteapp_sync PUBLIC libgit2package keychain)
noteapp_target_defaults(noteapp_sync)
