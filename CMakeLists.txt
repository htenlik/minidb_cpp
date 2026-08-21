cmake_minimum_required(VERSION 3.20)
project(minidb LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
add_library(minidb_core
    src/pager.cpp
    src/row.cpp
)
target_include_directories(minidb_core PUBLIC include)
add_executable(minidb src/main.cpp)
target_link_libraries(minidb PRIVATE minidb_core)
enable_testing()
add_executable(pager_test tests/pager_test.cpp)
target_link_libraries(pager_test PRIVATE minidb_core)
add_test(NAME pager_test COMMAND pager_test)
