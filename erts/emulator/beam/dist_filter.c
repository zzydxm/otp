#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "sys.h"
#include "global.h"
#include "atom.h"
#include "dist.h"
#include "external.h"
#include "erl_binary.h"
#include "erl_process.h"
#include "dist_filter.h"
#include "dist_filter_cursor.h"

typedef enum {
    DIST_FILTER_INVALID = 0,
    DIST_FILTER_DROP,
    DIST_FILTER_EMIT,
    DIST_FILTER_INSPECT,
    DIST_FILTER_REDIRECT
} DistFilterAction;

Eterm dist_filter_spawn_module;
Eterm dist_filter_spawn_fun;

static int enabled;
static Eterm drop_control_heap[5];
static Eterm drop_control;
static Eterm atom_rex;
static Eterm atom_gen_call;
static Eterm atom_gen_cast;
static Eterm atom_try_again_restart;
static Eterm atom_start_child;
static Eterm atom_terminate_child;
static Eterm atom_restart_child;
static Eterm atom_delete_child;
static Eterm atom_apply_once;
static Eterm atom_apply_interval;
static Eterm atom_apply_repeatedly;
static Eterm atom_dict_insert;
static Eterm atom_filter;
static Eterm atom_remote;
static Eterm atom_load_compiled;
static Eterm atom_job;
static Eterm atom_io_request;
static Eterm atom_io_reply;
static Eterm atom_is_auth;
static Eterm atom_features_request;
static Eterm atom_features_reply;
static Eterm atom_dist_filter;
static Eterm atom_req;

void
dist_filter_init(void)
{
    char value[8] = {0};
    size_t value_size = sizeof(value);

#define INIT_ATOM(Var, Name)                                               \
    (Var) = erts_atom_put((const byte *)(Name), sizeof(Name) - 1,          \
                          ERTS_ATOM_ENC_LATIN1, 1)

    dist_filter_spawn_module = am_erts_internal;
    dist_filter_spawn_fun = am_dist_spawn_init;
    enabled =
        erts_sys_explicit_8bit_getenv("DIST_FILTER_ENABLED",
                                      value,
                                      &value_size) == 1
        && value[0] == '1' && value[1] == '\0';
    if (!enabled) {
        return;
    }

    drop_control =
        TUPLE4(drop_control_heap,
               make_small(DOP_REG_SEND),
               make_internal_pid(0),
               am_Empty,
               am_undefined);

    INIT_ATOM(atom_rex, "rex");
    INIT_ATOM(atom_gen_call, "$gen_call");
    INIT_ATOM(atom_gen_cast, "$gen_cast");
    INIT_ATOM(atom_try_again_restart, "try_again_restart");
    INIT_ATOM(atom_start_child, "start_child");
    INIT_ATOM(atom_terminate_child, "terminate_child");
    INIT_ATOM(atom_restart_child, "restart_child");
    INIT_ATOM(atom_delete_child, "delete_child");
    INIT_ATOM(atom_apply_once, "apply_once");
    INIT_ATOM(atom_apply_interval, "apply_interval");
    INIT_ATOM(atom_apply_repeatedly, "apply_repeatedly");
    INIT_ATOM(atom_dict_insert, "dict_insert");
    INIT_ATOM(atom_filter, "filter");
    INIT_ATOM(atom_remote, "remote");
    INIT_ATOM(atom_load_compiled, "load_compiled");
    INIT_ATOM(atom_job, "job");
    INIT_ATOM(atom_io_request, "io_request");
    INIT_ATOM(atom_io_reply, "io_reply");
    INIT_ATOM(atom_is_auth, "is_auth");
    INIT_ATOM(atom_features_request, "features_request");
    INIT_ATOM(atom_features_reply, "features_reply");
    INIT_ATOM(atom_dist_filter, "dist_filter");
    INIT_ATOM(atom_req, "req");

    dist_filter_spawn_module = atom_dist_filter;
    dist_filter_spawn_fun = atom_req;

#undef INIT_ATOM
}

static int
get_altact_flags(Eterm term, byte *flagsp)
{
    if (is_small(term)) {
        Sint value = signed_val(term);
        if (value < 0) {
            return 0;
        }
        *flagsp = (byte)(value & 0xff);
        return 1;
    }
    if (is_big(term)) {
        Eterm *value = big_val(term);
        if (BIG_SIGN(value)) {
            return 0;
        }
        *flagsp = (byte)(BIG_V(value)[0] & 0xff);
        return 1;
    }
    return 0;
}

