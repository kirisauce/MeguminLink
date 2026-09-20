# ARQ 控制算法（main/arq.cc）的宿主机单元测试：把算法核心放在假时钟与假空口上跑，
# 覆盖丢帧、MAC 失败、重复、乱序、序号回绕、熔断与流控，不需要烧板。
#
#   make        编译
#   make run    编译并运行
#   make clean  清理
#
# 产物在 build/ 下（已被 .gitignore 的 **/build 忽略）。
#
# 测试输出用 C++23 的 std::print，因此这里固定 -std=c++23；核心本身只要求
# C++17。本机（GCC 16 / libstdc++ 16）直接编译即可，不需要额外链接库。

CXX ?= g++
CXXFLAGS ?= -std=c++23 -Wall -Wextra -Werror -O1
CPPFLAGS ?= -I ../main/include

SOURCES = arq_host_test.cc ../main/arq.cc
HEADERS = ../main/include/meguminlink/arq.hpp ../main/include/meguminlink/common.hpp
TARGET = build/arq_host_test

all: $(TARGET)

$(TARGET): $(SOURCES) $(HEADERS)
	@mkdir -p $(dir $(TARGET))
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $(SOURCES) -o $(TARGET)

run: $(TARGET)
	./$(TARGET)

clean:
	rm -rf build

.PHONY: all run clean
