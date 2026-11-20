/* Counts blocks and traces DynamoRIO builds, split by file-backed module against other memory. */
#include "dr_api.h"
static int bb_mod, bb_anon, tr, xl, app_instrs_mod, app_instrs_anon;
static void *lock;
static dr_emit_flags_t bb(void *dc, void *tag, instrlist_t *il, bool for_trace, bool translating) {
    if (translating) { dr_mutex_lock(lock); xl++; dr_mutex_unlock(lock); return DR_EMIT_DEFAULT; }
    if (for_trace) return DR_EMIT_DEFAULT;
    int n = 0;
    for (instr_t *in = instrlist_first_app(il); in != NULL; in = instr_get_next_app(in)) n++;
    module_data_t *m = dr_lookup_module((byte *)tag);
    dr_mutex_lock(lock);
    if (m != NULL) { bb_mod++; app_instrs_mod += n; } else { bb_anon++; app_instrs_anon += n; }
    dr_mutex_unlock(lock);
    if (m != NULL) dr_free_module_data(m);
    return DR_EMIT_DEFAULT;
}
static dr_emit_flags_t trace(void *dc, void *tag, instrlist_t *il, bool translating) {
    if (!translating) { dr_mutex_lock(lock); tr++; dr_mutex_unlock(lock); }
    return DR_EMIT_DEFAULT;
}
static void ex(void) {
    dr_fprintf(STDERR, "COUNT bb_module=%d bb_other=%d instr_module=%d instr_other=%d traces=%d retranslations=%d\n",
               bb_mod, bb_anon, app_instrs_mod, app_instrs_anon, tr, xl);
    dr_mutex_destroy(lock);
}
DR_EXPORT void dr_client_main(client_id_t id, int argc, const char *argv[]) {
    lock = dr_mutex_create();
    dr_register_bb_event(bb);
    dr_register_trace_event(trace);
    dr_register_exit_event(ex);
}
