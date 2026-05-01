cmake_minimum_required(VERSION 3.16)
project(TinyWebServer VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Source files
set(SOURCES
    main.cpp
    webserver.cpp
    config.cpp
    timer/lst_timer.cpp
    http/http_conn.cpp
    mysql/sql_connection_pool.cpp
    redis/redis_connection_pool.cpp
    redis/redis_cache.cpp
)
add_executable(server ${SOURCES})

# Threads
find_package(Threads REQUIRED)

# MySQL client library
find_library(MYSQLCLIENT_LIB mysqlclient)

# hiredis
find_library(HIREDIS_LIB hiredis)

target_link_libraries(server PRIVATE
    Threads::Threads
    ${MYSQLCLIENT_LIB}
    ${HIREDIS_LIB}
)

# Include directories
target_include_directories(server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/mysql
    ${CMAKE_CURRENT_SOURCE_DIR}/http
    ${CMAKE_CURRENT_SOURCE_DIR}/redis
    ${HIREDIS_INCLUDE_DIR}
)
