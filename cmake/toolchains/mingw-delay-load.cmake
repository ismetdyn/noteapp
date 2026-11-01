# Windows'ta bilinen DLL (KnownDLLs) olmayan sistem DLL'leri içe aktarma tablosunda kalırsa, exe'nin yanına bırakılan aynı
# adlı DLL main'den önce yüklenir (DLL search-order hijack). Bu dört DLL gecikmeli yüklemeye alınır: ilk çağrıya kadar
# yüklenmezler, yüklenirken de main'in ilk satırındaki SetDefaultDllDirectories (src/core/dll_search.hpp) geçerlidir.
# GNU ld'de /DELAYLOAD yok; dlltool --output-delaylib içe aktarma kütüphanesi üretir. Aynı adla öne konan -L dizini,
# SDL3 ve libgit2'nin -lversion/-lwinmm/-lwinhttp/-lcrypt32 taleplerini link sırasına bakmadan buraya yönlendirir.
find_program(DLLTOOL dlltool REQUIRED)
set(NOTEAPP_DELAY_LOAD_DIR ${CMAKE_BINARY_DIR}/delay-load)
file(MAKE_DIRECTORY ${NOTEAPP_DELAY_LOAD_DIR})

foreach(_dll IN ITEMS version winmm winhttp crypt32)
    # Dolu değişkende find_library aramaz: her turda sıfırlanır.
    unset(_import_library)
    find_library(_import_library NAMES lib${_dll}.a REQUIRED NO_CACHE)
    execute_process(
        COMMAND ${CMAKE_NM} --defined-only ${_import_library}
        OUTPUT_VARIABLE _symbols
        COMMAND_ERROR_IS_FATAL ANY)
    string(REGEX MATCHALL "[0-9a-f]+ T [^\n]+" _exports "${_symbols}")
    list(TRANSFORM _exports REPLACE "^[0-9a-f]+ T " "")
    list(JOIN _exports "\n" _exports)
    file(WRITE ${NOTEAPP_DELAY_LOAD_DIR}/${_dll}.def "EXPORTS\n${_exports}\n")
    execute_process(
        COMMAND ${DLLTOOL} --input-def ${_dll}.def --dllname ${_dll}.dll --output-delaylib lib${_dll}.a
        WORKING_DIRECTORY ${NOTEAPP_DELAY_LOAD_DIR}
        COMMAND_ERROR_IS_FATAL ANY)
endforeach()

# Yardımcı (__delayLoadHelper2) stub'lardan sonra gelmeli; standart kütüphaneler link satırının sonundadır.
string(APPEND CMAKE_CXX_STANDARD_LIBRARIES " -ldelayimp")