/*
 * INSPECT send operations, DROP link/exit/group-leader operations,
 * REDIRECT spawn requests, and EMIT unlink/monitor/spawn replies.
 */
static DistFilterAction
classify_control(Sint type, Eterm control, Eterm *targetp)
{
    Eterm *tuple = tuple_val(control);

    *targetp = THE_NON_VALUE;
    switch (type) {
    case DOP_SEND:
    case DOP_SEND_TT:
    case DOP_SEND_SENDER:
    case DOP_SEND_SENDER_TT:
    case DOP_ALIAS_SEND:
    case DOP_ALIAS_SEND_TT:
        return DIST_FILTER_INSPECT;
    case DOP_REG_SEND:
        if (arityval(tuple[0]) != 4 || !is_atom(tuple[4])) {
            return DIST_FILTER_INVALID;
        }
        *targetp = tuple[4];
        return DIST_FILTER_INSPECT;
    case DOP_REG_SEND_TT:
        if (arityval(tuple[0]) != 5 || !is_atom(tuple[4])) {
            return DIST_FILTER_INVALID;
        }
        *targetp = tuple[4];
        return DIST_FILTER_INSPECT;
    case DOP_ALTACT_SIG_SEND: {
        byte flags;
        if ((arityval(tuple[0]) != 4 && arityval(tuple[0]) != 5)
            || !get_altact_flags(tuple[2], &flags)) {
            return DIST_FILTER_INVALID;
        }
        if (flags & ERTS_DOP_ALTACT_SIG_FLG_EXIT) {
            return DIST_FILTER_DROP;
        }
        if (flags & ERTS_DOP_ALTACT_SIG_FLG_NAME) {
            if (!is_atom(tuple[4])) {
                return DIST_FILTER_INVALID;
            }
            *targetp = tuple[4];
        }
        return DIST_FILTER_INSPECT;
    }
    case DOP_LINK:
    case DOP_EXIT:
    case DOP_GROUP_LEADER:
    case DOP_EXIT2:
    case DOP_EXIT_TT:
    case DOP_EXIT2_TT:
    case DOP_PAYLOAD_EXIT:
    case DOP_PAYLOAD_EXIT_TT:
    case DOP_PAYLOAD_EXIT2:
    case DOP_PAYLOAD_EXIT2_TT:
        return DIST_FILTER_DROP;
    case DOP_UNLINK:
    case DOP_MONITOR_P:
    case DOP_DEMONITOR_P:
    case DOP_MONITOR_P_EXIT:
    case DOP_PAYLOAD_MONITOR_P_EXIT:
    case DOP_SPAWN_REPLY:
    case DOP_SPAWN_REPLY_TT:
    case DOP_UNLINK_ID:
    case DOP_UNLINK_ID_ACK:
        return DIST_FILTER_EMIT;
    case DOP_SPAWN_REQUEST:
    case DOP_SPAWN_REQUEST_TT:
        return DIST_FILTER_REDIRECT;
    default:
        return DIST_FILTER_INVALID;
    }
}

static int
cursor_is_mfa(DistFilterCursor *cursor)
{
    Uint32 arity;

    return cursor_read_tuple(cursor, &arity) && arity == 3
        && cursor_skip_atom(cursor)
        && cursor_skip_atom(cursor)
        && cursor_is_list(cursor);
}

/*
 * DROP timer apply requests:
 *   apply_once: {Integer, Integer, {Module, Function, Arguments}}
 *   otherwise:  {Integer, Integer, _, {Module, Function, Arguments}}
 */
static DistFilterAction
classify_apply(DistFilterCursor *cursor, Eterm action)
{
    Uint32 arity;
    Uint32 want = action == atom_apply_once ? 3 : 4;

    if (!cursor_read_tuple(cursor, &arity) || arity != want
        || !cursor_skip_integer(cursor)
        || !cursor_skip_integer(cursor)) {
        return DIST_FILTER_EMIT;
    }
    if (want == 4 && !cursor_skip_terms(cursor, 1)) {
        return DIST_FILTER_EMIT;
    }
    return cursor_is_mfa(cursor) ? DIST_FILTER_DROP : DIST_FILTER_EMIT;
}

/*
 * DROP gen-call requests:
 *   {start_child | terminate_child | restart_child | delete_child, _}
 *   {spawn, Pid, Module, Function, Arguments}
 *   {apply_once | apply_interval | apply_repeatedly, ApplyRequest}
 *   {dict_insert, {filter, Name}, Fun}
 */
