/* src/module.c
 * valkey-fractalsql v1.0 — Stochastic Fractal Search as a Valkey module.
 *
 * Stable ABI target: Valkey 7.2+ (includes 8.0 / 8.1). Valkey is the
 * Linux Foundation fork of Redis (March 2024) and preserves the
 * REDISMODULE_APIVER_1 module contract, so this source is byte-
 * identical to redis-fractalsql and the compiled .so loads unchanged
 * on every 7.2 / 8.0 / 8.1 server. Valkey ships redismodule.h as a
 * first-class header (alongside the preferred-alias valkeymodule.h),
 * so we keep #include "redismodule.h" and the RedisModule_* symbol
 * names for maximum ecosystem compat.
 *
 * Command
 *   FRACTAL.SEARCH <corpus_key> <query_blob> [<K>]
 *
 *     corpus_key   Redis key holding a BLOB of packed little-endian
 *                  float32 values: dim * N floats back-to-back,
 *                  where N = byte_len(corpus) / (dim * 4). The dim
 *                  is inferred from the query blob.
 *     query_blob   Packed little-endian float32 vector. Its length
 *                  defines `dim`.
 *     K            Optional integer, default 10, clamped to [1, 10000].
 *
 *   Reply: RESP3 map (auto-downgraded by Redis to RESP2 array of
 *   key/value pairs for RESP2 clients).
 *
 *     { "dim":        <int>,
 *       "n_corpus":   <int>,
 *       "best_point": [<double>, ...],
 *       "best_fit":   <double>,
 *       "top_k":      [{"idx": <int>, "dist": <double>}, ...] }
 *
 * Zero-copy
 *   The corpus and query blobs are read via RedisModule_StringDMA
 *   (direct memory access). The corpus buffer is cast to
 *   `const float *` and indexed in place — nothing is memcpy'd into
 *   a scratch buffer. Per-vector cosine distance is computed on the
 *   raw Redis-owned memory.
 *
 * Threading
 *   Command flags: "readonly fast allow-stale". Each thread that
 *   ever executes FRACTAL.SEARCH keeps its own LuaJIT state in
 *   __thread-local storage, lazily initialized on first call, so
 *   there is no cross-thread mutex in the fast path.
 *
 * Static LuaJIT
 *   libluajit-5.1.a is statically linked into fractalsql.so by the
 *   Docker build. The deployed module has no runtime dependency on
 *   LuaJIT — drop the .so in /usr/lib/valkey/modules and set
 *   loadmodule in valkey.conf.
 */

/* MSVC doesn't understand __attribute__, and redismodule.h from Redis
 * 7.4 tags the deprecated RedisModuleEvent_ReplBackup struct with an
 * unconditional __attribute__ ((deprecated)) (one of the three other
 * __attribute__ uses in that header are properly guarded with
 * #ifdef __GNUC__). Shim it out so MSVC can parse the header. */
#if defined(_MSC_VER) && !defined(__attribute__)
#  define __attribute__(x)
#endif

#include "redismodule.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
#include <luajit.h>

#include "sfs_core_bc.h"

/* Windows DLLs need RedisModule_OnLoad (and _OnUnload, if defined) in
 * the export table — Redis / Memurai's loader resolves the entry
 * point by name via GetProcAddress. Linux .so files land the symbol
 * in .dynsym automatically via -fvisibility=default. One macro,
 * applied at the definition site only — never forward-declare these
 * entry points, or MSVC's C2375 "different linkage" fires. */
#if defined(_WIN32) || defined(__CYGWIN__)
#  define FRACTAL_EXPORT __declspec(dllexport)
#else
#  define FRACTAL_EXPORT
#endif

/* Thread-local storage spelling. GCC/Clang: __thread. MSVC:
 * __declspec(thread). C11 _Thread_local would cover both, but MSVC's
 * C mode support for _Thread_local lands only on recent toolchains
 * with /std:c11 — use the per-compiler spelling for widest coverage. */
#if defined(_MSC_VER)
#  define FRACTAL_TLS __declspec(thread)
#else
#  define FRACTAL_TLS __thread
#endif

