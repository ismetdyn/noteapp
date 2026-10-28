noteapp_add_test(gui
    pdf_smoke_test.cpp
    note_list_model_test.cpp
    main_window_test.cpp)
# Ekransız ortamlarda (CI, Fedora container) da koşabilmek için pencere sistemi istemeyen platform eklentisi.
set_tests_properties(gui PROPERTIES ENVIRONMENT QT_QPA_PLATFORM=offscreen)
