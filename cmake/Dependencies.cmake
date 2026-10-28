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

if(NOTEAPP_BUILD_TESTS)
    FetchContent_Declare(doctest
        URL https://github.com/doctest/doctest/archive/refs/tags/v2.5.3.tar.gz
        URL_HASH SHA256=174ebc4e769928959614789c5b4e9c3d0a0f81a62bb608756b127bfebfb21331)
    FetchContent_MakeAvailable(doctest)
endif()
