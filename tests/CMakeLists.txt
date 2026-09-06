# Tests are compiled into a single deterministic executable.

file(GLOB SLOFABRIC_TEST_SOURCES CONFIGURE_DEPENDS "test_*.cpp")

add_executable(slofabric_tests ${SLOFABRIC_TEST_SOURCES} test_main.cpp)
target_link_libraries(slofabric_tests PRIVATE slofabric)
slofabric_apply_warnings(slofabric_tests)
target_include_directories(slofabric_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_test(NAME slofabric_tests COMMAND slofabric_tests)
