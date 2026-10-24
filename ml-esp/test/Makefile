#   make            编译全部
#   make run        编译并运行全部
#   make arq        只跑 arq
#   make meaproto   只跑 meaproto
#   make clean      清理
#
# 产物在 build/ 下（已被 .gitignore 的 **/build 忽略）。

CXX ?= g++
CXXFLAGS ?= -std=c++23 -Wall -Wextra -Werror -O1
CPPFLAGS ?= -I ../main/include

ARQ_TEST = build/arq_host_test
ARQ_SRC = arq_host_test.cc ../main/wireless/arq.cc
ARQ_HDR = ../main/include/meguminlink/wireless/arq.hpp ../main/include/meguminlink/common.hpp

MEAPROTO_TEST = build/meaproto_test
MEAPROTO_SRC = meaproto_test.cc ../main/meaproto/wire.cc ../main/meaproto/hash.cc ../main/binfmt.cc
MEAPROTO_HDR = ../main/include/meaproto/wire.hpp ../main/include/meaproto/hash.hpp \
               ../main/include/meaproto/types.hpp ../main/include/meguminlink/binfmt.hpp \
               ../main/include/meguminlink/common.hpp

TESTS = $(ARQ_TEST) $(MEAPROTO_TEST)

all: $(TESTS)

$(ARQ_TEST): $(ARQ_SRC) $(ARQ_HDR)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $(ARQ_SRC) -o $@

$(MEAPROTO_TEST): $(MEAPROTO_SRC) $(MEAPROTO_HDR)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $(MEAPROTO_SRC) -o $@

run: $(TESTS)
	@for test in $(TESTS); do echo "== $$test"; ./$$test || exit 1; done

arq: $(ARQ_TEST)
	./$(ARQ_TEST)

meaproto: $(MEAPROTO_TEST)
	./$(MEAPROTO_TEST)

clean:
	rm -rf build

.PHONY: all run arq meaproto clean
