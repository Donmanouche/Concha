# Unit tests (CTest). A minimal harness (tests/check.h), no test framework to download.
add_executable(concha_pattern_tests pattern_tests.cpp)
target_link_libraries(concha_pattern_tests PRIVATE concha_pattern)
add_test(NAME pattern COMMAND concha_pattern_tests)

add_executable(concha_sched_tests sched_tests.cpp)
target_link_libraries(concha_sched_tests PRIVATE concha_sched)
add_test(NAME sched COMMAND concha_sched_tests)

add_executable(concha_superdough_tests superdough_tests.cpp)
target_link_libraries(concha_superdough_tests PRIVATE concha_library)
add_test(NAME superdough COMMAND concha_superdough_tests)
