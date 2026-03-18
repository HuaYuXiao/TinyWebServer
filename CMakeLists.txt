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
    log/log.cpp
    CGImysql/sql_connection_pool.cpp
)

add_executable(server ${SOURCES})

# Threads
find_package(Threads REQUIRED)
if(Threads_FOUND)
    target_link_libraries(server PRIVATE Threads::Threads)
endif()

# MySQL client library
find_library(MYSQLCLIENT_LIB mysqlclient)
if(MYSQLCLIENT_LIB)
    target_link_libraries(server PRIVATE ${MYSQLCLIENT_LIB})
else()
    message(WARNING "Could not find mysqlclient library. You may need to install libmysqlclient and/or set MYSQLCLIENT_LIB paths.")
endif()

# Include directories
target_include_directories(server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/CGImysql
    ${CMAKE_CURRENT_SOURCE_DIR}/http
    ${CMAKE_CURRENT_SOURCE_DIR}/log
    ${CMAKE_CURRENT_SOURCE_DIR}/lock
)
