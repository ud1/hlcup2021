cmake_minimum_required(VERSION 3.14)
project(cpp)

set(CMAKE_CXX_STANDARD 17)

SET(CMAKE_CXX_FLAGS "-D_LINUX -O2 -g -Wall -Wno-unknown-pragmas -Werror=return-type -DMAX_REQS_T=100000 ")

add_executable(cpp main.cpp)
target_link_libraries(cpp pthread)