static DistFilterAction
classify_gen_call(DistFilterCursor *cursor)
{
    Eterm action;
    Uint32 arity;

    if (!cursor_read_tuple(cursor, &arity) || arity == 0
        || !cursor_read_atom(cursor, &action)) {
        return DIST_FILTER_EMIT;
    }

    if (arity == 2
        && (action == atom_start_child
            || action == atom_terminate_child
            || action == atom_restart_child
            || action == atom_delete_child)) {
        return DIST_FILTER_DROP;
    }

    if (arity == 5 && action == am_spawn
        && cursor_skip_pid(cursor)
        && cursor_skip_atom(cursor)
        && cursor_skip_atom(cursor)
        && cursor_is_list(cursor)) {
        return DIST_FILTER_DROP;
    }

    if (arity == 2
        && (action == atom_apply_once
            || action == atom_apply_interval
            || action == atom_apply_repeatedly)) {
        return classify_apply(cursor, action);
    }

    if (arity == 3 && action == atom_dict_insert) {
        Eterm key_tag;
        Uint32 key_arity;

        if (cursor_read_tuple(cursor, &key_arity)
            && key_arity == 2
            && cursor_read_atom(cursor, &key_tag)
            && key_tag == atom_filter
            && cursor_skip_atom(cursor)
            && cursor_is_fun(cursor)) {
            return DIST_FILTER_DROP;
        }
    }

    return DIST_FILTER_EMIT;
}

/*
 * DROP send payloads:
 *   {'$gen_cast', {try_again_restart, _}}
 *   {system | 'EXIT' | io_reply, _, _}
 *   {'$gen_call', _, Request} when Request matches a dropped gen-call
 *   {remote, load_compiled, _}
 *   {command, Pid, {start, {job, Pid, _, Arguments}}}
 *   {io_request, _, _, _}
 */
static DistFilterAction
classify_send(DistFilterCursor *cursor)
{
    Eterm tag;
    Uint32 arity;

    if (!cursor_read_tuple(cursor, &arity) || arity == 0
        || !cursor_read_atom(cursor, &tag)) {
        return DIST_FILTER_EMIT;
    }

    if (arity == 2 && tag == atom_gen_cast) {
        Eterm request;
        Uint32 request_arity;

        if (cursor_read_tuple(cursor, &request_arity)
            && request_arity == 2
            && cursor_read_atom(cursor, &request)
            && request == atom_try_again_restart) {
            return DIST_FILTER_DROP;
        }
        return DIST_FILTER_EMIT;
    }

    if (arity == 3) {
        if (tag == am_system || tag == am_EXIT || tag == atom_io_reply) {
            return DIST_FILTER_DROP;
        }
        if (tag == atom_gen_call) {
            if (!cursor_skip_terms(cursor, 1)) {
                return DIST_FILTER_EMIT;
            }
            return classify_gen_call(cursor);
        }
        if (tag == atom_remote) {
            Eterm operation;

            if (cursor_read_atom(cursor, &operation)
                && operation == atom_load_compiled) {
                return DIST_FILTER_DROP;
            }
            return DIST_FILTER_EMIT;
        }
        if (tag == am_command && cursor_skip_pid(cursor)) {
            Eterm start_tag;
            Eterm job_tag;
            Uint32 start_arity;
            Uint32 job_arity;

            if (cursor_read_tuple(cursor, &start_arity)
                && start_arity == 2
                && cursor_read_atom(cursor, &start_tag)
                && start_tag == am_start
                && cursor_read_tuple(cursor, &job_arity)
                && job_arity == 4
                && cursor_read_atom(cursor, &job_tag)
                && job_tag == atom_job
                && cursor_skip_pid(cursor)
                && cursor_skip_terms(cursor, 1)
                && cursor_is_list(cursor)) {
                return DIST_FILTER_DROP;
            }
        }
    }

    if (arity == 4 && tag == atom_io_request) {
        return DIST_FILTER_DROP;
    }
    return DIST_FILTER_EMIT;
}

/* EMIT only {'$gen_call', _, {is_auth, _}}; DROP everything else. */
static DistFilterAction
classify_net_kernel(DistFilterCursor *cursor)
{
    Eterm tag;
    Eterm request;
    Uint32 arity;
    Uint32 request_arity;

    if (!cursor_read_tuple(cursor, &arity) || arity != 3
        || !cursor_read_atom(cursor, &tag)
        || tag != atom_gen_call
        || !cursor_skip_terms(cursor, 1)
        || !cursor_read_tuple(cursor, &request_arity)
        || request_arity != 2
        || !cursor_read_atom(cursor, &request)) {
        return DIST_FILTER_DROP;
    }
    return request == atom_is_auth ? DIST_FILTER_EMIT : DIST_FILTER_DROP;
}

