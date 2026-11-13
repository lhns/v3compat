/* A client with an empty block event: what DynamoRIO costs and does with any client. */
#include "dr_api.h"
static bool store;
static dr_emit_flags_t bb(void *dc, void *tag, instrlist_t *ilist, bool for_trace, bool translating) {
    (void)dc; (void)tag; (void)ilist; (void)for_trace; (void)translating;
    return store ? DR_EMIT_STORE_TRANSLATIONS : DR_EMIT_DEFAULT;
}
DR_EXPORT void dr_client_main(client_id_t id, int argc, const char *argv[]) {
    (void)id;
    store = argc > 1 && argv[1][0] == '-' && argv[1][1] == 's';
    dr_register_bb_event(bb);
}
