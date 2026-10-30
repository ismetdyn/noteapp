include(FetchContent)

# Tüm üçüncü parti bağımlılıklar burada pin'lenir (SPEC.md §3); başka hiçbir dosya FetchContent çağırmaz.
# Qt burada yok: sistemden gelir (find_package), yalnız NOTEAPP_BUILD_GUI açıkken src/gui içinde aranır.

FetchContent_Declare(tomlplusplus
    URL https://github.com/marzer/tomlplusplus/archive/refs/tags/v3.4.0.tar.gz
    URL_HASH SHA256=8517f65938a4faae9ccf8ebb36631a38c1cadfb5efa85d9a72e15b9e97d25155)
FetchContent_MakeAvailable(tomlplusplus)

# Yalnız kütüphane hedefleri gerekir; örnek/doküman/test/install hedefleri derleme süresini ve kurulum yüzeyini
# büyütür. SYSTEM: FTXUI başlıklarındaki uyarılar bizim -Werror'umuzu kırmasın.
set(FTXUI_BUILD_EXAMPLES OFF)
set(FTXUI_BUILD_DOCS OFF)
set(FTXUI_BUILD_TESTS OFF)
set(FTXUI_ENABLE_INSTALL OFF)
FetchContent_Declare(ftxui
    URL https://github.com/ArthurSonzogni/FTXUI/archive/refs/tags/v7.0.3.tar.gz
    URL_HASH SHA256=e7c62ffe19009759821b4f0f8df7f2a6fb83784c3a9f1477d81f56d3ee723c88
    SYSTEM)
FetchContent_MakeAvailable(ftxui)

# libgit2 ve keychain ayarları cache değişkeni olmalı: libgit2 cmake_minimum_required(3.5) ile CMP0077'yi OLD
# bıraktığından option() normal değişkeni ezer. BUILD_SHARED_LIBS/BUILD_TESTS globaldir; keychain'in testlerini de
# kapatır ve libgit2'nin varsayılan ON'u bizim kütüphanelerimizi paylaşımlıya çevirmesin diye OFF tutulur.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "")
set(BUILD_TESTS OFF CACHE BOOL "")
set(BUILD_CLI OFF CACHE BOOL "")
set(BUILD_EXAMPLES OFF CACHE BOOL "")
# SSH: sistemdeki OpenSSH çağrılır (anahtar/agent/~/.ssh/config ondan; libssh2 bağımlılığı yok, SPEC.md §12/2).
set(USE_SSH exec CACHE STRING "")
# HTTPS: her platformun yerel TLS yığını; Windows'ta ek kütüphane gerekmez.
if(WIN32)
    set(USE_HTTPS WinHTTP CACHE STRING "")
elseif(APPLE)
    set(USE_HTTPS SecureTransport CACHE STRING "")
else()
    set(USE_HTTPS OpenSSL CACHE STRING "")
endif()
# Sistem kütüphanesi aranmasın: Windows'ta yoklar, Linux'ta sürüm farkı üretir.
set(USE_BUNDLED_ZLIB ON CACHE BOOL "")
set(REGEX_BACKEND builtin CACHE STRING "")
set(USE_HTTP_PARSER builtin CACHE STRING "")
FetchContent_Declare(libgit2
    URL https://github.com/libgit2/libgit2/archive/refs/tags/v1.9.7.tar.gz
    URL_HASH SHA256=1a4fbe7589e814777ae76b64734ad80f4ecad22cd33a22682a2aaea4ae5375e7)
FetchContent_MakeAvailable(libgit2)
# libgit2 başlık yolunu yalnız kurulum arayüzünde yayınlar; alt proje kullanımı için build arayüzüne eklenir.
# SYSTEM: libgit2 başlıklarındaki uyarılar bizim -Werror'umuzu kırmasın.
target_include_directories(libgit2package SYSTEM INTERFACE $<BUILD_INTERFACE:${libgit2_SOURCE_DIR}/include>)

# Windows'ta token doğrudan Credential Manager'a yazılır (src/sync/auth.cpp); kütüphane CRED_PERSIST_ENTERPRISE
# kullandığından yalnız Linux/macOS'ta gerekir.
if(NOT WIN32)
    FetchContent_Declare(keychain
        URL https://github.com/hrantzsch/keychain/archive/refs/tags/v1.3.1.tar.gz
        URL_HASH SHA256=9a9fc5aaa94d03180f1c3735f93d1ebcf9d2925fc915306b8476abc17a3906a9)
    FetchContent_MakeAvailable(keychain)
endif()

if(NOTEAPP_BUILD_TESTS)
    FetchContent_Declare(doctest
        URL https://github.com/doctest/doctest/archive/refs/tags/v2.5.3.tar.gz
        URL_HASH SHA256=174ebc4e769928959614789c5b4e9c3d0a0f81a62bb608756b127bfebfb21331)
    FetchContent_MakeAvailable(doctest)
endif()
