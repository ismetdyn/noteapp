noteapp_add_test(net http_test.cpp)
# Uçtan uca testlerin yerel HTTPS sunucusu (yalnız Linux'ta koşar, gerekçe http_test.cpp'de).
target_compile_definitions(net_tests PRIVATE NOTEAPP_NET_TEST_SERVER="${CMAKE_CURRENT_SOURCE_DIR}/https_server.py")
# Takılan bir aktarım derleme slotunu kilitlemesin: tüm ikili ~15 sn'de biter, üst sınır bunun birkaç katı.
set_tests_properties(net PROPERTIES TIMEOUT 120)
