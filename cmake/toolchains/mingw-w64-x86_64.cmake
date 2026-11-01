# Linux'ta Windows x86_64 için MinGW-w64 çapraz derleme (preset: windows-cross-*; imaj: docker/windows-cross.Containerfile).
# MINGW_ROOT: hedef kütüphanelerin kökü; Fedora dışı dağıtımda ortam değişkeniyle değiştirilir.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(_noteapp_mingw_triplet x86_64-w64-mingw32)
set(CMAKE_C_COMPILER ${_noteapp_mingw_triplet}-gcc)
set(CMAKE_CXX_COMPILER ${_noteapp_mingw_triplet}-g++)
set(CMAKE_RC_COMPILER ${_noteapp_mingw_triplet}-windres)
# libgit2'nin WinHTTP içe aktarma kütüphanesi için; önek olmadan `dlltool` arar, çapraz araç bu adla bulunmaz.
set(DLLTOOL ${_noteapp_mingw_triplet}-dlltool CACHE FILEPATH "")

if(DEFINED ENV{MINGW_ROOT})
    set(CMAKE_FIND_ROOT_PATH "$ENV{MINGW_ROOT}")
else()
    set(CMAKE_FIND_ROOT_PATH /usr/${_noteapp_mingw_triplet}/sys-root/mingw)
endif()
# Derleme sırasında çalışacak araçlar host'tan; bağlanacak her şey yalnız hedef kökten.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# add_test ve try_run .exe'leri Wine üzerinden çalıştırır.
set(CMAKE_CROSSCOMPILING_EMULATOR wine)
