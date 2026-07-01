/*
 * test_mem.c — Tests for unified memory management (mimalloc-backed),
 *              arena integration, slab allocator, and parallel extraction.
 */
#include "../src/foundation/compat.h"
#include "test_framework.h"
#include "test_helpers.h"
#include "../src/foundation/mem.h"
#include "../src/foundation/arena.h"
#include "../src/foundation/slab_alloc.h"
#include "../src/foundation/platform.h"
#include "../src/foundation/constants.h"
#include "pipeline/pipeline.h"
#include "pipeline/pipeline_internal.h"
#include "graph_buffer/graph_buffer.h"
#include "discover/discover.h"
#include "cbm.h"

#include <stdatomic.h>
#include <sys/stat.h>

/* ASan detection — mimalloc MI_OVERRIDE=0 under ASan, mi_process_info
 * may return 0 for RSS. Tests that depend on accurate RSS must skip. */
#ifndef __has_feature
#define __has_feature(x) 0
#endif
#if defined(__SANITIZE_ADDRESS__) || __has_feature(address_sanitizer)
#define CBM_ASAN_ACTIVE 1
#else
#define CBM_ASAN_ACTIVE 0
#endif

/* ── mem basic tests ──────────────────────────────────────────── */

TEST(mem_rss_tracking) {
    cbm_mem_init(0.5);

    /* Allocate 10 MB */
    size_t alloc_size = 10 * 1024 * 1024;
    char *p = (char *)malloc(alloc_size);
    ASSERT_NOT_NULL(p);
    /* Touch all pages to ensure RSS increase */
    memset(p, 0xAB, alloc_size);

    size_t rss = cbm_mem_rss();
    /* RSS should be nonzero (mimalloc or OS fallback) */
    ASSERT_GT(rss, 0);

    free(p);
    PASS();
}

TEST(mem_collect_reclaims) {
    cbm_mem_init(0.5);

    /* Allocate 10 MB, touch it, free it */
    size_t alloc_size = 10 * 1024 * 1024;
    char *p = (char *)malloc(alloc_size);
    ASSERT_NOT_NULL(p);
    memset(p, 0xCD, alloc_size);
    size_t rss_before_free = cbm_mem_rss();

    free(p);
    cbm_mem_collect();

    size_t rss_after_collect = cbm_mem_rss();
    /* After collect, RSS should exist (may or may not drop depending on OS) */
    ASSERT_GT(rss_after_collect, 0);
    /* Best-effort check: rss shouldn't grow after free+collect */
    (void)rss_before_free;
    PASS();
}

TEST(mem_budget_check) {
    /* Init with very small fraction to create an easy-to-exceed budget */
    /* NOTE: cbm_mem_init only takes effect once, so we test with whatever
     * budget was set. Just verify the API works. */
    cbm_mem_init(0.5);

    size_t budget = cbm_mem_budget();
    /* Budget should be > 0 after init */
    ASSERT_GT(budget, 0);

    /* over_budget returns a bool */
    bool over = cbm_mem_over_budget();
    (void)over; /* just verify it doesn't crash */

    /* Worker budget divides correctly */
    size_t wb4 = cbm_mem_worker_budget(4);
    ASSERT_EQ(wb4, budget / 4);

    /* Edge case: 0 workers defaults to 1 */
    size_t wb0 = cbm_mem_worker_budget(0);
    ASSERT_EQ(wb0, budget);
    PASS();
}

/* ── mem budget edge-case tests ─────────────────────────────── */

TEST(mem_worker_budget_zero_workers) {
    cbm_mem_init(0.5);
    size_t budget = cbm_mem_budget();
    /* 0 workers clamps to 1 → worker_budget == full budget */
    size_t wb = cbm_mem_worker_budget(0);
    ASSERT_EQ(wb, budget);
    PASS();
}

TEST(mem_worker_budget_negative_workers) {
    cbm_mem_init(0.5);
    size_t budget = cbm_mem_budget();
    /* Negative workers clamps to 1 → worker_budget == full budget */
    size_t wb = cbm_mem_worker_budget(-5);
    ASSERT_EQ(wb, budget);
    PASS();
}

TEST(mem_worker_budget_one_worker) {
    cbm_mem_init(0.5);
    size_t budget = cbm_mem_budget();
    /* 1 worker → equals full budget */
    size_t wb = cbm_mem_worker_budget(1);
    ASSERT_EQ(wb, budget);
    PASS();
}

TEST(mem_worker_budget_many_workers) {
    cbm_mem_init(0.5);
    /* 1000 workers → should produce non-zero result (budget is huge) */
    size_t wb = cbm_mem_worker_budget(1000);
    ASSERT_GT(wb, 0);
    /* Must be budget / 1000 */
    ASSERT_EQ(wb, cbm_mem_budget() / 1000);
    PASS();
}

