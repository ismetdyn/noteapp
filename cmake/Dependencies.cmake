include(FetchContent)

# Tüm üçüncü parti bağımlılıklar burada pin'lenir (SPEC.md §3); başka hiçbir dosya FetchContent çağırmaz.
# EXCLUDE_FROM_ALL: bağımlılıklar statik bağlanır; `cmake --install` başlıklarını ve .a dosyalarını kurmasın.

FetchContent_Declare(tomlplusplus
    URL https://github.com/marzer/tomlplusplus/archive/refs/tags/v3.4.0.tar.gz
    URL_HASH SHA256=8517f65938a4faae9ccf8ebb36631a38c1cadfb5efa85d9a72e15b9e97d25155
    EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(tomlplusplus)

# Yalnız kütüphane hedefleri gerekir; örnek/doküman/test/install hedefleri derleme süresini ve kurulum yüzeyini
# büyütür. SYSTEM: FTXUI başlıklarındaki uyarılar bizim -Werror'umuzu kırmasın.
set(FTXUI_BUILD_EXAMPLES OFF)
set(FTXUI_BUILD_DOCS OFF)
set(FTXUI_BUILD_TESTS OFF)
set(FTXUI_ENABLE_INSTALL OFF)
# Yama: FTXUI her karede tüm ekranı yazar, çıktı kancası ve bracketed paste yoktur; gerekçe yama dosyasının başında.
find_package(Patch REQUIRED)
set(ftxui_patch ${CMAKE_CURRENT_LIST_DIR}/patches/ftxui-7.0.3-frame-writer.patch)
# FetchContent yama dosyası değişince kaynağı yeniden yamamaz (komut aynıysa eski kaynak kalır, komut
# değişirse yamalı ağaca ikinci kez uygular ve düşer): yamanın özeti değiştiyse kaynak önbellekteki arşivden
# yeniden açılıp yamalanır. Arşiv ve URL_HASH kalır: ağ gerekmez (indirme adımı özeti tutan dosyayı atlar).
# FULLY_DISCONNECTED'da FetchContent hiçbir adımı koşmaz: kaynak silinmez, açık hatayla durulur. Kullanıcının
# verdiği kaynak dizinine (FETCHCONTENT_SOURCE_DIR_FTXUI) dokunulmaz.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${ftxui_patch})
file(SHA256 ${ftxui_patch} ftxui_patch_hash)
set(ftxui_patch_stamp ${FETCHCONTENT_BASE_DIR}/ftxui-patch.sha256)
set(ftxui_applied_hash "")
if(EXISTS ${ftxui_patch_stamp})
    file(READ ${ftxui_patch_stamp} ftxui_applied_hash)
endif()
if(NOT FETCHCONTENT_SOURCE_DIR_FTXUI AND EXISTS ${FETCHCONTENT_BASE_DIR}/ftxui-src
   AND NOT ftxui_applied_hash STREQUAL ftxui_patch_hash)
    if(FETCHCONTENT_FULLY_DISCONNECTED)
        message(FATAL_ERROR "The FTXUI patch stamp is missing or changed, but FETCHCONTENT_FULLY_DISCONNECTED "
                            "is ON, so the sources in ${FETCHCONTENT_BASE_DIR}/ftxui-src may keep an old patch. "
                            "Configure once "
                            "with -DFETCHCONTENT_FULLY_DISCONNECTED=OFF (the cached archive is reused, no "
                            "network needed) or remove ${FETCHCONTENT_BASE_DIR}/ftxui-*.")
    endif()
    message(STATUS "FTXUI patch changed: unpacking and patching FTXUI again")
    file(REMOVE_RECURSE ${FETCHCONTENT_BASE_DIR}/ftxui-src ${FETCHCONTENT_BASE_DIR}/ftxui-build
         ${FETCHCONTENT_BASE_DIR}/ftxui-subbuild/ftxui-populate-prefix/src/ftxui-populate-stamp)
endif()
FetchContent_Declare(ftxui
    URL https://github.com/ArthurSonzogni/FTXUI/archive/refs/tags/v7.0.3.tar.gz
    URL_HASH SHA256=e7c62ffe19009759821b4f0f8df7f2a6fb83784c3a9f1477d81f56d3ee723c88
    PATCH_COMMAND ${Patch_EXECUTABLE} -p1 --binary -i ${ftxui_patch}
    SYSTEM)
FetchContent_MakeAvailable(ftxui)
file(WRITE ${ftxui_patch_stamp} ${ftxui_patch_hash})

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
    URL_HASH SHA256=1a4fbe7589e814777ae76b64734ad80f4ecad22cd33a22682a2aaea4ae5375e7
    EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(libgit2)
# libgit2 başlık yolunu yalnız kurulum arayüzünde yayınlar; alt proje kullanımı için build arayüzüne eklenir.
# SYSTEM: libgit2 başlıklarındaki uyarılar bizim -Werror'umuzu kırmasın.
target_include_directories(libgit2package SYSTEM INTERFACE $<BUILD_INTERFACE:${libgit2_SOURCE_DIR}/include>)

# Windows'ta token doğrudan Credential Manager'a yazılır (src/sync/auth.cpp); kütüphane CRED_PERSIST_ENTERPRISE
# kullandığından yalnız Linux/macOS'ta gerekir.
if(NOT WIN32)
    FetchContent_Declare(keychain
        URL https://github.com/hrantzsch/keychain/archive/refs/tags/v1.3.1.tar.gz
        URL_HASH SHA256=9a9fc5aaa94d03180f1c3735f93d1ebcf9d2925fc915306b8476abc17a3906a9
        EXCLUDE_FROM_ALL)
    FetchContent_MakeAvailable(keychain)
endif()

if(NOTEAPP_BUILD_TESTS)
    FetchContent_Declare(doctest
        URL https://github.com/doctest/doctest/archive/refs/tags/v2.5.3.tar.gz
        URL_HASH SHA256=174ebc4e769928959614789c5b4e9c3d0a0f81a62bb608756b127bfebfb21331
        EXCLUDE_FROM_ALL)
    FetchContent_MakeAvailable(doctest)
endif()

# PDF (`noteapp_pdf`): GUI'siz derlemede de gerekir (`note export --pdf`). EXCLUDE_FROM_ALL: yalnız bağlanan hedefler
# derlenir, kurulum kuralları `cmake --install`'a sızmaz.
# zlib/PNG kapalı: find_package sistem zlib'ini bulsaydı libgit2'nin gömülü zlib'iyle aynı sembolleri farklı sürümden
# taşırdı ve Windows'ta bulunmaz, derleme makineye göre değişirdi. Bedeli: PDF akışları sıkıştırılmaz. Görseller
# PDF'e girmediğinden (SPEC-gui T36) PNG gerekmez.
# libgit2 CMAKE_C_STANDARD=90 ve eklentisiz C'yi cache'e yazar; libharu `//` yorumları ve SDL3 C99 ister. Normal değişken
# cache'i yalnız bundan sonraki alt projelerde gölgeler, libgit2'ye dokunmaz.
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_DISABLE_FIND_PACKAGE_ZLIB ON)
set(CMAKE_DISABLE_FIND_PACKAGE_PNG ON)
FetchContent_Declare(libharu
    URL https://github.com/libharu/libharu/archive/refs/tags/v2.4.6.tar.gz
    URL_HASH SHA256=ec8f327520d1d354ce58b5d2af75b64f380cddc522437c169463b39760921348
    EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(libharu)
unset(CMAKE_DISABLE_FIND_PACKAGE_ZLIB)
unset(CMAKE_DISABLE_FIND_PACKAGE_PNG)
# libharu başlık yolunu yalnız kendi dizininde tanımlar; hpdf_config.h üretildiği için build dizini de gerekir.
target_include_directories(hpdf SYSTEM INTERFACE
    $<BUILD_INTERFACE:${libharu_SOURCE_DIR}/include> $<BUILD_INTERFACE:${libharu_BINARY_DIR}/include>)

# md4c-html yalnız testte: markdown önizleme uygunluk koşumunun başvuru çıktısıdır (tests/tui/markdown_conformance_test).
# EXCLUDE_FROM_ALL: bağlanmayan yapıda derlenmez.
set(BUILD_HTML_LIBRARY ${NOTEAPP_BUILD_TESTS})
set(BUILD_MD2HTML_EXECUTABLE OFF)
FetchContent_Declare(md4c
    URL https://github.com/mity/md4c/archive/refs/tags/v0.6.0.tar.gz
    URL_HASH SHA256=4d151298125a81da3b2efa2e0eed8bdb7a9318569804e4fa4d7a2375ab83ef70
    EXCLUDE_FROM_ALL
    SYSTEM)
FetchContent_MakeAvailable(md4c)
# HTML5 adlı varlık tablosu (md4c'nin entity.c'si, ~2100 ad): core::decode_html_entity bunu arar. Yeni bağımlılık
# değil, aynı sabit arşivin dosyası; md4c-html'in tamamı gerekmez.
add_library(md4c_entity STATIC ${md4c_SOURCE_DIR}/src/entity.c)
target_include_directories(md4c_entity SYSTEM PUBLIC ${md4c_SOURCE_DIR}/src)
target_compile_options(md4c_entity PRIVATE $<IF:$<C_COMPILER_ID:MSVC>,/w,-w>)

# Graph deposu (`noteapp_graph`). Amalgamation'ın CMake'i yoktur; sqlite.org yalnız SHA3-256 yayımlar (92281c86…28e30),
# arşiv buna karşı doğrulanıp SHA256'sı alındı. Bayrakların gerekçesi: tasks/graph-design.md "Bağlantı".
# SQLITE_THREADSAFE=2: her thread kendi bağlantısını açar, bağlantı mutex'i gereksizdir.
# SQLITE_DQS=0: çift tırnaklı dizge kabulü yazım hatasını sessiz değere çevirirdi.
# SQLITE_OMIT_LOAD_EXTENSION: dosyadan kod yükleme yüzeyi olmasın. compile.html OMIT_* için amalgamation'da çalışma
# garantisi vermez; bu bayrağın etkisi graph testinde sqlite3_compileoption_used ile sınanır.
# SQLITE_DEFAULT_MEMSTATUS=0: compile.html önerilen kümesinde; sqlite3_status kullanılmıyor, ayırma başına kilit kalkar.
enable_language(C)
FetchContent_Declare(sqlite
    URL https://www.sqlite.org/2026/sqlite-amalgamation-3540100.zip
    URL_HASH SHA256=0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9)
FetchContent_MakeAvailable(sqlite)
add_library(noteapp_sqlite STATIC ${sqlite_SOURCE_DIR}/sqlite3.c)
target_include_directories(noteapp_sqlite SYSTEM PUBLIC ${sqlite_SOURCE_DIR})
target_compile_definitions(noteapp_sqlite PRIVATE
    SQLITE_THREADSAFE=2 SQLITE_DQS=0 SQLITE_OMIT_LOAD_EXTENSION SQLITE_DEFAULT_MEMSTATUS=0)
# Yalnız bu hedefin uyarıları kapalı: üçüncü parti C kodu bizim -Werror'umuza girmez.
target_compile_options(noteapp_sqlite PRIVATE $<IF:$<C_COMPILER_ID:MSVC>,/w,-w>)
find_package(Threads REQUIRED)
target_link_libraries(noteapp_sqlite PUBLIC Threads::Threads)

if(NOTEAPP_BUILD_GUI)
    # Not uygulaması ses, oyun kolu, kamera, GPU API'si kullanmaz: kapalı alt sistemler derleme süresini, çalışma zamanı
    # bağımlılığını ve saldırı yüzeyini küçültür. Dialog ve D-Bus açık kalır (portal dosya diyaloğu, sistem teması).
    set(SDL_SHARED OFF CACHE BOOL "")
    set(SDL_STATIC ON CACHE BOOL "")
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "")
    set(SDL_EXAMPLES OFF CACHE BOOL "")
    foreach(_sdl_option IN ITEMS AUDIO CAMERA GPU HAPTIC HIDAPI JOYSTICK POWER SENSOR VULKAN KMSDRM PIPEWIRE LIBUDEV
                                 LIBURING FRIBIDI LIBTHAI)
        set(SDL_${_sdl_option} OFF CACHE BOOL "")
    endforeach()
    FetchContent_Declare(SDL3
        URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-3.4.18.tar.gz
        URL_HASH SHA256=9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3
        EXCLUDE_FROM_ALL
        SYSTEM)
    FetchContent_MakeAvailable(SDL3)

    # Dear ImGui'nin CMake'i yoktur; yalnız kullanılan arka uçlar derlenir. imgui_stdlib: std::string'li InputText.
    FetchContent_Declare(imgui
        URL https://github.com/ocornut/imgui/archive/refs/tags/v1.93.0.tar.gz
        URL_HASH SHA256=f7bf6bc76e5d79d40d751096663e6c5d18daf08129dd52e7f7b09e4251c8eeee)
    FetchContent_MakeAvailable(imgui)
    add_library(imgui STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp)
    target_include_directories(imgui SYSTEM PUBLIC
        ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends ${imgui_SOURCE_DIR}/misc/cpp)
    # BMP dışı kod noktaları (emoji) çizilmese de metin işlemede tek kod noktası sayılmalı (plan: davranış 4).
    target_compile_definitions(imgui PUBLIC IMGUI_USE_WCHAR32)
    target_link_libraries(imgui PUBLIC SDL3::SDL3-static)
endif()
