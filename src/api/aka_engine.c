/*
 * aka_engine.c — Stable embedded API wrapper.
 */
#include "api/aka_engine.h"

#include "cbm.h"
#include "foundation/compat.h"
#include "foundation/constants.h"
#include "foundation/log.h"
#include "foundation/mem.h"
#include "foundation/profile.h"
#include "foundation/compat_fs.h"
#include "foundation/compat_thread.h"
#include "pipeline/pipeline.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

typedef struct {
    const aka_engine_fact_sink_t *outer;
} sink_bridge_t;

uint64_t aka_engine_monotonic_ms(void) {
    struct timespec now;
    cbm_clock_gettime(CLOCK_MONOTONIC, &now);
    return ((uint64_t)now.tv_sec * CBM_MSEC_PER_SEC) +
           ((uint64_t)now.tv_nsec / CBM_NSEC_PER_MSEC);
}

static aka_engine_log_level_t to_api_log_level(CBMLogLevel level) {
    switch (level) {
    case CBM_LOG_DEBUG:
        return AKA_ENGINE_LOG_DEBUG;
    case CBM_LOG_INFO:
        return AKA_ENGINE_LOG_INFO;
    case CBM_LOG_WARN:
        return AKA_ENGINE_LOG_WARN;
    case CBM_LOG_ERROR:
    default:
        return AKA_ENGINE_LOG_ERROR;
    }
}

static void bridge_log(CBMLogLevel level, const char *line, void *userdata) {
    const aka_engine_callbacks_t *callbacks = userdata;
    if (callbacks && callbacks->log) {
        callbacks->log(callbacks->userdata, to_api_log_level(level), line ? line : "");
    }
}

static int bridge_progress(void *userdata, const char *phase, const char *file_path, int current,
                           int total, int nodes, int edges) {
    const aka_engine_callbacks_t *callbacks = userdata;
    if (!callbacks || !callbacks->progress) {
        return 0;
    }
    return callbacks->progress(callbacks->userdata, phase, file_path, current, total, nodes, edges);
}

static int bridge_should_cancel(void *userdata) {
    const aka_engine_callbacks_t *callbacks = userdata;
    if (!callbacks || !callbacks->should_cancel) {
        return 0;
    }
    return callbacks->should_cancel(callbacks->userdata);
}

static cbm_index_mode_t to_cbm_mode(aka_engine_mode_t mode) {
    switch (mode) {
    case AKA_ENGINE_MODE_FULL:
        return CBM_MODE_FULL;
    case AKA_ENGINE_MODE_MODERATE:
        return CBM_MODE_MODERATE;
    case AKA_ENGINE_MODE_FAST:
        return CBM_MODE_FAST;
    default:
        return CBM_MODE_FAST;
    }
}

static int bridge_manifest(void *userdata, const char *repo_path, int files, int nodes,
                           int edges) {
    sink_bridge_t *bridge = userdata;
    if (!bridge || !bridge->outer || !bridge->outer->manifest) {
        return 0;
    }
    return bridge->outer->manifest(bridge->outer->userdata, repo_path, files, nodes, edges);
}

static int bridge_node(void *userdata, int64_t cbm_id, const char *label, const char *name,
                       const char *qualified_name, const char *file_path,
                       int start_line_0based, int end_line_0based, const char *properties_json) {
    sink_bridge_t *bridge = userdata;
    if (!bridge || !bridge->outer || !bridge->outer->node) {
        return 0;
    }
    return bridge->outer->node(bridge->outer->userdata, cbm_id, label, name, qualified_name,
                               file_path, start_line_0based, end_line_0based, properties_json);
}

static int bridge_edge(void *userdata, int64_t edge_id, int64_t source_id, const char *source_qn,
                       int64_t target_id, const char *target_qn, const char *type,
                       double confidence, const char *reason, int has_step, int64_t step,
                       const char *evidence_json) {
    sink_bridge_t *bridge = userdata;
    if (!bridge || !bridge->outer || !bridge->outer->edge) {
        return 0;
    }
    return bridge->outer->edge(bridge->outer->userdata, edge_id, source_id, source_qn, target_id,
                               target_qn, type, confidence, reason, has_step, step,
                               evidence_json);
}

static int bridge_done(void *userdata, int files, int nodes, int edges) {
    sink_bridge_t *bridge = userdata;
    if (!bridge || !bridge->outer || !bridge->outer->done) {
        return 0;
    }
    return bridge->outer->done(bridge->outer->userdata, files, nodes, edges);
}

static int set_cache_dir(const char *cache_dir) {
    if (!cache_dir || !cache_dir[0]) {
        return 0;
    }
    return cbm_setenv("AKA_ENGINE_CACHE_DIR", cache_dir, 1);
}

