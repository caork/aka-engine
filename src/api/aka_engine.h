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

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AKA_ENGINE_MODE_FULL = 0,
    AKA_ENGINE_MODE_MODERATE = 1,
    AKA_ENGINE_MODE_FAST = 2,
} aka_engine_mode_t;

typedef struct {
    const char *repo_path;
    const char *cache_dir;
    aka_engine_mode_t mode;
    bool direct_facts_only;
} aka_engine_index_options_t;

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
} aka_engine_fact_sink_t;

/* Returns 0 on success, nonzero on failure. The fact sink is borrowed for the
 * duration of the call. If direct_facts_only is true, the engine emits facts but
 * skips its SQLite dump/persistence; Rust owns graph/search persistence. */
int aka_engine_index_with_sink(const aka_engine_index_options_t *options,
                               const aka_engine_fact_sink_t *sink);

#ifdef __cplusplus
}
#endif

#endif /* AKA_ENGINE_API_H */
