cmake_minimum_required(VERSION 3.20)
project(minidb LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
add_library(minidb_core
    src/database_format.cpp
    src/pager.cpp
    src/record_page.cpp
    src/row.cpp
)
target_include_directories(minidb_core PUBLIC include)
add_executable(minidb src/main.cpp)
target_link_libraries(minidb PRIVATE minidb_core)
enable_testing()
add_executable(pager_test tests/pager_test.cpp)
target_link_libraries(pager_test PRIVATE minidb_core)
add_test(NAME pager_test COMMAND pager_test)

add_executable(row_test tests/row_test.cpp)
target_link_libraries(row_test PRIVATE minidb_core)
add_test(NAME row_test COMMAND row_test)

add_executable(database_format_test tests/database_format_test.cpp)
target_link_libraries(database_format_test PRIVATE minidb_core)
add_test(NAME database_format_test COMMAND database_format_test)
