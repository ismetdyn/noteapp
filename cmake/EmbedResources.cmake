# resources/ altındaki dosyaları C++ bayt dizilerine çevirip hedefe gömer.
#   noteapp_embed_resources(<hedef> <resources/ altındaki göreli dosyalar...>)
# Üretilen kaynak, core/resources.hpp'deki embedded_resource() işlevini tanımlar. Dosya değişince
# add_custom_command DEPENDS sayesinde yeniden üretilir. Aynı dosya üretimi `cmake -P` ile de yürütür.

if(CMAKE_SCRIPT_MODE_FILE)
    # Betik modu: -DOUT=<çıktı.cpp> -DRES_DIR=<dizin> -DFILES=<a|b|c>
    string(REPLACE "|" ";" _files "${FILES}")
    set(_arrays "")
    set(_table "")
    set(_i 0)
    foreach(_f IN LISTS _files)
        file(READ "${RES_DIR}/${_f}" _hex HEX)
        string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _bytes "${_hex}")
        string(LENGTH "${_hex}" _hex_len)
        math(EXPR _size "${_hex_len} / 2")
        # Boş dosyada sıfır uzunluklu dizi geçersiz olurdu; sondaki 0x00 boyuta dahil değildir.
        string(APPEND _arrays "const unsigned char kData${_i}[] = {${_bytes}0x00};\n")
        string(APPEND _table "    {\"${_f}\", kData${_i}, ${_size}},\n")
        math(EXPR _i "${_i} + 1")
    endforeach()
    file(WRITE "${OUT}" "// Otomatik üretildi (cmake/EmbedResources.cmake); elle düzenlenmez.
#include \"core/resources.hpp\"

#include <cstddef>

namespace noteapp::core {

namespace {

${_arrays}
struct Entry {
    std::string_view name;
    const unsigned char* data;
    std::size_t size;
};

const Entry kEntries[] = {
${_table}};

}  // namespace

std::optional<std::string_view> embedded_resource(std::string_view relative_path) {
    for (const Entry& e : kEntries) {
        if (e.name == relative_path)
            return std::string_view(reinterpret_cast<const char*>(e.data), e.size);
    }
    return std::nullopt;
}

}  // namespace noteapp::core
")
    return()
endif()

set(NOTEAPP_EMBED_SCRIPT "${CMAKE_CURRENT_LIST_FILE}")

function(noteapp_embed_resources target)
    set(_res_dir "${PROJECT_SOURCE_DIR}/resources")
    set(_out "${CMAKE_CURRENT_BINARY_DIR}/embedded_resources.cpp")
    set(_deps "")
    foreach(_f IN LISTS ARGN)
        list(APPEND _deps "${_res_dir}/${_f}")
    endforeach()
    list(JOIN ARGN "|" _files)
    add_custom_command(
        OUTPUT "${_out}"
        COMMAND ${CMAKE_COMMAND} -DOUT=${_out} -DRES_DIR=${_res_dir} "-DFILES=${_files}"
                -P "${NOTEAPP_EMBED_SCRIPT}"
        DEPENDS ${_deps} "${NOTEAPP_EMBED_SCRIPT}"
        COMMENT "Kaynaklar gömülüyor: ${_files}"
        VERBATIM)
    target_sources(${target} PRIVATE "${_out}")
endfunction()