static char *effective_db_path(const aka_engine_index_options_t *options) {
    if (options->db_path && options->db_path[0]) {
        return strdup(options->db_path);
    }
    if (!options->cache_dir || !options->cache_dir[0]) {
        return NULL;
    }
    cbm_mkdir_p(options->cache_dir, 0755);
    size_t len = strlen(options->cache_dir) + strlen("/facts.db") + 1;
    char *path = malloc(len);
    if (!path) {
        return NULL;
    }
    snprintf(path, len, "%s/facts.db", options->cache_dir);
    return path;
}

static void restore_log_sink(cbm_log_sink_fn legacy, cbm_log_sink_ex_fn extended,
                             void *userdata) {
    if (extended) {
        cbm_log_set_sink_ex(extended, userdata);
    } else {
        cbm_log_set_sink(legacy);
    }
}

static int api_should_cancel(const cbm_pipeline_callbacks_t *callbacks) {
    if (!callbacks) {
        return 0;
    }
    if (callbacks->deadline_ms_monotonic > 0 &&
        aka_engine_monotonic_ms() >= callbacks->deadline_ms_monotonic) {
        return CBM_CANCELLED;
    }
    if (callbacks->should_cancel && callbacks->should_cancel(callbacks->userdata) != 0) {
        return CBM_CANCELLED;
    }
    return 0;
}

static int pipeline_lock_cooperative(const cbm_pipeline_callbacks_t *callbacks) {
    struct timespec wait = {0, 100000000};
    while (!cbm_pipeline_try_lock()) {
        int cancel_rc = api_should_cancel(callbacks);
        if (cancel_rc != 0) {
            return cancel_rc;
        }
        cbm_nanosleep(&wait, NULL);
    }
    return 0;
}

int aka_engine_index_with_sink(const aka_engine_index_options_t *options,
                               const aka_engine_fact_sink_t *sink) {
    if (!options || !options->repo_path || !options->repo_path[0] || !sink) {
        return AKA_ENGINE_ERROR;
    }
    if (set_cache_dir(options->cache_dir) != 0) {
        return AKA_ENGINE_ERROR;
    }

    cbm_alloc_init();
    cbm_profile_init();
    cbm_log_init_from_env();
    cbm_mem_init(0.5);

    const aka_engine_callbacks_t *callbacks = sink->callbacks;
    char *db_path = effective_db_path(options);
    cbm_pipeline_t *pipeline =
        cbm_pipeline_new(options->repo_path, db_path, to_cbm_mode(options->mode));
    free(db_path);
    if (!pipeline) {
        return AKA_ENGINE_ERROR;
    }

    sink_bridge_t bridge = {.outer = sink};
    cbm_fact_sink_t inner = {.userdata = &bridge,
                             .manifest = bridge_manifest,
                             .node = bridge_node,
                             .edge = bridge_edge,
                             .done = bridge_done};
    cbm_pipeline_set_fact_sink(pipeline, &inner);
    cbm_pipeline_set_skip_dump(pipeline, options->direct_facts_only);
    cbm_pipeline_set_baseline_facts_only(pipeline, options->baseline_facts_only);
    cbm_pipeline_callbacks_t pipeline_callbacks = {
        .userdata = (void *)callbacks,
        .progress = callbacks && callbacks->progress ? bridge_progress : NULL,
        .should_cancel = callbacks && callbacks->should_cancel ? bridge_should_cancel : NULL,
        .deadline_ms_monotonic = options->deadline_ms_monotonic,
    };
    if (options->max_indexing_time_ms > 0) {
        uint64_t relative_deadline = aka_engine_monotonic_ms() + options->max_indexing_time_ms;
        if (pipeline_callbacks.deadline_ms_monotonic == 0 ||
            relative_deadline < pipeline_callbacks.deadline_ms_monotonic) {
            pipeline_callbacks.deadline_ms_monotonic = relative_deadline;
        }
    }
    if (callbacks || pipeline_callbacks.deadline_ms_monotonic > 0) {
        cbm_pipeline_set_callbacks(pipeline, &pipeline_callbacks);
    }

    int lock_rc = pipeline_lock_cooperative(&pipeline_callbacks);
    if (lock_rc != 0) {
        cbm_pipeline_free(pipeline);
        cbm_mem_collect();
        return AKA_ENGINE_CANCELLED;
    }
    cbm_log_sink_fn prev_log_sink = NULL;
    cbm_log_sink_ex_fn prev_log_sink_ex = NULL;
    void *prev_log_userdata = NULL;
    cbm_log_get_sink_state(&prev_log_sink, &prev_log_sink_ex, &prev_log_userdata);
    if (callbacks && callbacks->log) {
        cbm_log_set_sink_ex(bridge_log, (void *)callbacks);
    }
    int rc = cbm_pipeline_run(pipeline);
    restore_log_sink(prev_log_sink, prev_log_sink_ex, prev_log_userdata);
    cbm_pipeline_unlock();
    cbm_pipeline_free(pipeline);
    cbm_mem_collect();
    if (rc == CBM_CANCELLED) {
        return AKA_ENGINE_CANCELLED;
    }
    if (rc != 0) {
        return AKA_ENGINE_ERROR;
    }
    return AKA_ENGINE_OK;
}