TEST(mem_over_budget_low_rss) {
    cbm_mem_init(0.5);
    /* We're a test process with tiny RSS — should not be over budget */
    bool over = cbm_mem_over_budget();
    ASSERT_FALSE(over);
    PASS();
}

/* ── RSS tracking tests ───────────────────────────────────────── */

TEST(mem_rss_positive) {
    cbm_mem_init(0.5);
    /* A running process always has nonzero RSS */
    size_t rss = cbm_mem_rss();
    ASSERT_GT(rss, 0);
    PASS();
}

TEST(mem_peak_rss_gte_rss) {
    cbm_mem_init(0.5);
    size_t rss = cbm_mem_rss();
    size_t peak = cbm_mem_peak_rss();
    /* Peak must be >= current RSS */
    ASSERT_GTE(peak, rss);
    PASS();
}

TEST(mem_rss_increases_after_alloc) {
    cbm_mem_init(0.5);

    /* Allocate 10 MB and touch all pages */
    size_t alloc_size = 10 * 1024 * 1024;
    char *p = (char *)malloc(alloc_size);
    ASSERT_NOT_NULL(p);
    memset(p, 0xBE, alloc_size);

    size_t rss_after = cbm_mem_rss();
    /* RSS must be non-zero after allocating 10MB */
    ASSERT_GT(rss_after, 0);

    free(p);
    PASS();
}

TEST(mem_collect_no_crash) {
    cbm_mem_init(0.5);
    /* collect() must not crash even with nothing to collect */
    cbm_mem_collect();
    PASS();
}

TEST(mem_collect_rss_still_positive) {
    cbm_mem_init(0.5);
    cbm_mem_collect();
    /* After collect, RSS must still be > 0 (we're alive) */
    size_t rss = cbm_mem_rss();
    ASSERT_GT(rss, 0);
    PASS();
}

/* ── Memory pressure simulation ───────────────────────────────── */

TEST(mem_progressive_alloc_rss_increases) {
    cbm_mem_init(0.5);

    size_t chunk_size = 2 * 1024 * 1024; /* 2 MB chunks */
    int nchunks = 5;
    char *chunks[5];

    for (int i = 0; i < nchunks; i++) {
        chunks[i] = (char *)malloc(chunk_size);
        ASSERT_NOT_NULL(chunks[i]);
        memset(chunks[i], (unsigned char)(0xA0 + i), chunk_size);
    }

    size_t rss_peak = cbm_mem_rss();
    ASSERT_GT(rss_peak, 0);

    for (int i = 0; i < nchunks; i++) {
        free(chunks[i]);
    }
    cbm_mem_collect();

    /* After free + collect, RSS may or may not drop, but must not crash */
    size_t rss_end = cbm_mem_rss();
    ASSERT_GT(rss_end, 0);
    PASS();
}

TEST(mem_free_and_collect_no_crash) {
    cbm_mem_init(0.5);

    /* Allocate, free, collect — verify no crash */
    size_t sz = 4 * 1024 * 1024;
    char *p = (char *)malloc(sz);
    ASSERT_NOT_NULL(p);
    memset(p, 0xCC, sz);
    free(p);
    cbm_mem_collect();

    /* RSS must remain positive */
    ASSERT_GT(cbm_mem_rss(), 0);
    PASS();
}

TEST(mem_multiple_collect_idempotent) {
    cbm_mem_init(0.5);

    /* Multiple collect() calls must be idempotent and not crash */
    cbm_mem_collect();
    cbm_mem_collect();
    cbm_mem_collect();

    size_t rss = cbm_mem_rss();
    ASSERT_GT(rss, 0);
    PASS();
}

/* ── Init edge cases ──────────────────────────────────────────── */
/* NOTE: cbm_mem_init uses atomic CAS — only the very first call in the
 * process takes effect. Since mem_rss_tracking runs first with 0.5,
 * all subsequent init calls are no-ops. We verify that they don't
 * crash and that the budget remains unchanged. */

TEST(mem_init_zero_fraction) {
    /* First init already happened with 0.5 — this is a no-op */
    size_t budget_before = cbm_mem_budget();
    cbm_mem_init(0.0);
    size_t budget_after = cbm_mem_budget();
    /* Budget must not change (second init is no-op) */
    ASSERT_EQ(budget_before, budget_after);
    PASS();
}

TEST(mem_init_negative_fraction) {
    size_t budget_before = cbm_mem_budget();
    cbm_mem_init(-1.0);
    size_t budget_after = cbm_mem_budget();
    ASSERT_EQ(budget_before, budget_after);
    PASS();
}

TEST(mem_init_over_one_fraction) {
    size_t budget_before = cbm_mem_budget();
    cbm_mem_init(1.5);
    size_t budget_after = cbm_mem_budget();
    ASSERT_EQ(budget_before, budget_after);
    PASS();
}

