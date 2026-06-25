/*
 * aka_engine.h — Stable embedded API for Rust/Tauri hosts.
 *
 * This public header intentionally exposes only the facts callback ABI. Internal
 * pipeline/store types remain private so the C implementation can evolve
 * without forcing Rust bindings to track engine internals.
 */
#ifndef AKA_ENGINE_API_H
#define AKA_ENGINE_API_H

#include <stdbool.h>
#include <stdint.h>

#if defined(_WIN32) && defined(AKA_ENGINE_BUILD_DLL)
#define AKA_ENGINE_API __declspec(dllexport)
#else
#define AKA_ENGINE_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AKA_ENGINE_MODE_FULL = 0,
    AKA_ENGINE_MODE_MODERATE = 1,
    AKA_ENGINE_MODE_FAST = 2,
} aka_engine_mode_t;

typedef enum {
    AKA_ENGINE_LOG_DEBUG = 0,
    AKA_ENGINE_LOG_INFO = 1,
    AKA_ENGINE_LOG_WARN = 2,
    AKA_ENGINE_LOG_ERROR = 3,
} aka_engine_log_level_t;

typedef enum {
    AKA_ENGINE_OK = 0,
    AKA_ENGINE_ERROR = -1,
    AKA_ENGINE_CANCELLED = -2,
} aka_engine_status_t;

typedef struct {
    const char *repo_path;
    const char *cache_dir;
    aka_engine_mode_t mode;
    bool direct_facts_only;
    uint64_t deadline_ms_monotonic; /* 0 disables; compared with engine monotonic clock */
    uint64_t max_indexing_time_ms;  /* 0 disables; relative budget from call start */
} aka_engine_index_options_t;

typedef struct {
    void *userdata;
    int (*progress)(void *userdata, const char *phase, const char *file_path, int current,
                    int total, int nodes, int edges);
    void (*log)(void *userdata, aka_engine_log_level_t level, const char *line);
    int (*should_cancel)(void *userdata);
} aka_engine_callbacks_t;

typedef struct {
    void *userdata;
    int (*manifest)(void *userdata, const char *repo_path, int files, int nodes, int edges);
    int (*node)(void *userdata, int64_t cbm_id, const char *label, const char *name,
                const char *qualified_name, const char *file_path, int start_line_0based,
                int end_line_0based, const char *properties_json);
    int (*edge)(void *userdata, int64_t edge_id, int64_t source_id, const char *source_qn,
                int64_t target_id, const char *target_qn, const char *type, double confidence,
                const char *reason, int has_step, int64_t step, const char *evidence_json);
    int (*done)(void *userdata, int files, int nodes, int edges);
    const aka_engine_callbacks_t *callbacks;
} aka_engine_fact_sink_t;

/* Returns AKA_ENGINE_OK on success, AKA_ENGINE_CANCELLED when callbacks or
 * deadlines request cooperative cancellation, or AKA_ENGINE_ERROR on failure.
 *
 * ABI note: new fields are appended to aka_engine_index_options_t and
 * aka_engine_fact_sink_t; existing field order is unchanged. Rust bindings must
 * sync the enlarged structs before passing non-NULL callbacks/deadlines.
 * Progress and cancellation callbacks may be called from indexing worker
 * threads; hosts should keep them thread-safe and non-blocking.
 *
 * The fact sink and optional callbacks are borrowed for the duration of the
 * call. If direct_facts_only is true, the engine emits facts but skips its
 * SQLite dump/persistence; Rust owns graph/search persistence. */
AKA_ENGINE_API int aka_engine_index_with_sink(const aka_engine_index_options_t *options,
                                              const aka_engine_fact_sink_t *sink);

/* Monotonic milliseconds using the same clock as deadline_ms_monotonic. */
AKA_ENGINE_API uint64_t aka_engine_monotonic_ms(void);

#ifdef __cplusplus
}
#endif

#endif /* AKA_ENGINE_API_H */