#define MODULE_NAME        "fractalsql"
#define MODULE_VERSION     1
#define COMMAND_NAME       "FRACTAL.SEARCH"
#define EDITION_STRING     "Community"
#define VERSION_STRING     "1.0.0"
#define DEFAULT_K          10
#define MAX_K              10000
#define DEFAULT_POP_SIZE   50
#define DEFAULT_ITERATIONS 30
#define DEFAULT_DIFF       2
#define DEFAULT_WALK       0.5

/* ------------------------------------------------------------------ */
/* Per-thread LuaJIT state                                            */
/*                                                                    */
/* Each Redis thread that executes FRACTAL.SEARCH lazily constructs   */
/* its own state on first call. States are never torn down — server   */
/* threads are long-lived so we accept the one-time-per-thread        */
/* allocation.                                                        */
/* ------------------------------------------------------------------ */

typedef struct sfs_tls {
    lua_State *L;
    int        module_ref;
    char      *result_buf;
    size_t     result_cap;
} sfs_tls;

static FRACTAL_TLS sfs_tls tls = { .L = NULL, .module_ref = LUA_NOREF };

/* Set in RedisModule_OnLoad to whether the running server exports
 * RedisModule_ReplyWithMap (added in Redis 7.0). On 6.2 the function
 * pointer is left NULL and calling it segfaults the server — we
 * fall back to ReplyWithArray with 2*N elements emitting alternating
 * key/value pairs, which is identical to the RESP2 view a RESP3 map
 * gets auto-downgraded to on newer servers anyway. */
static int g_have_reply_with_map = 0;

static int
reply_open_map(RedisModuleCtx *ctx, long n)
{
    if (g_have_reply_with_map)
        return RedisModule_ReplyWithMap(ctx, n);
    return RedisModule_ReplyWithArray(ctx, n * 2);
}

static bool
tls_init(void)
{
    int rc;

    if (tls.L != NULL) return true;

    tls.L = luaL_newstate();
    if (tls.L == NULL) return false;
    luaL_openlibs(tls.L);

    rc = luaL_loadbuffer(tls.L,
                         (const char *) luaJIT_BC_fractalsql_community,
                         luaJIT_BC_fractalsql_community_SIZE,
                         "=fractalsql_community");
    if (rc != 0) { lua_close(tls.L); tls.L = NULL; return false; }

    rc = lua_pcall(tls.L, 0, 1, 0);
    if (rc != 0) { lua_close(tls.L); tls.L = NULL; return false; }

    tls.module_ref = luaL_ref(tls.L, LUA_REGISTRYINDEX);
    return true;
}

static bool
ensure_result_cap(size_t need)
{
    if (tls.result_cap >= need) return true;
    size_t ncap = tls.result_cap ? tls.result_cap : 512;
    while (ncap < need) ncap *= 2;
    char *nb = realloc(tls.result_buf, ncap);
    if (nb == NULL) return false;
    tls.result_buf = nb;
    tls.result_cap = ncap;
    return true;
}

/* ------------------------------------------------------------------ */
/* Cosine distance — direct on the Redis-owned BLOB (zero-copy).      */
/* ------------------------------------------------------------------ */

static double
cosine_distance_f32_to_double(const float *a, const double *b, int dim)
{
    double dot = 0, na = 0, nb = 0;
    for (int i = 0; i < dim; i++) {
        double ai = (double) a[i];
        double bi = b[i];
        dot += ai * bi;
        na  += ai * ai;
        nb  += bi * bi;
    }
    if (na == 0 || nb == 0) return 1.0;
    return 1.0 - dot / (sqrt(na) * sqrt(nb));
}

/* Top-k selection over N corpus rows, ranking by distance to
 * best_point. Naive O(N*k); k is small so it beats a heap's
 * constant-factor overhead. */
