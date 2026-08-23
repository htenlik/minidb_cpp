cmake_minimum_required(VERSION 3.20)
project(minidb LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
add_library(minidb_core
    src/bplus_tree.cpp
    src/database_format.cpp
    src/page_allocator.cpp
    src/pager.cpp
    src/persistent_bplus_tree.cpp
    src/record_page.cpp
    src/record_store.cpp
    src/row.cpp
    src/schema.cpp
    src/slotted_page.cpp
    src/tuple_codec.cpp
    src/tuple_store.cpp
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

add_executable(page_allocator_test tests/page_allocator_test.cpp)
target_link_libraries(page_allocator_test PRIVATE minidb_core)
add_test(NAME page_allocator_test COMMAND page_allocator_test)

add_executable(record_page_test tests/record_page_test.cpp)
target_link_libraries(record_page_test PRIVATE minidb_core)
add_test(NAME record_page_test COMMAND record_page_test)

add_executable(slotted_page_test tests/slotted_page_test.cpp)
target_link_libraries(slotted_page_test PRIVATE minidb_core)
add_test(NAME slotted_page_test COMMAND slotted_page_test)

add_executable(record_store_test tests/record_store_test.cpp)
target_link_libraries(record_store_test PRIVATE minidb_core)
add_test(NAME record_store_test COMMAND record_store_test)

add_executable(tuple_store_test tests/tuple_store_test.cpp)
target_link_libraries(tuple_store_test PRIVATE minidb_core)
add_test(NAME tuple_store_test COMMAND tuple_store_test)

add_executable(tuple_store_random_test tests/tuple_store_random_test.cpp)
target_link_libraries(tuple_store_random_test PRIVATE minidb_core)
add_test(NAME tuple_store_random_test COMMAND tuple_store_random_test)

add_executable(schema_tuple_test tests/schema_tuple_test.cpp)
target_link_libraries(schema_tuple_test PRIVATE minidb_core)
add_test(NAME schema_tuple_test COMMAND schema_tuple_test)

add_executable(bplus_tree_test tests/bplus_tree_test.cpp)
target_link_libraries(bplus_tree_test PRIVATE minidb_core)
add_test(NAME bplus_tree_test COMMAND bplus_tree_test)

add_executable(persistent_bplus_tree_test tests/persistent_bplus_tree_test.cpp)
target_link_libraries(persistent_bplus_tree_test PRIVATE minidb_core)
add_test(NAME persistent_bplus_tree_test COMMAND persistent_bplus_tree_test)

add_executable(persistent_bplus_tree_delete_test tests/persistent_bplus_tree_delete_test.cpp)
target_link_libraries(persistent_bplus_tree_delete_test PRIVATE minidb_core)
add_test(NAME persistent_bplus_tree_delete_test COMMAND persistent_bplus_tree_delete_test)
