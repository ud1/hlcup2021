cmake_minimum_required(VERSION 3.14)
project(cppAsync)

set(CMAKE_CXX_STANDARD 17)

SET(CMAKE_CXX_FLAGS "-D_LINUX -O2 -g -Wall -Wno-unknown-pragmas -Werror=return-type -DMAX_TOTAL_REQS=300000 ")

add_executable(cppAsync main.cpp simdjson.cpp)

target_link_libraries(cppAsync pthread)
