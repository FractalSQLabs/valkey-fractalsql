# valkey-fractalsql Makefile — local (non-Docker) development build.
#
# For shipping multi-arch artifacts use:
#   ./build.sh        (Docker, static LuaJIT, per-arch .so in ./dist/${arch}/)
#
# This Makefile is for quick local iteration. It:
#   1. Fetches include/redismodule.h from upstream Valkey if missing.
#   2. Compiles with -O3 -flto.
#   3. Links LuaJIT dynamically (faster to iterate than static).
#
# Build:   make
# Install: sudo make install
# Load:    valkey-cli MODULE LOAD /usr/lib/valkey/modules/fractalsql.so
#          (or add `loadmodule ...` to valkey.conf)

CC ?= gcc

# Default to Valkey's 8.1 branch — current stable. Override to pin a
# different release line. Valkey's redismodule.h is a thin compat
# shim that `#include "valkeymodule.h"`, so both headers have to
# land in include/ before src/module.c will compile. We keep
# `#include "redismodule.h"` in the source so it stays byte-identical
# to redis-fractalsql.
VALKEY_HEADER_BASE ?= https://raw.githubusercontent.com/valkey-io/valkey/8.1/src

# LuaJIT discovery
LUAJIT_CFLAGS := $(shell pkg-config --cflags luajit 2>/dev/null)
LUAJIT_LIBS   := $(shell pkg-config --libs luajit 2>/dev/null)
ifeq ($(strip $(LUAJIT_CFLAGS)),)
  LUAJIT_CFLAGS := -I/usr/include/luajit-2.1
  LUAJIT_LIBS   := -lluajit-5.1
endif

CFLAGS  = -Wall -Wextra -O3 -flto -fPIC $(LUAJIT_CFLAGS) -Iinclude
LDFLAGS = -shared -flto $(LUAJIT_LIBS) -lm -ldl -lpthread

TARGET = fractalsql.so
SRCS   = src/module.c
OBJS   = $(SRCS:.c=.o)

# Default Valkey module dir.
MODULE_DIR ?= /usr/lib/valkey/modules

all: $(TARGET)

# Fetch module headers on demand if missing. redismodule.h is the
# shim; valkeymodule.h carries the real API surface.
include/redismodule.h:
	@echo "Fetching redismodule.h from $(VALKEY_HEADER_BASE)/redismodule.h"
	@curl -fsSL "$(VALKEY_HEADER_BASE)/redismodule.h" -o $@

include/valkeymodule.h:
	@echo "Fetching valkeymodule.h from $(VALKEY_HEADER_BASE)/valkeymodule.h"
	@curl -fsSL "$(VALKEY_HEADER_BASE)/valkeymodule.h" -o $@

$(TARGET): $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

%.o: %.c include/sfs_core_bc.h include/redismodule.h include/valkeymodule.h
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)
	@# Note: include/*module.h is deliberately preserved across
	@# clean — refetch with `make distclean` if you want a fresh pull.

distclean: clean
	rm -f include/redismodule.h include/valkeymodule.h

install: $(TARGET)
	install -Dm0755 $(TARGET) $(MODULE_DIR)/fractalsql.so

.PHONY: all clean distclean install
