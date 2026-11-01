target_sources(graph_tests PRIVATE
    cypher_syntax_test.cpp
    cypher_read_test.cpp
    cypher_write_test.cpp
    cypher_equivalence_test.cpp
    cypher_scan_test.cpp
    cypher_security_test.cpp
    cypher_fuzz_regression_test.cpp)