static void
topk_by_distance(const float *corpus, int n_rows, int dim,
                 const double *best_point, int k,
                 int *out_idx, double *out_dist)
{
    for (int i = 0; i < k; i++) {
        out_idx[i]  = -1;
        out_dist[i] = INFINITY;
    }
    for (int r = 0; r < n_rows; r++) {
        double d = cosine_distance_f32_to_double(
                       corpus + (size_t) r * dim, best_point, dim);
        int worst = 0;
        for (int i = 1; i < k; i++)
            if (out_dist[i] > out_dist[worst]) worst = i;
        if (d < out_dist[worst]) {
            out_dist[worst] = d;
            out_idx[worst]  = r;
        }
    }
    /* insertion sort ascending by dist */
    for (int i = 1; i < k; i++) {
        double d = out_dist[i]; int idx = out_idx[i];
        int j = i - 1;
        while (j >= 0 && out_dist[j] > d) {
            out_dist[j+1] = out_dist[j];
            out_idx[j+1]  = out_idx[j];
            j--;
        }
        out_dist[j+1] = d;
        out_idx[j+1]  = idx;
    }
}

/* ------------------------------------------------------------------ */
/* SFS bridge: run sfs_core.run with cosine_fitness(query).           */
/* On success the Lua stack grows by [best_point, best_fit, ...].     */
/* ------------------------------------------------------------------ */

static int
run_sniper(lua_State *L, int module_ref,
           const double *qv, int dim)
{
    int rc, i;

    lua_rawgeti(L, LUA_REGISTRYINDEX, module_ref);  /* [M] */
    lua_getfield(L, -1, "run");                     /* [M, run] */
    lua_getfield(L, -2, "cosine_fitness");          /* [M, run, cf] */
    lua_remove(L, -3);                              /* [run, cf] */

    lua_createtable(L, dim, 0);
    for (i = 0; i < dim; i++) {
        lua_pushnumber(L, qv[i]);
        lua_rawseti(L, -2, i + 1);
    }
    rc = lua_pcall(L, 1, 1, 0);                     /* [run, fit_closure] */
    if (rc != 0) return rc;

    lua_createtable(L, 0, 8);                       /* [run, fn, cfg] */
    lua_createtable(L, dim, 0);
    for (i = 1; i <= dim; i++) { lua_pushnumber(L, -1.0); lua_rawseti(L, -2, i); }
    lua_setfield(L, -2, "lower");
    lua_createtable(L, dim, 0);
    for (i = 1; i <= dim; i++) { lua_pushnumber(L,  1.0); lua_rawseti(L, -2, i); }
    lua_setfield(L, -2, "upper");
    lua_pushinteger(L, DEFAULT_ITERATIONS); lua_setfield(L, -2, "max_generation");
    lua_pushinteger(L, DEFAULT_POP_SIZE);   lua_setfield(L, -2, "population_size");
    lua_pushinteger(L, DEFAULT_DIFF);       lua_setfield(L, -2, "maximum_diffusion");
    lua_pushnumber(L,  DEFAULT_WALK);       lua_setfield(L, -2, "walk");
    lua_pushboolean(L, 1);                  lua_setfield(L, -2, "bound_clipping");
    lua_pushvalue(L, -2);
    lua_setfield(L, -2, "fitness");
    lua_remove(L, -2);                              /* [run, cfg] */

    return lua_pcall(L, 1, 4, 0);                   /* [bp, bf, trace, paths] */
}

/* ------------------------------------------------------------------ */
/* FRACTAL.SEARCH command handler                                     */
/* ------------------------------------------------------------------ */

