cmake_minimum_required(VERSION 3.0)

project(PingPong)

# 设置 C/C++ 编译器和编译选项
set(CMAKE_C_COMPILER gcc)  # C 编译器
set(CMAKE_CXX_COMPILER g++)  # C++ 编译器
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2 -g -Wall")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2 -g -Wall")

# 添加可执行文件 sndrcv_server 和链接库
add_executable(sndrcv send-recv.cpp)
target_link_libraries(sndrcv ibverbs)

# # 添加 "make clean" 目标
# add_custom_target(clean DEPENDS clean_all)
