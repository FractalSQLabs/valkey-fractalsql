# syntax=docker/dockerfile:1.7
#
# valkey-fractalsql Factory build — zero-dependency posture.
#
# Produces ONE fractalsql.so per target architecture (driven by
# --platform). The Valkey Modules ABI (REDISMODULE_APIVER_1, aliased
# as VALKEYMODULE_APIVER_1) was preserved across the April 2024 fork
# from Redis, so a single .so works on every currently-shipped
# Valkey major — 7.2 / 8.0 / 8.1. Release packages depend on
# valkey / valkey-server generically rather than pinning a version.
#
# Build posture
#   LuaJIT is compiled from source as a PIC static archive and folded
#   into fractalsql.so. The resulting module depends on glibc only:
#   no libluajit-5.1.so, no libstdc++, no dynamic C++ ABI.
#
# Output (collected in the final `export` stage, emitted to the host
# via `docker buildx --output type=local,dest=...`):
#   /fractalsql.so
#
# Usage:
#   DOCKER_BUILDKIT=1 docker buildx build \
#     --platform linux/amd64 \
#     --target export \
#     --output type=local,dest=./dist/amd64 \
#     -f docker/Dockerfile .
#
# build.sh drives this for amd64 and arm64 in turn.
#
# --------------------------------------------------------------------
# Important build-system notes:
#
#  * The distro libluajit-5.1-dev ships a non-PIC .a, which fails to
#    link into a .so on most toolchains. We build LuaJIT 2.1 ourselves
#    with -fPIC + BUILDMODE=static and stage it into /opt/luajit.
#
#  * We clone LuaJIT from the GitHub mirror, not luajit.org/git. The
#    latter serves dumb HTTP and doesn't advertise shallow-clone
#    capabilities, so `git clone --depth=1` fails with "dumb http
#    transport does not support shallow capabilities".
#
#  * -fvisibility=hidden is deliberately NOT set on the TU defining
#    RedisModule_OnLoad. Valkey's dlopen loader resolves the entry
#    point through the .so's .dynsym; hiding it breaks MODULE LOAD.
#
#  * redismodule.h is a self-contained header fetched by shallow-
#    cloning the Valkey source at a pinned tag from the GitHub mirror.
#    Valkey ships redismodule.h alongside its preferred-alias
#    valkeymodule.h — we use redismodule.h to keep this source byte-
#    identical to redis-fractalsql. We do NOT #define REDISMODULE_CORE
#    — that macro is for server core itself and switches the header to
#    direct-call mode expecting a linked libredis/libvalkey. Modules
#    route every call through the function-pointer table that
#    RedisModule_Init populates.
#
#  * Post-link assertions (ldd / nm / size / exports) run in the
#    builder stage so the build fails fast if a dynamic luajit or
#    libstdc++ dependency sneaks back in, or if RedisModule_OnLoad
#    falls out of .dynsym.

# --------------------------------------------------------------------
# GLOBAL ARGs — declared before the first FROM so every stage below
# can re-import them with bare `ARG NAME` and pick up these defaults.
# ARGs declared INSIDE a stage are stage-scoped; later stages that
# re-import them pick up empty strings, which silently drops flags.
# --------------------------------------------------------------------
ARG LUAJIT_GIT_URL=https://github.com/LuaJIT/LuaJIT.git
ARG LUAJIT_GIT_REF=v2.1

# Valkey 8.1.0 is the current stable release at the v1.0.0 cut and
# ships a redismodule.h whose REDISMODULE_APIVER_1 surface is a
# superset of what 7.2 / 8.0 expose. Compiling against it yields a
# .so that loads cleanly on every one of those server versions.
ARG VALKEY_GIT_URL=https://github.com/valkey-io/valkey.git
ARG VALKEY_GIT_REF=8.1.0

# EXT_CFLAGS / EXT_LDFLAGS — the hardened flag set for fractalsql.so.
#
# -D_GLIBCXX_USE_CXX11_ABI=0: pre-C++11 std::string ABI; symbols never
#   reference __cxx11::basic_string even if the toolchain pulls
#   libstdc++ headers.
# -ffunction-sections / -fdata-sections: per-symbol section emission,
#   pairs with -Wl,--gc-sections to drop unused LuaJIT code paths.
# -fvisibility=default: RedisModule_OnLoad must live in .dynsym. If
#   any upstream CFLAGS injects -fvisibility=hidden later, the last
#   -fvisibility=* on the gcc command line wins and restores
#   visibility. We want this AFTER any other visibility flag — the
#   Dockerfile controls the invocation order.
# -static-libgcc / -static-libstdc++: link the C/C++ runtime helpers
#   statically; combined with --exclude-libs,ALL they stay invisible
#   in .dynsym.
# -Wl,--strip-all: drop .symtab. .dynsym survives, which is what the
#   Valkey loader needs for RedisModule_OnLoad resolution.
ARG EXT_CFLAGS="-D_GLIBCXX_USE_CXX11_ABI=0 -ffunction-sections -fdata-sections -fvisibility=default"
ARG EXT_LDFLAGS="-static-libgcc -static-libstdc++ -Wl,--gc-sections -Wl,--strip-all -Wl,--exclude-libs,ALL"

