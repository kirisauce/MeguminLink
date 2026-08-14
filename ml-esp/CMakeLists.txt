# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 默认 Release 构建：SWD 位时钟循环与 USB 热路径的优化代码差异显著
# （实测烧录 0.37s vs Debug 0.48s），且不损失调试能力（仍可连接 GDB）。
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(
    megumin-link-esp
    LANGUAGE C CXX
)
