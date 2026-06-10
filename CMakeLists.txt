cmake_minimum_required(VERSION 3.16)
project(distributed-kvstore C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Compiler flags
add_compile_options(-Wall -Wextra -Werror -O2 -g -pthread)
add_compile_definitions(_GNU_SOURCE)

# Find required libraries
find_package(Threads REQUIRED)

# Include directories
include_directories(${CMAKE_SOURCE_DIR}/src)

# ---------------------------------------------------------------------------
# Common library (shared by server, client, and tests)
# ---------------------------------------------------------------------------
add_library(kvstore-common STATIC
    src/common/log.c
    src/common/buffer.c
    src/common/hash.c
    src/common/sha1.c
    src/common/config.c
)
target_link_libraries(kvstore-common PUBLIC Threads::Threads m)

# ---------------------------------------------------------------------------
# WAL library
# ---------------------------------------------------------------------------
add_library(kvstore-wal STATIC
    src/wal/wal.c
    src/wal/snapshot.c
)
target_link_libraries(kvstore-wal PUBLIC kvstore-common)

# ---------------------------------------------------------------------------
# Store library (KV store + consistent hashing)
# ---------------------------------------------------------------------------
add_library(kvstore-store STATIC
    src/store/kvstore.c
    src/store/shard.c
)
target_link_libraries(kvstore-store PUBLIC kvstore-common)

# ---------------------------------------------------------------------------
# Networking library
# ---------------------------------------------------------------------------
add_library(kvstore-net STATIC
    src/net/tcp_server.c
    src/net/tcp_client.c
    src/net/websocket.c
    src/net/protocol.c
)
target_link_libraries(kvstore-net PUBLIC kvstore-common)

# ---------------------------------------------------------------------------
# Raft library
# ---------------------------------------------------------------------------
add_library(kvstore-raft STATIC
    src/raft/raft.c
    src/raft/raft_log.c
    src/raft/raft_rpc.c
    src/raft/raft_timer.c
)
target_link_libraries(kvstore-raft PUBLIC kvstore-common kvstore-wal kvstore-net)

# ---------------------------------------------------------------------------
# Server executable
# ---------------------------------------------------------------------------
add_executable(kvstore-server
    src/server/main.c
)
target_link_libraries(kvstore-server PRIVATE
    kvstore-raft
    kvstore-wal
    kvstore-store
    kvstore-net
    kvstore-common
    Threads::Threads
    m
)

# ---------------------------------------------------------------------------
# CLI client executable
# ---------------------------------------------------------------------------
add_executable(kvstore-cli
    src/client/cli.c
)
target_link_libraries(kvstore-cli PRIVATE
    kvstore-common
    Threads::Threads
)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
enable_testing()

add_executable(test_wal tests/test_wal.c)
target_link_libraries(test_wal PRIVATE kvstore-wal kvstore-common Threads::Threads m)
add_test(NAME test_wal COMMAND test_wal)

add_executable(test_hash tests/test_hash.c)
target_link_libraries(test_hash PRIVATE kvstore-store kvstore-common Threads::Threads m)
add_test(NAME test_hash COMMAND test_hash)

add_executable(test_kvstore tests/test_kvstore.c)
target_link_libraries(test_kvstore PRIVATE kvstore-store kvstore-common Threads::Threads m)
add_test(NAME test_kvstore COMMAND test_kvstore)

add_executable(test_raft tests/test_raft.c)
target_link_libraries(test_raft PRIVATE kvstore-raft kvstore-wal kvstore-net kvstore-common Threads::Threads m)
add_test(NAME test_raft COMMAND test_raft)
