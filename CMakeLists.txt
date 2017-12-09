cmake_minimum_required(VERSION 3.9)
project(wbproxy)

set(CMAKE_CXX_STANDARD 11)
add_executable(wbproxy
        wblog.c
        wblog.h
        wbproxy.c)
if(WIN32)
    target_link_libraries(wbproxy wsock32 ws2_32)
endif()