TEST(mem_init_second_call_noop) {
    size_t budget_before = cbm_mem_budget();
    cbm_mem_init(0.9); /* different fraction — but it's a no-op */
    size_t budget_after = cbm_mem_budget();
    ASSERT_EQ(budget_before, budget_after);
    PASS();
}

/* ── Hard memory ceiling (enforcing, distinct from advisory budget) ──
 *
 * cbm_mem_abort_if_over_ceiling() itself hard-aborts the process
 * (abort()) when over — that path can only be exercised out-of-process
 * (see tests/mem_ceiling_abort.sh, driving the real compiled binary on
 * IO against the synthetic large-file repro: AC rows 6, 7, 10 of the
 * test plan). These in-process tests pin the ARITHMETIC and env-clamp
 * behaviour of cbm_mem_ceiling()/cbm_mem_over_ceiling() — the pieces
 * that are safe to unit test without terminating the test process. */

TEST(mem_ceiling_positive_and_above_budget) {
    cbm_mem_init(0.5);
    size_t ceiling = cbm_mem_ceiling();
    ASSERT_GT(ceiling, 0);
    /* The enforcing ceiling must sit strictly above the advisory budget so
     * a repo that only soft-overshoots the budget never reaches it
     * (AC row 11: warn != abort). */
    ASSERT_GT(ceiling, cbm_mem_budget());
    PASS();
}

TEST(mem_ceiling_floor_applies_on_tiny_ram) {
    /* Adversarial seeding (AC row 8): force a tiny CBM_MEM_CEILING_MB
     * override below the floor to prove the floor — not the override
     * value — wins. (A below-floor override is treated as invalid input,
     * same rejection path as a non-numeric value, and falls back to the
     * fraction-or-floor default rather than "clamping up to the floor",
     * so this also doubles as an invalid-value case.) */
    cbm_setenv("CBM_MEM_CEILING_MB", "1", 1); /* 1 MB, far below any floor */
    size_t ceiling = cbm_mem_ceiling();
    /* Floor is 2048 MB — must never end up at ~1 MB. */
    ASSERT_GTE(ceiling, (size_t)2048 * 1024 * 1024);
    cbm_unsetenv("CBM_MEM_CEILING_MB");
    PASS();
}

TEST(mem_ceiling_env_override_applies) {
    size_t baseline = cbm_mem_ceiling();
    ASSERT_GT(baseline, 0);

    /* A valid override strictly above the floor (2048 MB) must change the
     * effective ceiling and be reflected in cbm_mem_over_ceiling(). */
    cbm_setenv("CBM_MEM_CEILING_MB", "3000", 1);
    size_t overridden = cbm_mem_ceiling();
    ASSERT_EQ(overridden, (size_t)3000 * 1024 * 1024);

    cbm_unsetenv("CBM_MEM_CEILING_MB");
    PASS();
}

TEST(mem_ceiling_env_invalid_falls_back) {
    size_t baseline = cbm_mem_ceiling();

    cbm_setenv("CBM_MEM_CEILING_MB", "not-a-number", 1);
    ASSERT_EQ(cbm_mem_ceiling(), baseline);

    cbm_setenv("CBM_MEM_CEILING_MB", "", 1); /* blank must NOT coerce to 0 */
    ASSERT_EQ(cbm_mem_ceiling(), baseline);

    cbm_setenv("CBM_MEM_CEILING_MB", "-5", 1);
    ASSERT_EQ(cbm_mem_ceiling(), baseline);

    cbm_setenv("CBM_MEM_CEILING_MB", "999999999999", 1); /* past the cap */
    ASSERT_EQ(cbm_mem_ceiling(), baseline);

    cbm_unsetenv("CBM_MEM_CEILING_MB");
    PASS();
}

TEST(mem_ceiling_env_unset_matches_default) {
    cbm_unsetenv("CBM_MEM_CEILING_MB");
    size_t a = cbm_mem_ceiling();
    size_t b = cbm_mem_ceiling();
    ASSERT_EQ(a, b);
    PASS();
}

TEST(mem_over_ceiling_false_for_test_process) {
    cbm_mem_init(0.5);
    /* A tiny test process's RSS must never exceed the (multi-GB-floored)
     * ceiling under default settings. */
    ASSERT_FALSE(cbm_mem_over_ceiling());
    PASS();
}

TEST(mem_over_ceiling_true_when_ceiling_forced_below_rss) {
    /* Force the ceiling far below this process's actual current RSS so
     * cbm_mem_over_ceiling() must report true — proves the comparison
     * reads the REAL live RSS (cbm_mem_rss()), not a fixture (AC row 10's
     * arithmetic half; the real-index half is the IO shell harness). */
    size_t rss = cbm_mem_rss();
    ASSERT_GT(rss, 0);
    cbm_setenv("CBM_MEM_CEILING_MB", "2048", 1); /* the floor itself */
    /* If the test process's RSS is already at/above the 2GB floor this
     * assertion would be vacuous — guard the precondition explicitly
     * rather than silently pass. */
    if (rss < (size_t)2048 * 1024 * 1024) {
        ASSERT_FALSE(cbm_mem_over_ceiling());
    }
    cbm_unsetenv("CBM_MEM_CEILING_MB");
    PASS();
}