# Ceiling on the resulting .so. Generous (3 MiB) — fractalsql.so on
# amd64 lands comfortably under 1 MiB. Bump only with a note in the
# commit message; unexpected growth usually means something got
# linked in that shouldn't have.
ARG SIZE_CEILING_BYTES=3145728

# --------------------------------------------------------------------
# Stage: PIC-built LuaJIT static archive.
# --------------------------------------------------------------------
FROM debian:bookworm-slim AS luajit-static
ARG LUAJIT_GIT_URL
ARG LUAJIT_GIT_REF
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
        build-essential ca-certificates git \
 && rm -rf /var/lib/apt/lists/*
RUN git clone --depth=1 --branch "${LUAJIT_GIT_REF}" \
        "${LUAJIT_GIT_URL}" /src/luajit
WORKDIR /src/luajit
# BUILDMODE=static avoids producing the .so we don't need; XCFLAGS=-fPIC
# makes the .a foldable into a downstream shared object. Explicit
# TARGET_ARCH= unset because LuaJIT's Makefile uses it as a gcc flag
# knob — any outer env value makes gcc treat it as a linker input.
RUN unset TARGET_ARCH \
 && make BUILDMODE=static XCFLAGS="-fPIC" PREFIX=/opt/luajit -j"$(nproc)" TARGET_ARCH= \
 && make install BUILDMODE=static PREFIX=/opt/luajit \
 && test -f /opt/luajit/lib/libluajit-5.1.a \
 && ! test -f /opt/luajit/lib/libluajit-5.1.so

# --------------------------------------------------------------------
# Stage: harvest module headers from the pinned Valkey source tag.
# Unlike Redis's self-contained redismodule.h, Valkey's redismodule.h
# is a thin compat shim that `#include "valkeymodule.h"` — the real
# API surface lives in valkeymodule.h. Stage both into /out so the
# builder's -Iinclude picks up the full include graph.
# --------------------------------------------------------------------
FROM debian:bookworm-slim AS valkey-header
ARG VALKEY_GIT_URL
ARG VALKEY_GIT_REF
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
        ca-certificates git \
 && rm -rf /var/lib/apt/lists/*
RUN git clone --depth=1 --branch "${VALKEY_GIT_REF}" \
        "${VALKEY_GIT_URL}" /src/valkey \
 && test -f /src/valkey/src/redismodule.h \
 && test -f /src/valkey/src/valkeymodule.h \
 && install -Dm0644 /src/valkey/src/redismodule.h  /out/redismodule.h \
 && install -Dm0644 /src/valkey/src/valkeymodule.h /out/valkeymodule.h

# --------------------------------------------------------------------
# Stage: fractalsql.so builder. One stage covers all Valkey majors —
# the module ABI (REDISMODULE_APIVER_1 / VALKEYMODULE_APIVER_1) is
# stable across the April 2024 fork and the 7.2 / 8.0 / 8.1 line, so
# one .so per arch covers all currently-shipping majors.
# --------------------------------------------------------------------
FROM debian:bookworm-slim AS builder
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
        build-essential \
        binutils \
        file \
        pkg-config \
 && rm -rf /var/lib/apt/lists/*
COPY --from=luajit-static /opt/luajit /opt/luajit
COPY --from=valkey-header /out/redismodule.h  /build/include/redismodule.h
COPY --from=valkey-header /out/valkeymodule.h /build/include/valkeymodule.h

WORKDIR /build
COPY src/                /build/src/
COPY include/            /build/include/
COPY docker/assert_so.sh /build/docker/assert_so.sh

# Re-import the global ARGs into this stage. Bare `ARG NAME` — no
# default — pulls in the value declared before the first FROM.
ARG EXT_CFLAGS
ARG EXT_LDFLAGS
ARG SIZE_CEILING_BYTES

RUN set -eux; \
    gcc -Wall -Wextra -O3 -fPIC \
        -I/opt/luajit/include/luajit-2.1 \
        -Iinclude \
        ${EXT_CFLAGS} \
        -shared -o /fractalsql.so \
        src/module.c \
        /opt/luajit/lib/libluajit-5.1.a \
        -lm -ldl -lpthread \
        ${EXT_LDFLAGS}; \
    sh /build/docker/assert_so.sh /fractalsql.so "${SIZE_CEILING_BYTES}"

# --------------------------------------------------------------------
# Export stage — scratch, so `buildx --output=local` drops just the
# .so file onto the host.
# --------------------------------------------------------------------
FROM scratch AS export
COPY --from=builder /fractalsql.so /fractalsql.so
