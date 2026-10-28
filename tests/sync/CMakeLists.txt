noteapp_add_test(sync
    libgit2_spike_test.cpp
    keychain_spike_test.cpp
    sync_basic_test.cpp
    sync_conflict_test.cpp
    enable_test.cpp
    auth_test.cpp
    recovery_test.cpp
    auto_sync_test.cpp)

# AutoSync testleri `note` yerine fake_editor'ı (uyuyan/çıkan çocuk) ve uçtan uca gerçek `note`'u başlatır.
add_dependencies(sync_tests note fake_editor)
target_compile_definitions(sync_tests PRIVATE NOTEAPP_NOTE_EXE="$<TARGET_FILE:note>"
                                              NOTEAPP_FAKE_EDITOR="$<TARGET_FILE:fake_editor>")

# SSH ön kontrolü testleri PATH'i sahte ssh-keygen'in dizinine çevirir: ikilinin adı gerçeğininki olmalı ve ayrı
# dizinde durur. MinGW'de statik: PATH yalnız bu dizine indirildiğinde de çalışma zamanı DLL'i aranmasın.
add_executable(fake_ssh_keygen fake_ssh_keygen.cpp)
set_target_properties(fake_ssh_keygen PROPERTIES OUTPUT_NAME ssh-keygen
                                                 RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/fake_ssh)
noteapp_target_defaults(fake_ssh_keygen)
if(MINGW)
    target_link_options(fake_ssh_keygen PRIVATE -static)
endif()
add_dependencies(sync_tests fake_ssh_keygen)
target_compile_definitions(sync_tests PRIVATE NOTEAPP_FAKE_SSH_DIR="$<TARGET_FILE_DIR:fake_ssh_keygen>")