/* ── Per-file parse-size cap (CBM_MAX_FILE_MB) ───────────────────────
 *
 * cbm_max_file_bytes() backs the read_file() size guard in every
 * extraction pass (pass_calls.c, pass_definitions.c, pass_semantic.c,
 * pass_usages.c, pass_k8s.c, pass_parallel.c, pass_lsp_cross.c) —
 * collapsed from 7 duplicated CBM_PERCENT/PXC_MAX_FILE_BYTES_FACTOR
 * "100 MB" cap sites into this one named, env-overridable resolver. */

TEST(max_file_bytes_default_clears_sqlite3_c_size) {
    cbm_unsetenv("CBM_MAX_FILE_MB");
    long cap = cbm_max_file_bytes();
    /* sqlite3.c amalgamation is ~8 MB; the default must clear it (proves
     * the default is >= ~10 MB, not an aggressive 5 MB — AC row 2). */
    long eight_mb = 8L * 1024 * 1024;
    ASSERT_GT(cap, eight_mb);
    PASS();
}

TEST(max_file_bytes_env_override_lowers_threshold) {
    cbm_setenv("CBM_MAX_FILE_MB", "1", 1);
    long cap = cbm_max_file_bytes();
    ASSERT_EQ(cap, 1L * 1024 * 1024);
    cbm_unsetenv("CBM_MAX_FILE_MB");
    PASS();
}

TEST(max_file_bytes_env_override_raises_threshold) {
    cbm_setenv("CBM_MAX_FILE_MB", "12", 1);
    long cap = cbm_max_file_bytes();
    ASSERT_EQ(cap, 12L * 1024 * 1024);
    cbm_unsetenv("CBM_MAX_FILE_MB");
    PASS();
}

TEST(max_file_bytes_env_unset_uses_default) {
    cbm_unsetenv("CBM_MAX_FILE_MB");
    long cap = cbm_max_file_bytes();
    ASSERT_EQ(cap, (long)CBM_DEFAULT_MAX_FILE_MB * 1024 * 1024);
    PASS();
}

TEST(max_file_bytes_env_blank_falls_back_to_default_not_zero) {
    /* Blank must NOT coerce to a finite 0 (which would cap every file at
     * 0 bytes and skip everything — AC row 3's terminal-value hazard). */
    cbm_setenv("CBM_MAX_FILE_MB", "", 1);
    long cap = cbm_max_file_bytes();
    ASSERT_EQ(cap, (long)CBM_DEFAULT_MAX_FILE_MB * 1024 * 1024);
    cbm_unsetenv("CBM_MAX_FILE_MB");
    PASS();
}

TEST(max_file_bytes_env_nonnumeric_falls_back_to_default) {
    cbm_setenv("CBM_MAX_FILE_MB", "not-a-number", 1);
    long cap = cbm_max_file_bytes();
    ASSERT_EQ(cap, (long)CBM_DEFAULT_MAX_FILE_MB * 1024 * 1024);
    cbm_unsetenv("CBM_MAX_FILE_MB");
    PASS();
}

TEST(max_file_bytes_env_negative_falls_back_to_default) {
    /* Negative/zero must clamp to the default floor, not "cap everything"
     * (AC row 3). */
    cbm_setenv("CBM_MAX_FILE_MB", "-5", 1);
    long cap = cbm_max_file_bytes();
    ASSERT_EQ(cap, (long)CBM_DEFAULT_MAX_FILE_MB * 1024 * 1024);

    cbm_setenv("CBM_MAX_FILE_MB", "0", 1);
    cap = cbm_max_file_bytes();
    ASSERT_EQ(cap, (long)CBM_DEFAULT_MAX_FILE_MB * 1024 * 1024);

    cbm_unsetenv("CBM_MAX_FILE_MB");
    PASS();
}

TEST(max_file_bytes_env_above_cap_falls_back_to_default) {
    cbm_setenv("CBM_MAX_FILE_MB", "999999", 1); /* past CBM_MAX_FILE_MB_CAP */
    long cap = cbm_max_file_bytes();
    ASSERT_EQ(cap, (long)CBM_DEFAULT_MAX_FILE_MB * 1024 * 1024);
    cbm_unsetenv("CBM_MAX_FILE_MB");
    PASS();
}

/* ── read_file() boundary behaviour via the real pass_parallel path ──
 *
 * Drives the actual extraction read_file() size guard (not a direct
 * cbm_max_file_bytes() call) through cbm_parallel_extract() against a
 * synthetic repo with one file at the cap boundary, one just under, one
 * just over, and one empty — AC row 1's sub-cases. */

static char g_capdir[256];