/* EMIT only {_, features_request} and {features_reply, _, _}; DROP everything else. */
static DistFilterAction
classify_rex(DistFilterCursor *cursor)
{
    Eterm tag;
    Uint32 arity;

    if (!cursor_read_tuple(cursor, &arity)) {
        return DIST_FILTER_DROP;
    }
    if (arity == 2) {
        if (!cursor_skip_terms(cursor, 1) || !cursor_read_atom(cursor, &tag)) {
            return DIST_FILTER_DROP;
        }
        return tag == atom_features_request ? DIST_FILTER_EMIT
                                            : DIST_FILTER_DROP;
    }
    if (arity == 3 && cursor_read_atom(cursor, &tag)
        && tag == atom_features_reply) {
        return DIST_FILTER_EMIT;
    }
    return DIST_FILTER_DROP;
}

static DistFilterAction
classify_payload(ErtsDistExternal *edep, Eterm target)
{
    DistFilterCursor cursor;
    DistFilterAction action;

    if (!cursor_init(&cursor, edep)) {
        return DIST_FILTER_INVALID;
    }
    if (target == am_net_kernel) {
        action = classify_net_kernel(&cursor);
    } else if (target == atom_rex) {
        action = classify_rex(&cursor);
    } else {
        action = classify_send(&cursor);
    }
    return cursor.invalid ? DIST_FILTER_INVALID : action;
}

static int
log_event(ErtsDistExternal *edep, DistFilterAction action, Eterm control)
{
    const byte *saved_extp = NULL;
    ErtsHeapFactory payload_factory;
    ErtsMessage *message;
    Eterm heap[6];
    Eterm *hp = heap;
    Eterm args = NIL;
    Eterm payload = am_undefined;
    Sint size;
    erts_dsprintf_buf_t *dsbuf;

    if (action == DIST_FILTER_EMIT) {
        return 1;
    }
    if (cursor_has_data(edep)) {
        edep->heap_size = -1;
        size = erts_decode_dist_ext_size(edep, 0, 1);
        if (size < 0) {
            return 0;
        }
        saved_extp = edep->data->extp;

        message = erts_factory_message_create(&payload_factory, NULL, 0, size);
        ERL_MESSAGE_TERM(message) = NIL;
        payload = erts_decode_dist_ext(&payload_factory, edep, 0);
        if (is_non_value(payload)) {
            edep->data->extp = saved_extp;
            edep->heap_size = -1;
            erts_factory_undo(&payload_factory);
            return 0;
        }
    }

    args = CONS(hp, payload, args);
    hp += 2;
    args = CONS(hp, control, args);
    hp += 2;
    args = CONS(hp, edep->dep->sysname, args);
    hp += 2;
    ASSERT(hp == &heap[sizeof(heap) / sizeof(heap[0])]);

    dsbuf = erts_create_logger_dsbuf();
    erts_dsprintf(dsbuf, "dist_filter~p~p~p");
    erts_send_error_term_to_logger(NIL, dsbuf, args);

    if (saved_extp != NULL) {
        if (action != DIST_FILTER_DROP) {
            edep->data->extp = saved_extp;
            edep->heap_size = -1;
        }
        erts_factory_undo(&payload_factory);
    }
    return 1;
}

int
dist_filter_message(ErtsDistExternal *edep, Eterm *controlp)
{
    Eterm control;
    Eterm target;
    Eterm *tuple;
    Sint type;
    DistFilterAction action;

    if (!enabled) {
        return 1;
    }
    if (edep == NULL || edep->data == NULL || controlp == NULL) {
        return 0;
    }

    control = *controlp;
    if (!is_tuple(control)) {
        return 0;
    }
    tuple = tuple_val(control);
    if (arityval(tuple[0]) < 1 || !is_small(tuple[1])) {
        return 0;
    }
    type = unsigned_val(tuple[1]);
    action = classify_control(type, control, &target);
    if (action == DIST_FILTER_INVALID) {
        return 0;
    }

    if (action == DIST_FILTER_INSPECT) {
        action = classify_payload(edep, target);
        if (action == DIST_FILTER_INVALID) {
            return 0;
        }
    }

    if (!log_event(edep, action, control)) {
        return 0;
    }
    if (action == DIST_FILTER_DROP) {
        *controlp = drop_control;
    }
    return 1;
}
