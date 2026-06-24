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
#include "pipeline/pipeline.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    const aka_engine_fact_sink_t *outer;
} sink_bridge_t;

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

int aka_engine_index_with_sink(const aka_engine_index_options_t *options,
                               const aka_engine_fact_sink_t *sink) {
    if (!options || !options->repo_path || !options->repo_path[0] || !sink) {
        return CBM_NOT_FOUND;
    }
    if (set_cache_dir(options->cache_dir) != 0) {
        return CBM_NOT_FOUND;
    }

    cbm_alloc_init();
    cbm_profile_init();
    cbm_log_init_from_env();
    cbm_mem_init(0.5);

    cbm_pipeline_t *pipeline =
        cbm_pipeline_new(options->repo_path, NULL, to_cbm_mode(options->mode));
    if (!pipeline) {
        return CBM_NOT_FOUND;
    }

    sink_bridge_t bridge = {.outer = sink};
    cbm_fact_sink_t inner = {.userdata = &bridge,
                             .manifest = bridge_manifest,
                             .node = bridge_node,
                             .edge = bridge_edge,
                             .done = bridge_done};
    cbm_pipeline_set_fact_sink(pipeline, &inner);
    cbm_pipeline_set_skip_dump(pipeline, options->direct_facts_only);

    cbm_pipeline_lock();
    int rc = cbm_pipeline_run(pipeline);
    cbm_pipeline_unlock();
    cbm_pipeline_free(pipeline);
    cbm_mem_collect();
    return rc;
}