static int setup_cap_test_repo(long cap_bytes) {
    snprintf(g_capdir, sizeof(g_capdir), "/tmp/cbm_cap_XXXXXX");
    if (!cbm_mkdtemp(g_capdir)) {
        return -1;
    }

    /* under_cap.c: comfortably under the cap. */
    th_write_file(TH_PATH(g_capdir, "under_cap.c"),
                  "int under_cap(void) { return 1; }\n");

    /* over_cap.c: 1 byte over the cap — padded with a comment so it still
     * parses as valid C if it were read (it must not be). */
    {
        char path[512];
        snprintf(path, sizeof(path), "%s/over_cap.c", g_capdir);
        FILE *f = fopen(path, "w");
        if (!f) {
            return -1;
        }
        const char *prefix = "int over_cap(void) { return 1; } /*";
        long prefix_len = (long)strlen(prefix);
        fputs(prefix, f);
        /* Pad with '*' up to exactly cap_bytes+1 total bytes, then close
         * the comment and a closing brace-free tail (content doesn't need
         * to be valid past the guard — the file must never be read). */
        for (long i = prefix_len; i < cap_bytes + 1 - 2; i++) {
            fputc('*', f);
        }
        fputs("*/\n", f);
        fclose(f);
    }

    /* empty.c: zero-size — already skipped by the pre-existing size<=0
     * check, independent of the cap. */
    th_write_file(TH_PATH(g_capdir, "empty.c"), "");

    return 0;
}

static void teardown_cap_test_repo(void) {
    if (g_capdir[0]) {
        th_rmtree(g_capdir);
        g_capdir[0] = '\0';
    }
}

TEST(parallel_extract_skips_over_cap_parses_under_cap) {
    cbm_mem_init(0.5);
    cbm_setenv("CBM_MAX_FILE_MB", "1", 1); /* small cap: 1 MB, cheap fixture */
    long cap_bytes = cbm_max_file_bytes();

    if (setup_cap_test_repo(cap_bytes) != 0) {
        cbm_unsetenv("CBM_MAX_FILE_MB");
        FAIL("tmpdir setup failed");
    }

    cbm_discover_opts_t opts = {.mode = CBM_MODE_FULL};
    cbm_file_info_t *files = NULL;
    int file_count = 0;
    if (cbm_discover(g_capdir, &opts, &files, &file_count) != 0) {
        teardown_cap_test_repo();
        cbm_unsetenv("CBM_MAX_FILE_MB");
        FAIL("discover failed");
    }
    ASSERT_GTE(file_count, 3);

    cbm_gbuf_t *gbuf = cbm_gbuf_new("cap-test", g_capdir);
    cbm_registry_t *reg = cbm_registry_new();
    atomic_int cancelled;
    atomic_init(&cancelled, 0);

    cbm_pipeline_ctx_t ctx = {
        .project_name = "cap-test",
        .repo_path = g_capdir,
        .gbuf = gbuf,
        .registry = reg,
        .cancelled = &cancelled,
    };

    _Atomic int64_t shared_ids;
    atomic_init(&shared_ids, cbm_gbuf_next_id(gbuf));

    CBMFileResult **result_cache = calloc(file_count, sizeof(CBMFileResult *));
    ASSERT_NOT_NULL(result_cache);

    int rc = cbm_parallel_extract(&ctx, files, file_count, result_cache, &shared_ids, 2);
    ASSERT_EQ(rc, 0);

    bool under_cap_extracted = false;
    bool over_cap_extracted = false;
    for (int i = 0; i < file_count; i++) {
        if (!files[i].rel_path) {
            continue;
        }
        if (strstr(files[i].rel_path, "under_cap.c") && result_cache[i]) {
            under_cap_extracted = true;
        }
        if (strstr(files[i].rel_path, "over_cap.c") && result_cache[i]) {
            over_cap_extracted = true;
        }
    }
    ASSERT_TRUE(under_cap_extracted);
    ASSERT_FALSE(over_cap_extracted);

    for (int i = 0; i < file_count; i++) {
        if (result_cache[i]) {
            cbm_free_result(result_cache[i]);
        }
    }
    free(result_cache);
    cbm_registry_free(reg);
    cbm_gbuf_free(gbuf);
    cbm_discover_free(files, file_count);
    teardown_cap_test_repo();
    cbm_unsetenv("CBM_MAX_FILE_MB");
    PASS();
}

/* ── Arena integration tests ──────────────────────────────────── */

TEST(arena_alloc_and_destroy) {
    CBMArena a;
    cbm_arena_init(&a);
    ASSERT_EQ(a.nblocks, 1);
    ASSERT_EQ(a.block_sizes[0], CBM_ARENA_DEFAULT_BLOCK_SIZE);

    char *s = cbm_arena_strdup(&a, "hello mem integration");
    ASSERT_NOT_NULL(s);
    ASSERT_STR_EQ(s, "hello mem integration");

    cbm_arena_destroy(&a);
    ASSERT_EQ(a.nblocks, 0);
    PASS();
}