static int
FractalSearch_RedisCommand(RedisModuleCtx *ctx,
                           RedisModuleString **argv,
                           int argc)
{
    size_t      corpus_len = 0, query_len = 0;
    const char *corpus_blob = NULL;
    const char *query_blob  = NULL;
    int         dim = 0, n_corpus = 0, k = DEFAULT_K;
    int         saved_top, rc, i;
    double     *query = NULL;
    double     *best_point = NULL;
    int        *top_idx  = NULL;
    double     *top_dist = NULL;

    if (argc < 3 || argc > 4)
        return RedisModule_WrongArity(ctx);

    if (!tls_init())
        return RedisModule_ReplyWithError(ctx, "ERR failed to init LuaJIT state");

    /* Corpus key (zero-copy DMA). Use STRING DMA for read. */
    RedisModuleKey *kh = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ);
    if (kh == NULL)
        return RedisModule_ReplyWithError(ctx, "ERR corpus key missing");
    corpus_blob = RedisModule_StringDMA(kh, &corpus_len, REDISMODULE_READ);
    if (corpus_blob == NULL) {
        RedisModule_CloseKey(kh);
        return RedisModule_ReplyWithError(ctx, "ERR corpus key not a string/blob");
    }

    /* Query (value-arg, not a key). Pull contiguous bytes from
     * the RedisModuleString — zero-copy, lives on the request arena. */
    query_blob = RedisModule_StringPtrLen(argv[2], &query_len);
    if (query_blob == NULL || query_len == 0 || query_len % 4 != 0) {
        RedisModule_CloseKey(kh);
        return RedisModule_ReplyWithError(ctx,
            "ERR query must be a packed float32 BLOB (length % 4 == 0)");
    }
    dim = (int) (query_len / 4);

    if (corpus_len % ((size_t) dim * 4) != 0) {
        RedisModule_CloseKey(kh);
        return RedisModule_ReplyWithError(ctx,
            "ERR corpus length not a multiple of dim * 4");
    }
    n_corpus = (int) (corpus_len / ((size_t) dim * 4));

    /* Optional K. */
    if (argc == 4) {
        long long kv = 0;
        if (RedisModule_StringToLongLong(argv[3], &kv) != REDISMODULE_OK) {
            RedisModule_CloseKey(kh);
            return RedisModule_ReplyWithError(ctx, "ERR K must be an integer");
        }
        if (kv < 1)        kv = 1;
        if (kv > MAX_K)    kv = MAX_K;
        k = (int) kv;
    }
    if (k > n_corpus && n_corpus > 0) k = n_corpus;

    /* Decode query float32s -> doubles (once, not hot). */
    query = malloc((size_t) dim * sizeof(double));
    if (query == NULL) {
        RedisModule_CloseKey(kh);
        return RedisModule_ReplyWithError(ctx, "ERR OOM");
    }
    for (i = 0; i < dim; i++) {
        float f;
        memcpy(&f, query_blob + (size_t) i * 4, 4);
        query[i] = (double) f;
    }

    /* --- SFS sniper --------------------------------------------------- */
    saved_top = lua_gettop(tls.L);

    rc = run_sniper(tls.L, tls.module_ref, query, dim);
    if (rc != 0) {
        lua_settop(tls.L, saved_top);
        free(query);
        RedisModule_CloseKey(kh);
        return RedisModule_ReplyWithError(ctx, "ERR SFS call failed");
    }

    int bp_idx = saved_top + 1;
    int bf_idx = saved_top + 2;

    best_point = malloc((size_t) dim * sizeof(double));
    if (best_point == NULL) goto oom;
    for (i = 0; i < dim; i++) {
        lua_rawgeti(tls.L, bp_idx, i + 1);
        best_point[i] = lua_tonumber(tls.L, -1);
        lua_pop(tls.L, 1);
    }
    double best_fit = lua_tonumber(tls.L, bf_idx);

    /* --- top-k against the corpus (zero-copy over BLOB) --------------- */
    top_idx  = malloc((size_t) k * sizeof(int));
    top_dist = malloc((size_t) k * sizeof(double));
    if (top_idx == NULL || top_dist == NULL) goto oom;

    topk_by_distance((const float *) corpus_blob, n_corpus, dim,
                     best_point, k, top_idx, top_dist);

    /* --- RESP3 map reply --------------------------------------------- */
    /* Keys: dim, n_corpus, best_point, best_fit, top_k  (5 total).
     * reply_open_map falls back to a flat key/value array on Redis 6.2
     * where RedisModule_ReplyWithMap is NULL. */
    reply_open_map(ctx, 5);

    RedisModule_ReplyWithSimpleString(ctx, "dim");
    RedisModule_ReplyWithLongLong(ctx, (long long) dim);

    RedisModule_ReplyWithSimpleString(ctx, "n_corpus");
    RedisModule_ReplyWithLongLong(ctx, (long long) n_corpus);

    RedisModule_ReplyWithSimpleString(ctx, "best_point");
    RedisModule_ReplyWithArray(ctx, (long) dim);
    for (i = 0; i < dim; i++)
        RedisModule_ReplyWithDouble(ctx, best_point[i]);

    RedisModule_ReplyWithSimpleString(ctx, "best_fit");
    RedisModule_ReplyWithDouble(ctx, best_fit);

    RedisModule_ReplyWithSimpleString(ctx, "top_k");
    int valid = 0;
    for (int r = 0; r < k; r++) if (top_idx[r] >= 0) valid++;
    RedisModule_ReplyWithArray(ctx, (long) valid);
    for (int r = 0; r < k; r++) {
        if (top_idx[r] < 0) continue;
        reply_open_map(ctx, 2);
        RedisModule_ReplyWithSimpleString(ctx, "idx");
        RedisModule_ReplyWithLongLong(ctx, (long long) top_idx[r]);
        RedisModule_ReplyWithSimpleString(ctx, "dist");
        RedisModule_ReplyWithDouble(ctx, top_dist[r]);
    }

    lua_settop(tls.L, saved_top);
    free(query); free(best_point); free(top_idx); free(top_dist);
    RedisModule_CloseKey(kh);
    return REDISMODULE_OK;

