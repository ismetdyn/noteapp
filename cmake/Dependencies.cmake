include(FetchContent)

# Tüm üçüncü parti bağımlılıklar burada pin'lenir (SPEC.md §3); başka hiçbir dosya FetchContent çağırmaz.

FetchContent_Declare(tomlplusplus
    URL https://github.com/marzer/tomlplusplus/archive/refs/tags/v3.4.0.tar.gz
    URL_HASH SHA256=8517f65938a4faae9ccf8ebb36631a38c1cadfb5efa85d9a72e15b9e97d25155)
FetchContent_MakeAvailable(tomlplusplus)

if(NOTEAPP_BUILD_TESTS)
    FetchContent_Declare(doctest
        URL https://github.com/doctest/doctest/archive/refs/tags/v2.5.3.tar.gz
        URL_HASH SHA256=174ebc4e769928959614789c5b4e9c3d0a0f81a62bb608756b127bfebfb21331)
    FetchContent_MakeAvailable(doctest)
endif()