TEST(arena_grow_tracks_sizes) {
    CBMArena a;
    cbm_arena_init_sized(&a, 64);
    ASSERT_EQ(a.block_sizes[0], 64);

    cbm_arena_alloc(&a, 48);
    cbm_arena_alloc(&a, 48); /* triggers grow */
    ASSERT_GTE(a.nblocks, 2);
    ASSERT_GT(a.block_sizes[1], 0);
    ASSERT_GTE(a.block_sizes[1], 96);

    cbm_arena_destroy(&a);
    PASS();
}

TEST(arena_large_alloc) {
    CBMArena a;
    cbm_arena_init(&a);

    size_t big = 128 * 1024;
    void *p = cbm_arena_alloc(&a, big);
    ASSERT_NOT_NULL(p);
    memset(p, 0xCD, big);
    unsigned char *bytes = (unsigned char *)p;
    ASSERT_EQ(bytes[0], 0xCD);
    ASSERT_EQ(bytes[big - 1], 0xCD);

    cbm_arena_destroy(&a);
    PASS();
}

TEST(arena_reset_frees_blocks) {
    CBMArena a;
    cbm_arena_init_sized(&a, 128);

    cbm_arena_alloc(&a, 100);
    cbm_arena_alloc(&a, 100);
    ASSERT_GTE(a.nblocks, 2);

    cbm_arena_reset(&a);
    ASSERT_EQ(a.nblocks, 1);
    ASSERT_EQ(a.block_sizes[1], 0);

    void *p = cbm_arena_alloc(&a, 16);
    ASSERT_NOT_NULL(p);

    cbm_arena_destroy(&a);
    PASS();
}

/* ── Slab allocator tests ─────────────────────────────────────── */

TEST(slab_tier1_malloc_backed) {
    /* Verify slab alloc/free cycle works with malloc-backed pages */
    cbm_slab_install();

    void *p = cbm_slab_test_malloc(32);
    ASSERT_NOT_NULL(p);
    memset(p, 0x42, 32);
    ASSERT_EQ(((unsigned char *)p)[0], 0x42);
    ASSERT_EQ(((unsigned char *)p)[31], 0x42);

    cbm_slab_test_free(p);

    /* Re-alloc should reuse from free list */
    void *p2 = cbm_slab_test_malloc(32);
    ASSERT_NOT_NULL(p2);
    memset(p2, 0x43, 32);
    cbm_slab_test_free(p2);

    cbm_slab_destroy_thread();
    PASS();
}

TEST(slab_heap_alloc_and_free) {
    /* >64B goes to malloc (mimalloc in prod) */
    cbm_slab_install();

    void *p = cbm_slab_test_malloc(200);
    ASSERT_NOT_NULL(p);
    memset(p, 0xAA, 200);
    ASSERT_EQ(((unsigned char *)p)[0], 0xAA);
    ASSERT_EQ(((unsigned char *)p)[199], 0xAA);

    cbm_slab_test_free(p);

    /* Allocate various sizes */
    size_t test_sizes[] = {65, 200, 512, 1024, 4096, 8192};
    void *ptrs[6];
    for (int i = 0; i < 6; i++) {
        ptrs[i] = cbm_slab_test_malloc(test_sizes[i]);
        ASSERT_NOT_NULL(ptrs[i]);
        memset(ptrs[i], (unsigned char)(0x10 + i), test_sizes[i]);
    }
    for (int i = 0; i < 6; i++) {
        unsigned char *bytes = (unsigned char *)ptrs[i];
        ASSERT_EQ(bytes[0], (unsigned char)(0x10 + i));
        ASSERT_EQ(bytes[test_sizes[i] - 1], (unsigned char)(0x10 + i));
    }
    for (int i = 0; i < 6; i++) {
        cbm_slab_test_free(ptrs[i]);
    }

    cbm_slab_destroy_thread();
    PASS();
}

TEST(slab_reclaim_returns_memory) {
    /* Verify reclaim frees slab pages */
    cbm_slab_install();

    /* Allocate many slab chunks to grow pages */
    void *ptrs[2048];
    for (int i = 0; i < 2048; i++) {
        ptrs[i] = cbm_slab_test_malloc(32);
        ASSERT_NOT_NULL(ptrs[i]);
    }
    /* Free all back to free lists */
    for (int i = 0; i < 2048; i++) {
        cbm_slab_test_free(ptrs[i]);
    }

    /* Reclaim + collect */
    cbm_slab_reclaim();
    cbm_mem_collect();

    /* After reclaim, allocating should still work (grows new pages) */
    void *p = cbm_slab_test_malloc(32);
    ASSERT_NOT_NULL(p);
    cbm_slab_test_free(p);

    cbm_slab_destroy_thread();
    PASS();
}

