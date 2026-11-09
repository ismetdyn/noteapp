# HTTPS istemcisi: yalnız güncelleme denetimi kullanır, yalnız noteapp_app'e bağlanır. curl başlıkları ve
# kütüphanesi PRIVATE: arayüz (http.hpp) curl türü taşımaz.
add_library(noteapp_net http.cpp)
target_include_directories(noteapp_net PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(noteapp_net PUBLIC noteapp_core PRIVATE CURL::libcurl_static)
if(WIN32)
    # WinHttpGetIEProxyConfigForCurrentUser: sistem vekili. winhttp gecikmeli yüklenir
    # (cmake/toolchains/mingw-delay-load.cmake).
    target_link_libraries(noteapp_net PRIVATE winhttp)
endif()
noteapp_target_defaults(noteapp_net)
