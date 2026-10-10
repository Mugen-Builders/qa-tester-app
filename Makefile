# qa-tester-app
#
#   make                 guest build (inside the riscv64 builder image; what the Dockerfile runs)
#   make host            native build against the libcmt mock + libcma built from source
#   make host-test       native unit tests + mock-rollup end-to-end check
#   make image           `cartesi build` (needs Docker with riscv64 emulation; see docs/build.md)
#
# Host builds expect the pinned dependencies under third-party/ (see `make deps`).

CXX ?= g++
AR  ?= ar
ARCH := $(shell uname -m)

WARN_CXXFLAGS = -Wall -Wextra -Wpedantic -Wformat -Werror=format-security -Wno-missing-field-initializers
OPT_CXXFLAGS ?= -O2
HARDEN_CXXFLAGS = -fhardened -fno-strict-aliasing -fno-strict-overflow
CXXFLAGS += -std=c++20 $(WARN_CXXFLAGS) $(OPT_CXXFLAGS) -Iinclude $(EXTRA_CXXFLAGS)

LIB_SRC := src/lib/types.cpp src/lib/abi.cpp src/lib/config.cpp src/lib/commands.cpp src/lib/ledger.cpp src/lib/engine.cpp
MAIN_SRC := src/main.cpp

# ---------------------------------------------------------------------------- guest (riscv64)
# Headers and libraries come from the packages installed in the builder stage:
#   /usr/include/libcmt, /usr/include/libcma, /usr/include/nlohmann, libcmt.a, libcma.a
guest_OBJDIR := build/guest
guest_OBJ := $(patsubst %.cpp,$(guest_OBJDIR)/%.o,$(LIB_SRC) $(MAIN_SRC))
guest_BIN := $(guest_OBJDIR)/qa-tester

ifeq ($(ARCH),riscv64)
all: guest
else
all: host
endif

guest: $(guest_BIN)

$(guest_OBJDIR)/%.o: %.cpp
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(HARDEN_CXXFLAGS) -MMD -MP -c -o $@ $<

$(guest_BIN): $(guest_OBJ)
	$(CXX) $(CXXFLAGS) $(HARDEN_CXXFLAGS) -static -o $@ $^ -l:libcma.a -l:libcmt.a

# ---------------------------------------------------------------------------- host (native)
GUEST_TOOLS_DIR ?= third-party/machine-guest-tools
LIBCMA_DIR      ?= third-party/machine-asset-tools
host_LIBCMT_A   := $(GUEST_TOOLS_DIR)/sys-utils/libcmt/build/mock/libcmt.a
host_LIBCMA_A   := $(LIBCMA_DIR)/build/$(ARCH)/libcma.a
host_INC := -I$(GUEST_TOOLS_DIR)/sys-utils/libcmt/include -I$(LIBCMA_DIR)/include -I$(LIBCMA_DIR)/third-party

host_OBJDIR := build/host
host_OBJ := $(patsubst %.cpp,$(host_OBJDIR)/%.o,$(LIB_SRC))
host_MAIN_OBJ := $(host_OBJDIR)/src/main.o
host_LIB := $(host_OBJDIR)/libqatester.a
host_BIN := $(host_OBJDIR)/qa-tester
host_TEST := $(host_OBJDIR)/unit-tests

host: $(host_BIN) $(host_TEST)

$(host_OBJDIR)/%.o: %.cpp
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(host_INC) -g -MMD -MP -c -o $@ $<

$(host_LIB): $(host_OBJ)
	$(AR) rcs $@ $^

$(host_BIN): $(host_MAIN_OBJ) $(host_LIB)
	$(CXX) $(CXXFLAGS) -o $@ $(host_MAIN_OBJ) $(host_LIB) $(host_LIBCMA_A) $(host_LIBCMT_A)

$(host_TEST): $(host_OBJDIR)/tests/host/unit_tests.o $(host_LIB)
	$(CXX) $(CXXFLAGS) -o $@ $< $(host_LIB) $(host_LIBCMA_A) $(host_LIBCMT_A)

host-test: host
	$(host_TEST)
	tests/host/mock_e2e.sh $(host_BIN)

# Pinned dependency sources for host builds (network).
GUEST_TOOLS_TAG ?= v0.18.0
LIBCMA_TAG ?= v0.1.0-alpha.11
deps:
	@mkdir -p third-party
	@test -d $(GUEST_TOOLS_DIR) || git clone -q --depth 1 --branch $(GUEST_TOOLS_TAG) https://github.com/cartesi/machine-guest-tools.git $(GUEST_TOOLS_DIR)
	@test -d $(LIBCMA_DIR) || git clone -q --depth 1 --branch $(LIBCMA_TAG) https://github.com/Mugen-Builders/machine-asset-tools.git $(LIBCMA_DIR)
	$(MAKE) -C $(GUEST_TOOLS_DIR)/sys-utils/libcmt mock
	$(MAKE) -C $(LIBCMA_DIR) third-party libcma

# ---------------------------------------------------------------------------- image
image:
	cartesi build

-include $(wildcard build/host/*/*.d build/host/*/*/*.d build/guest/*/*.d build/guest/*/*/*.d)

clean:
	rm -rf build

.PHONY: all guest host host-test deps image clean