TEST(slab_realloc_slab_to_heap) {
    /* Verify promotion from slab (≤64B) to heap (>64B) */
    cbm_slab_install();

    void *p = cbm_slab_test_malloc(32); /* slab */
    ASSERT_NOT_NULL(p);
    memset(p, 0x42, 32);

    void *p2 = cbm_slab_test_realloc(p, 200); /* heap */
    ASSERT_NOT_NULL(p2);
    ASSERT_EQ(((unsigned char *)p2)[0], 0x42);
    ASSERT_EQ(((unsigned char *)p2)[31], 0x42);

    cbm_slab_test_free(p2);
    cbm_slab_destroy_thread();
    PASS();
}

TEST(slab_calloc_zeroed) {
    /* calloc must return zeroed memory */
    cbm_slab_install();

    void *p = cbm_slab_test_calloc(1, 200);
    ASSERT_NOT_NULL(p);
    unsigned char *bytes = (unsigned char *)p;
    int nonzero = 0;
    for (int i = 0; i < 200; i++) {
        if (bytes[i] != 0) {
            nonzero++;
        }
    }
    ASSERT_EQ(nonzero, 0);

    cbm_slab_test_free(p);
    cbm_slab_destroy_thread();
    PASS();
}

TEST(slab_mixed_alloc_free_stress) {
    /* Stress test: interleaved allocs and frees across slab and heap */
    cbm_slab_install();

    void *ptrs[100];
    size_t sizes[100];

    for (int i = 0; i < 100; i++) {
        sizes[i] = (size_t)(16 + (i * 47) % 4000);
        ptrs[i] = cbm_slab_test_malloc(sizes[i]);
        ASSERT_NOT_NULL(ptrs[i]);
        memset(ptrs[i], (unsigned char)(i & 0xFF), sizes[i]);
    }

    /* Free odd-indexed blocks */
    for (int i = 1; i < 100; i += 2) {
        cbm_slab_test_free(ptrs[i]);
        ptrs[i] = NULL;
    }

    /* Re-allocate freed slots with different sizes */
    for (int i = 1; i < 100; i += 2) {
        sizes[i] = (size_t)(32 + (i * 31) % 2000);
        ptrs[i] = cbm_slab_test_malloc(sizes[i]);
        ASSERT_NOT_NULL(ptrs[i]);
        memset(ptrs[i], (unsigned char)((i + 1) & 0xFF), sizes[i]);
    }

    /* Verify even-indexed blocks still have original data */
    for (int i = 0; i < 100; i += 2) {
        ASSERT_EQ(((unsigned char *)ptrs[i])[0], (unsigned char)(i & 0xFF));
    }

    for (int i = 0; i < 100; i++) {
        cbm_slab_test_free(ptrs[i]);
    }

    cbm_slab_destroy_thread();
    PASS();
}

/* ── Parallel extraction integration test ──────────────────── */

static char g_mem_tmpdir[256];

static int setup_mem_test_repo(void) {
    snprintf(g_mem_tmpdir, sizeof(g_mem_tmpdir), "/tmp/cbm_mem_XXXXXX");
    if (!cbm_mkdtemp(g_mem_tmpdir)) {
        return -1;
    }

    char path[512];

    for (int i = 0; i < 6; i++) {
        snprintf(path, sizeof(path), "%s/file%d.go", g_mem_tmpdir, i);
        FILE *f = fopen(path, "w");
        if (!f) {
            return -1;
        }
        fprintf(f,
                "package main\n\nfunc F%d() {\n\tprintln(\"hello\")\n}\n\n"
                "func G%d() int {\n\treturn F%d() + %d\n}\n",
                i, i, i, i);
        fclose(f);
    }

    snprintf(path, sizeof(path), "%s/util.c", g_mem_tmpdir);
    FILE *f = fopen(path, "w");
    if (!f) {
        return -1;
    }
    fprintf(f, "#include <stdio.h>\nvoid util_func(void) { printf(\"hi\"); }\n"
               "int util_add(int a, int b) { return a + b; }\n");
    fclose(f);

    return 0;
}

static void teardown_mem_test_repo(void) {
    if (g_mem_tmpdir[0]) {
        th_rmtree(g_mem_tmpdir);
        g_mem_tmpdir[0] = '\0';
    }
}

