#   make              编译全部
#   make run          编译并运行全部
#   make meaproto     meaproto 的全部子测试
#   make meaproto-wire / meaproto-core / meaproto-fragment   只跑其中一个
#   make clean        清理
#
# 二进制在 build/bin/，目标文件与依赖在 build/obj/（均被 .gitignore 的 **/build 忽略）。

CXX ?= g++
CXXFLAGS ?= -std=c++23 -Wall -Wextra -Werror -O1
CPPFLAGS ?= -I ../main/include

# 源文件搜索：测试源在本目录，固件源在 ../main/<模块>/
vpath %.cc ../main ../main/meaproto

# 每个测试一行：名字 = 源文件（固件源以 ../main/ 开头）
meaproto-wire = meaproto_wire_test.cc ../main/meaproto/wire.cc ../main/meaproto/hash.cc ../main/binfmt.cc
meaproto-core = meaproto_core_test.cc ../main/meaproto/core.cc ../main/meaproto/reassembly.cc ../main/meaproto/wire.cc ../main/meaproto/hash.cc ../main/binfmt.cc
meaproto-fragment = meaproto_fragment_test.cc ../main/meaproto/core.cc ../main/meaproto/reassembly.cc ../main/meaproto/wire.cc ../main/meaproto/hash.cc ../main/binfmt.cc

TESTS = meaproto-wire meaproto-core meaproto-fragment

# 源文件 → 目标文件：../main/a/b.cc → build/obj/a/b.o，其余 → build/obj/<name>.o
obj = $(addprefix build/obj/,$(patsubst %.cc,%.o,$(patsubst ../main/%,%,$(1))))

build/bin/meaproto-wire: $(call obj,$(meaproto-wire))
build/bin/meaproto-core: $(call obj,$(meaproto-core))
build/bin/meaproto-fragment: $(call obj,$(meaproto-fragment))

build/bin/%:
	@mkdir -p $(@D)
	$(CXX) $^ -o $@

build/obj/%.o: %.cc
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -MMD -MP -c $< -o $@

all: $(addprefix build/bin/,$(TESTS))

run: meaproto

meaproto: meaproto-wire meaproto-core meaproto-fragment

meaproto-wire meaproto-core meaproto-fragment: %: build/bin/%
	./$<

clean:
	rm -rf build

-include $(shell find build -name '*.d' 2>/dev/null)

.PHONY: all run meaproto meaproto-wire meaproto-core meaproto-fragment clean
