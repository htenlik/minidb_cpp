cmake_minimum_required(VERSION 3.20)
project(minidb LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
add_library(minidb_core
    src/buffer_pool_manager.cpp
    src/bplus_tree.cpp
    src/catalog.cpp
    src/database_format.cpp
    src/database_metadata_manager.cpp
    src/disk_manager.cpp
    src/lru_k_replacer.cpp
    src/log_manager.cpp
    src/page_allocator.cpp
    src/page_guard.cpp
    src/pager.cpp
    src/persistent_bplus_tree.cpp
    src/record_page.cpp
    src/record_store.cpp
    src/recovery_log.cpp
    src/recovery.cpp
    src/row.cpp
    src/schema.cpp
    src/slotted_page.cpp
    src/table.cpp
    src/table_definition.cpp
    src/tuple_codec.cpp
    src/tuple_store.cpp
    src/wal.cpp
)
target_include_directories(minidb_core PUBLIC include)

add_library(minidb_sql
    src/sql_ast.cpp
    src/sql_error.cpp
    src/sql_lexer.cpp
    src/sql_parser.cpp
)
target_include_directories(minidb_sql PUBLIC include)

add_library(minidb_sql_semantics
    src/sql_semantics.cpp
)
target_include_directories(minidb_sql_semantics PUBLIC include)
target_link_libraries(minidb_sql_semantics PUBLIC minidb_sql minidb_core)

add_library(minidb_sql_execution
    src/sql_executor.cpp
)
target_include_directories(minidb_sql_execution PUBLIC include)
target_link_libraries(minidb_sql_execution PUBLIC minidb_sql_semantics)

add_library(minidb_wire
    src/wire_protocol.cpp
)
target_include_directories(minidb_wire PUBLIC include)
target_link_libraries(minidb_wire PUBLIC minidb_sql_execution)

add_library(minidb_network
    src/cli_format.cpp
    src/database_server.cpp
    src/minidb_client.cpp
    src/tcp_server.cpp
    src/tcp_transport.cpp
)
target_include_directories(minidb_network PUBLIC include)
target_link_libraries(minidb_network PUBLIC minidb_wire)

execute_process(
    COMMAND git rev-parse --short=12 HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE MINIDB_GIT_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT MINIDB_GIT_COMMIT)
    set(MINIDB_GIT_COMMIT "unknown")
endif()

add_library(minidb_benchmark
    src/benchmark.cpp
    src/benchmark_workloads.cpp
)
target_include_directories(minidb_benchmark PUBLIC include)
target_link_libraries(minidb_benchmark PUBLIC minidb_network)
target_compile_definitions(minidb_benchmark PRIVATE
    MINIDB_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
    MINIDB_GIT_COMMIT="${MINIDB_GIT_COMMIT}"
)

add_executable(minidb src/main.cpp)
target_link_libraries(minidb PRIVATE minidb_core)

add_executable(minidb_server src/server_main.cpp)
target_link_libraries(minidb_server PRIVATE minidb_network)

add_executable(minidb_client src/client_main.cpp)
target_link_libraries(minidb_client PRIVATE minidb_network)

add_executable(minidb_bench src/benchmark_main.cpp)
target_link_libraries(minidb_bench PRIVATE minidb_benchmark)
enable_testing()
add_executable(pager_test tests/pager_test.cpp)
target_link_libraries(pager_test PRIVATE minidb_core)
add_test(NAME pager_test COMMAND pager_test)

add_executable(pager_stats_test tests/pager_stats_test.cpp)
target_link_libraries(pager_stats_test PRIVATE minidb_core)
add_test(NAME pager_stats_test COMMAND pager_stats_test)

add_executable(disk_manager_test tests/disk_manager_test.cpp)
target_link_libraries(disk_manager_test PRIVATE minidb_core)
add_test(NAME disk_manager_test COMMAND disk_manager_test)

add_executable(lru_k_replacer_test tests/lru_k_replacer_test.cpp)
target_link_libraries(lru_k_replacer_test PRIVATE minidb_core)
add_test(NAME lru_k_replacer_test COMMAND lru_k_replacer_test)

add_executable(buffer_pool_manager_test tests/buffer_pool_manager_test.cpp)
target_link_libraries(buffer_pool_manager_test PRIVATE minidb_core)
add_test(NAME buffer_pool_manager_test COMMAND buffer_pool_manager_test)

add_executable(buffer_pool_random_test tests/buffer_pool_random_test.cpp)
target_link_libraries(buffer_pool_random_test PRIVATE minidb_core)
add_test(NAME buffer_pool_random_test COMMAND buffer_pool_random_test)

add_executable(buffer_pool_wal_test tests/buffer_pool_wal_test.cpp)
target_link_libraries(buffer_pool_wal_test PRIVATE minidb_core)
add_test(NAME buffer_pool_wal_test COMMAND buffer_pool_wal_test)

add_executable(buffer_engine_integration_test tests/buffer_engine_integration_test.cpp)
target_link_libraries(buffer_engine_integration_test PRIVATE minidb_sql_execution)
add_test(NAME buffer_engine_integration_test COMMAND buffer_engine_integration_test)

add_executable(row_test tests/row_test.cpp)
target_link_libraries(row_test PRIVATE minidb_core)
add_test(NAME row_test COMMAND row_test)

add_executable(wal_codec_test tests/wal_codec_test.cpp)
target_link_libraries(wal_codec_test PRIVATE minidb_core)
add_test(NAME wal_codec_test COMMAND wal_codec_test)

add_executable(log_manager_test tests/log_manager_test.cpp)
target_link_libraries(log_manager_test PRIVATE minidb_core)
add_test(NAME log_manager_test COMMAND log_manager_test)

add_executable(recovery_log_test tests/recovery_log_test.cpp)
target_link_libraries(recovery_log_test PRIVATE minidb_core)
add_test(NAME recovery_log_test COMMAND recovery_log_test)

add_executable(recovery_test tests/recovery_test.cpp)
target_link_libraries(recovery_test PRIVATE minidb_core)
add_test(NAME recovery_test COMMAND recovery_test)

add_executable(wal_scanner_test tests/wal_scanner_test.cpp)
target_link_libraries(wal_scanner_test PRIVATE minidb_core)
add_test(NAME wal_scanner_test COMMAND wal_scanner_test)

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

add_executable(catalog_test tests/catalog_test.cpp)
target_link_libraries(catalog_test PRIVATE minidb_core)
add_test(NAME catalog_test COMMAND catalog_test)

add_executable(table_test tests/table_test.cpp)
target_link_libraries(table_test PRIVATE minidb_core)
add_test(NAME table_test COMMAND table_test)

add_executable(table_random_test tests/table_random_test.cpp)
target_link_libraries(table_random_test PRIVATE minidb_core)
add_test(NAME table_random_test COMMAND table_random_test)

add_executable(sql_lexer_test tests/sql_lexer_test.cpp)
target_link_libraries(sql_lexer_test PRIVATE minidb_sql)
add_test(NAME sql_lexer_test COMMAND sql_lexer_test)

add_executable(sql_parser_test tests/sql_parser_test.cpp)
target_link_libraries(sql_parser_test PRIVATE minidb_sql)
add_test(NAME sql_parser_test COMMAND sql_parser_test)

add_executable(sql_parser_robustness_test tests/sql_parser_robustness_test.cpp)
target_link_libraries(sql_parser_robustness_test PRIVATE minidb_sql)
add_test(NAME sql_parser_robustness_test COMMAND sql_parser_robustness_test)

add_executable(sql_semantics_test tests/sql_semantics_test.cpp)
target_link_libraries(sql_semantics_test PRIVATE minidb_sql_semantics)
add_test(NAME sql_semantics_test COMMAND sql_semantics_test)

add_executable(sql_executor_test tests/sql_executor_test.cpp)
target_link_libraries(sql_executor_test PRIVATE minidb_sql_execution)
add_test(NAME sql_executor_test COMMAND sql_executor_test)

add_executable(sql_executor_random_test tests/sql_executor_random_test.cpp)
target_link_libraries(sql_executor_random_test PRIVATE minidb_sql_execution)
add_test(NAME sql_executor_random_test COMMAND sql_executor_random_test)

add_executable(wire_protocol_test tests/wire_protocol_test.cpp)
target_link_libraries(wire_protocol_test PRIVATE minidb_wire)
add_test(NAME wire_protocol_test COMMAND wire_protocol_test)

add_executable(tcp_transport_test tests/tcp_transport_test.cpp)
target_link_libraries(tcp_transport_test PRIVATE minidb_network)
add_test(NAME tcp_transport_test COMMAND tcp_transport_test)

add_executable(network_protocol_test tests/network_protocol_test.cpp)
target_link_libraries(network_protocol_test PRIVATE minidb_network)
add_test(NAME network_protocol_test COMMAND network_protocol_test)

add_executable(network_stress_test tests/network_stress_test.cpp)
target_link_libraries(network_stress_test PRIVATE minidb_network)
add_test(NAME network_stress_test COMMAND network_stress_test)

add_executable(cli_format_test tests/cli_format_test.cpp)
target_link_libraries(cli_format_test PRIVATE minidb_network)
add_test(NAME cli_format_test COMMAND cli_format_test)

add_executable(benchmark_test tests/benchmark_test.cpp)
target_link_libraries(benchmark_test PRIVATE minidb_benchmark)
add_test(NAME benchmark_test COMMAND benchmark_test)

add_executable(benchmark_smoke_test tests/benchmark_smoke_test.cpp)
target_link_libraries(benchmark_smoke_test PRIVATE minidb_benchmark)
add_test(NAME benchmark_smoke_test COMMAND benchmark_smoke_test)

add_executable(wal_benchmark_test tests/wal_benchmark_test.cpp)
target_link_libraries(wal_benchmark_test PRIVATE minidb_benchmark)
add_test(NAME wal_benchmark_test COMMAND wal_benchmark_test)

add_executable(benchmark_quick_suite_test tests/benchmark_quick_suite_test.cpp)
target_link_libraries(benchmark_quick_suite_test PRIVATE minidb_benchmark)
add_test(NAME benchmark_quick_suite_test COMMAND benchmark_quick_suite_test)

add_executable(bplus_tree_test tests/bplus_tree_test.cpp)
target_link_libraries(bplus_tree_test PRIVATE minidb_core)
add_test(NAME bplus_tree_test COMMAND bplus_tree_test)

add_executable(persistent_bplus_tree_test tests/persistent_bplus_tree_test.cpp)
target_link_libraries(persistent_bplus_tree_test PRIVATE minidb_core)
add_test(NAME persistent_bplus_tree_test COMMAND persistent_bplus_tree_test)

add_executable(persistent_bplus_tree_delete_test tests/persistent_bplus_tree_delete_test.cpp)
target_link_libraries(persistent_bplus_tree_delete_test PRIVATE minidb_core)
add_test(NAME persistent_bplus_tree_delete_test COMMAND persistent_bplus_tree_delete_test)
