cmake_minimum_required(VERSION 3.16)
project(FlexQL VERSION 1.0.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ─── Build type defaults ────────────────────────────────────────────────────
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_FLAGS_RELEASE "-O3 -Ofast -DNDEBUG -march=native -mtune=native -flto -ffast-math -fno-signed-zeros -fno-trapping-math -fmerge-all-constants -fvisibility=hidden")
set(CMAKE_EXE_LINKER_FLAGS_RELEASE "-flto -fuse-linker-plugin")
set(CMAKE_CXX_FLAGS_DEBUG   "-g -O0 -DDEBUG -fsanitize=address,undefined")

# ─── Options ────────────────────────────────────────────────────────────────
option(FLEXQL_BUILD_TESTS     "Build unit tests"     ON)
option(FLEXQL_BUILD_BENCHMARK "Build benchmark suite" ON)

# ─── Include paths ──────────────────────────────────────────────────────────
include_directories(${CMAKE_SOURCE_DIR}/include)
include_directories(${CMAKE_SOURCE_DIR}/src)

# ─── Core library (shared between server and tests) ────────────────────────
set(FLEXQL_CORE_SOURCES
    src/server/storage/arena.cpp
    src/server/storage/schema.cpp
    src/server/storage/table.cpp
    src/server/parser/lexer.cpp
    src/server/parser/parser.cpp
    src/server/executor/executor.cpp
    src/server/index/bptree.cpp
    src/server/cache/lru_cache.cpp
    src/server/concurrency/thread_pool.cpp
    src/server/concurrency/lock_manager.cpp
    src/server/ttl/ttl_manager.cpp
    src/server/network/tcp_server.cpp
    src/server/database.cpp
)

add_library(flexql_core STATIC ${FLEXQL_CORE_SOURCES})
target_include_directories(flexql_core PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(flexql_core PRIVATE pthread)

# ─── Server executable ─────────────────────────────────────────────────────
add_executable(flexql_server src/server/main.cpp)
target_link_libraries(flexql_server PRIVATE flexql_core pthread)

# ─── Client library + executable ───────────────────────────────────────────
add_library(flexql_client_lib STATIC src/client/flexql_api.cpp)
target_include_directories(flexql_client_lib PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(flexql_client_lib PRIVATE pthread)

add_executable(flexql_client src/client/main.cpp)
target_link_libraries(flexql_client PRIVATE flexql_client_lib pthread)

# ─── Tests ──────────────────────────────────────────────────────────────────
if(FLEXQL_BUILD_TESTS)
    enable_testing()

    # Simple test driver (no external framework needed)
    set(TEST_SOURCES
        tests/test_arena.cpp
        tests/test_schema.cpp
        tests/test_storage.cpp
        tests/test_parser.cpp
        tests/test_bptree.cpp
        tests/test_executor.cpp
        tests/test_cache.cpp
        tests/test_integration.cpp
    )

    foreach(test_src ${TEST_SOURCES})
        get_filename_component(test_name ${test_src} NAME_WE)
        add_executable(${test_name} ${test_src})
        target_link_libraries(${test_name} PRIVATE flexql_core pthread)
        add_test(NAME ${test_name} COMMAND ${test_name})
    endforeach()
endif()

# ─── Benchmark ──────────────────────────────────────────────────────────────
if(FLEXQL_BUILD_BENCHMARK)
    add_executable(flexql_bench benchmark/bench_10m.cpp)
    target_link_libraries(flexql_bench PRIVATE flexql_core pthread)

    add_executable(flexql_eval ../benchmark_flexql.cpp)
    target_link_libraries(flexql_eval PRIVATE flexql_client_lib pthread)
endif()

message(STATUS "FlexQL v${PROJECT_VERSION} — Build type: ${CMAKE_BUILD_TYPE}")
