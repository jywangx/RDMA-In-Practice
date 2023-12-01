cmake_minimum_required(VERSION 3.0)

project(PingPong)

# 设置 C/C++ 编译器和编译选项
set(CMAKE_C_COMPILER gcc)  # C 编译器
set(CMAKE_CXX_COMPILER g++)  # C++ 编译器
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -g -Wall")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -Wall")

# Find necessary libraries (InfiniBand)
find_library(IBVERBS_LIBRARY NAMES ibverbs)

# RDMAEndpoint source files
set(SOURCE_FILES
        RDMAEndpoint.cpp
        send-recv.cpp)

# RDMAEndpoint header files
set(HEADER_FILES
        RDMAEndpoint.h)

# Executable
add_executable(sndrcv ${SOURCE_FILES} ${HEADER_FILES})

# Link libraries
target_link_libraries(sndrcv ${IBVERBS_LIBRARY})

