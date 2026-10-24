#   make              编译全部
#   make run          编译并运行全部
#   make meaproto     meaproto 的全部子测试
#   make arq / meaproto-wire / meaproto-core   只跑其中一个
#   make clean        清理
#
# 二进制在 build/bin/，目标文件与依赖在 build/obj/（均被 .gitignore 的 **/build 忽略）。

CXX ?= g++
CXXFLAGS ?= -std=c++23 -Wall -Wextra -Werror -O1
CPPFLAGS ?= -I ../main/include

# 源文件搜索：测试源在本目录，固件源在 ../main/<模块>/
vpath %.cc ../main ../main/wireless ../main/meaproto

# 每个测试一行：名字 = 源文件（固件源以 ../main/ 开头）
arq           = arq_host_test.cc ../main/wireless/arq.cc
meaproto-wire = meaproto_wire_test.cc ../main/meaproto/wire.cc ../main/meaproto/hash.cc ../main/binfmt.cc
meaproto-core = meaproto_core_test.cc ../main/meaproto/core.cc ../main/meaproto/wire.cc ../main/meaproto/hash.cc ../main/binfmt.cc

TESTS = arq meaproto-wire meaproto-core

# 源文件 → 目标文件：../main/a/b.cc → build/obj/a/b.o，其余 → build/obj/<name>.o
obj = $(addprefix build/obj/,$(patsubst %.cc,%.o,$(patsubst ../main/%,%,$(1))))

build/bin/arq: $(call obj,$(arq))
build/bin/meaproto-wire: $(call obj,$(meaproto-wire))
build/bin/meaproto-core: $(call obj,$(meaproto-core))

build/bin/%:
	@mkdir -p $(@D)
	$(CXX) $^ -o $@

build/obj/%.o: %.cc
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -MMD -MP -c $< -o $@

all: $(addprefix build/bin/,$(TESTS))

run: arq meaproto

meaproto: meaproto-wire meaproto-core

arq meaproto-wire meaproto-core: %: build/bin/%
	./$<

clean:
	rm -rf build

-include $(shell find build -name '*.d' 2>/dev/null)

.PHONY: all run arq meaproto meaproto-wire meaproto-core clean