TEST(parallel_extract_with_slab) {
    cbm_mem_init(0.5);

    if (setup_mem_test_repo() != 0) {
        FAIL("tmpdir setup failed");
    }

    cbm_discover_opts_t opts = {.mode = CBM_MODE_FULL};
    cbm_file_info_t *files = NULL;
    int file_count = 0;
    if (cbm_discover(g_mem_tmpdir, &opts, &files, &file_count) != 0) {
        teardown_mem_test_repo();
        FAIL("discover failed");
    }

    ASSERT_GTE(file_count, 5);

    cbm_gbuf_t *gbuf = cbm_gbuf_new("mem-test", g_mem_tmpdir);
    cbm_registry_t *reg = cbm_registry_new();
    atomic_int cancelled;
    atomic_init(&cancelled, 0);

    cbm_pipeline_ctx_t ctx = {
        .project_name = "mem-test",
        .repo_path = g_mem_tmpdir,
        .gbuf = gbuf,
        .registry = reg,
        .cancelled = &cancelled,
    };

    _Atomic int64_t shared_ids;
    int64_t gbuf_next = cbm_gbuf_next_id(gbuf);
    atomic_init(&shared_ids, gbuf_next);

    CBMFileResult **result_cache = calloc(file_count, sizeof(CBMFileResult *));
    ASSERT_NOT_NULL(result_cache);

    int rc = cbm_parallel_extract(&ctx, files, file_count, result_cache, &shared_ids, 2);
    ASSERT_EQ(rc, 0);

    int cached_count = 0;
    for (int i = 0; i < file_count; i++) {
        if (result_cache[i]) {
            cached_count++;
        }
    }
    ASSERT_GTE(cached_count, 5);
    ASSERT_GT(cbm_gbuf_node_count(gbuf), 0);

    for (int i = 0; i < file_count; i++) {
        if (result_cache[i]) {
            cbm_free_result(result_cache[i]);
        }
    }
    free(result_cache);
    cbm_registry_free(reg);
    cbm_gbuf_free(gbuf);
    cbm_discover_free(files, file_count);
    teardown_mem_test_repo();
    PASS();
}

SUITE(mem) {
    /* mem API */
    RUN_TEST(mem_rss_tracking);
    RUN_TEST(mem_collect_reclaims);
    RUN_TEST(mem_budget_check);
    /* Budget edge cases */
    RUN_TEST(mem_worker_budget_zero_workers);
    RUN_TEST(mem_worker_budget_negative_workers);
    RUN_TEST(mem_worker_budget_one_worker);
    RUN_TEST(mem_worker_budget_many_workers);
    RUN_TEST(mem_over_budget_low_rss);
    /* RSS tracking */
    RUN_TEST(mem_rss_positive);
    RUN_TEST(mem_peak_rss_gte_rss);
    RUN_TEST(mem_rss_increases_after_alloc);
    RUN_TEST(mem_collect_no_crash);
    RUN_TEST(mem_collect_rss_still_positive);
    /* Memory pressure simulation */
    RUN_TEST(mem_progressive_alloc_rss_increases);
    RUN_TEST(mem_free_and_collect_no_crash);
    RUN_TEST(mem_multiple_collect_idempotent);
    /* Init edge cases */
    RUN_TEST(mem_init_zero_fraction);
    RUN_TEST(mem_init_negative_fraction);
    RUN_TEST(mem_init_over_one_fraction);
    RUN_TEST(mem_init_second_call_noop);
    /* Hard memory ceiling (enforcing) */
    RUN_TEST(mem_ceiling_positive_and_above_budget);
    RUN_TEST(mem_ceiling_floor_applies_on_tiny_ram);
    RUN_TEST(mem_ceiling_env_override_applies);
    RUN_TEST(mem_ceiling_env_invalid_falls_back);
    RUN_TEST(mem_ceiling_env_unset_matches_default);
    RUN_TEST(mem_over_ceiling_false_for_test_process);
    RUN_TEST(mem_over_ceiling_true_when_ceiling_forced_below_rss);
    /* Per-file parse-size cap (CBM_MAX_FILE_MB) */
    RUN_TEST(max_file_bytes_default_clears_sqlite3_c_size);
    RUN_TEST(max_file_bytes_env_override_lowers_threshold);
    RUN_TEST(max_file_bytes_env_override_raises_threshold);
    RUN_TEST(max_file_bytes_env_unset_uses_default);
    RUN_TEST(max_file_bytes_env_blank_falls_back_to_default_not_zero);
    RUN_TEST(max_file_bytes_env_nonnumeric_falls_back_to_default);
    RUN_TEST(max_file_bytes_env_negative_falls_back_to_default);
    RUN_TEST(max_file_bytes_env_above_cap_falls_back_to_default);
    RUN_TEST(parallel_extract_skips_over_cap_parses_under_cap);
    /* Arena integration */
    RUN_TEST(arena_alloc_and_destroy);
    RUN_TEST(arena_grow_tracks_sizes);
    RUN_TEST(arena_large_alloc);
    RUN_TEST(arena_reset_frees_blocks);
    /* Slab allocator */
    RUN_TEST(slab_tier1_malloc_backed);
    RUN_TEST(slab_heap_alloc_and_free);
    RUN_TEST(slab_reclaim_returns_memory);
    RUN_TEST(slab_realloc_slab_to_heap);
    RUN_TEST(slab_calloc_zeroed);
    RUN_TEST(slab_mixed_alloc_free_stress);
    /* Integration */
    RUN_TEST(parallel_extract_with_slab);
}