oom:
    lua_settop(tls.L, saved_top);
    free(query); free(best_point); free(top_idx); free(top_dist);
    RedisModule_CloseKey(kh);
    return RedisModule_ReplyWithError(ctx, "ERR OOM emitting result");
}

/* ------------------------------------------------------------------ */
/* FRACTALSQL.EDITION / FRACTALSQL.VERSION — zero-key metadata commands.
 *                                                                    */
/* Mirrors the UDF triad on mariadb-fractalsql (fractalsql_edition /  */
/* fractalsql_version). Useful for operators to confirm which build   */
/* they loaded without reading the package metadata.                  */
/* ------------------------------------------------------------------ */

static int
FractalSqlEdition_RedisCommand(RedisModuleCtx *ctx,
                               RedisModuleString **argv, int argc)
{
    (void) argv;
    if (argc != 1) return RedisModule_WrongArity(ctx);
    return RedisModule_ReplyWithSimpleString(ctx, EDITION_STRING);
}

static int
FractalSqlVersion_RedisCommand(RedisModuleCtx *ctx,
                               RedisModuleString **argv, int argc)
{
    (void) argv;
    if (argc != 1) return RedisModule_WrongArity(ctx);
    return RedisModule_ReplyWithSimpleString(ctx, VERSION_STRING);
}

/* ------------------------------------------------------------------ */
/* Module entry                                                       */
/* ------------------------------------------------------------------ */

FRACTAL_EXPORT int
RedisModule_OnLoad(RedisModuleCtx *ctx,
                   RedisModuleString **argv, int argc)
{
    (void) argv;
    (void) argc;

    if (RedisModule_Init(ctx, MODULE_NAME, MODULE_VERSION,
                         REDISMODULE_APIVER_1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    /* ReplyWithMap is Redis 7.0+. On 6.2 the function pointer is left
     * NULL after RedisModule_Init; calling it segfaults. Cache the
     * availability flag once at load time. */
    g_have_reply_with_map = (RedisModule_ReplyWithMap != NULL);

    /* Command flags
     *   readonly    — never writes keys; safe under MULTI / replicas
     *   fast        — declares bounded / deterministic-ish work so
     *                 the server can schedule it on an I/O thread
     *                 where supported
     *   allow-stale — safe to serve from replica without consulting master
     *
     * First-key / last-key / step = 1, 1, 1: only argv[1] is a key.
     */
    if (RedisModule_CreateCommand(ctx,
            COMMAND_NAME,
            FractalSearch_RedisCommand,
            "readonly fast allow-stale",
            1, 1, 1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    /* Metadata commands. readonly, zero key args. Lowercase at
     * registration time is the standard Redis convention — CLI and
     * docs render them uppercase regardless. */
    if (RedisModule_CreateCommand(ctx,
            "fractalsql.edition",
            FractalSqlEdition_RedisCommand,
            "readonly fast",
            0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,
            "fractalsql.version",
            FractalSqlVersion_RedisCommand,
            "readonly fast",
            0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    return REDISMODULE_OK;
}
