#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#define ERTS_WANT_EXTERNAL_TAGS

#include "sys.h"
#include "global.h"
#include "atom.h"
#include "external.h"
#include "dist_filter_cursor.h"

int
cursor_has_data(const ErtsDistExternal *edep)
{
    Uint64 fragment;
    Uint64 fragments;

    if (edep == NULL || edep->data == NULL) {
        return 0;
    }
    fragments = edep->data[0].frag_id;
    if (fragments == 0) {
        fragments = 1;
    }
    for (fragment = 0; fragment < fragments; fragment++) {
        if (edep->data[fragment].extp < edep->data[fragment].ext_endp) {
            return 1;
        }
    }
    return 0;
}

static void
cursor_set_fragment(DistFilterCursor *cursor, Uint64 fragment)
{
    cursor->fragment = fragment;
    cursor->p = cursor->edep->data[fragment].extp;
    cursor->end = cursor->edep->data[fragment].ext_endp;
}

static int
cursor_advance(DistFilterCursor *cursor)
{
    while (cursor->p == cursor->end
           && cursor->fragment + 1 < cursor->fragments) {
        cursor_set_fragment(cursor, cursor->fragment + 1);
    }
    return cursor->p < cursor->end;
}

static int
cursor_fail(DistFilterCursor *cursor)
{
    cursor->invalid = 1;
    return 0;
}

static int
cursor_read_u8(DistFilterCursor *cursor, byte *value)
{
    if (!cursor_advance(cursor)) {
        return cursor_fail(cursor);
    }
    *value = *cursor->p++;
    return 1;
}

static int
cursor_read_u16(DistFilterCursor *cursor, Uint16 *value)
{
    byte a;
    byte b;

    if (!cursor_read_u8(cursor, &a) || !cursor_read_u8(cursor, &b)) {
        return 0;
    }
    *value = ((Uint16)a << 8) | b;
    return 1;
}

static int
cursor_read_u32(DistFilterCursor *cursor, Uint32 *value)
{
    byte a;
    byte b;
    byte c;
    byte d;

    if (!cursor_read_u8(cursor, &a)
        || !cursor_read_u8(cursor, &b)
        || !cursor_read_u8(cursor, &c)
        || !cursor_read_u8(cursor, &d)) {
        return 0;
    }
    *value = ((Uint32)a << 24) | ((Uint32)b << 16)
        | ((Uint32)c << 8) | d;
    return 1;
}

static int
cursor_skip_bytes(DistFilterCursor *cursor, Uint64 size)
{
    while (size != 0) {
        Uint64 available;
        Uint64 take;

        if (!cursor_advance(cursor)) {
            return cursor_fail(cursor);
        }
        available = cursor->end - cursor->p;
        take = size < available ? size : available;
        cursor->p += take;
        size -= take;
    }
    return 1;
}

static int
cursor_read_bytes(DistFilterCursor *cursor, byte *bytes, Uint size)
{
    Uint i;

    for (i = 0; i < size; i++) {
        if (!cursor_read_u8(cursor, &bytes[i])) {
            return 0;
        }
    }
    return 1;
}

static int
cursor_add_terms(DistFilterCursor *cursor, Uint32 *terms, Uint32 add)
{
    Uint32 before = *terms;

    *terms += add;
    return *terms >= before ? 1 : cursor_fail(cursor);
}

int
cursor_skip_terms(DistFilterCursor *cursor, Uint32 terms)
{
    Uint64 atom_extra_skip = 0;

    while (terms != 0) {
        Uint16 size16;
        Uint32 size32;
        byte size8;
        byte tag;

        if (!cursor_read_u8(cursor, &tag)) {
            return 0;
        }

        if (atom_extra_skip != 0
            && tag != ATOM_EXT && tag != SMALL_ATOM_EXT
            && tag != ATOM_UTF8_EXT && tag != SMALL_ATOM_UTF8_EXT
            && tag != ATOM_CACHE_REF) {
            return cursor_fail(cursor);
        }

        switch (tag) {
        case SMALL_INTEGER_EXT:
            if (!cursor_skip_bytes(cursor, 1)) return 0;
            break;
        case INTEGER_EXT:
            if (!cursor_skip_bytes(cursor, 4)) return 0;
            break;
        case FLOAT_EXT:
            if (!cursor_skip_bytes(cursor, 31)) return 0;
            break;
        case NEW_FLOAT_EXT:
            if (!cursor_skip_bytes(cursor, 8)) return 0;
            break;
        case ATOM_EXT:
        case ATOM_UTF8_EXT:
            if (!cursor_read_u16(cursor, &size16)) return 0;
            if ((tag == ATOM_EXT && size16 > MAX_ATOM_CHARACTERS)
                || (tag == ATOM_UTF8_EXT && size16 > MAX_ATOM_SZ_LIMIT)
                || !cursor_skip_bytes(cursor,
                                      (Uint64)size16 + atom_extra_skip)) {
                return cursor_fail(cursor);
            }
            atom_extra_skip = 0;
            break;
        case SMALL_ATOM_EXT:
        case SMALL_ATOM_UTF8_EXT:
            if (!cursor_read_u8(cursor, &size8)) return 0;
            if ((tag == SMALL_ATOM_EXT && size8 > MAX_ATOM_CHARACTERS)
                || !cursor_skip_bytes(cursor,
                                      (Uint64)size8 + atom_extra_skip)) {
                return cursor_fail(cursor);
            }
            atom_extra_skip = 0;
            break;
        case ATOM_CACHE_REF:
            if (!cursor_skip_bytes(cursor, 1 + atom_extra_skip)) {
                return 0;
            }
            atom_extra_skip = 0;
            break;
        case REFERENCE_EXT:
            atom_extra_skip = 5;
            if (!cursor_add_terms(cursor, &terms, 1)) return 0;
            break;
        case NEW_REFERENCE_EXT:
        case NEWER_REFERENCE_EXT:
            if (!cursor_read_u16(cursor, &size16)
                || size16 > ERTS_MAX_REF_NUMBERS) {
                return cursor_fail(cursor);
            }
            atom_extra_skip = (tag == NEW_REFERENCE_EXT ? 1 : 4)
                + 4 * (Uint64)size16;
            if (!cursor_add_terms(cursor, &terms, 1)) return 0;
            break;
        case PORT_EXT:
            atom_extra_skip = 5;
            if (!cursor_add_terms(cursor, &terms, 1)) return 0;
            break;
        case NEW_PORT_EXT:
            atom_extra_skip = 8;
            if (!cursor_add_terms(cursor, &terms, 1)) return 0;
            break;
        case V4_PORT_EXT:
            atom_extra_skip = 12;
            if (!cursor_add_terms(cursor, &terms, 1)) return 0;
            break;
        case PID_EXT:
            atom_extra_skip = 9;
            if (!cursor_add_terms(cursor, &terms, 1)) return 0;
            break;
        case NEW_PID_EXT:
            atom_extra_skip = 12;
            if (!cursor_add_terms(cursor, &terms, 1)) return 0;
            break;
        case SMALL_TUPLE_EXT:
            if (!cursor_read_u8(cursor, &size8)
                || !cursor_add_terms(cursor, &terms, size8)) {
                return 0;
            }
            break;
        case LARGE_TUPLE_EXT:
            if (!cursor_read_u32(cursor, &size32)
                || size32 > ERTS_MAX_TUPLE_SIZE
                || !cursor_add_terms(cursor, &terms, size32)) {
                return cursor_fail(cursor);
            }
            break;
        case NIL_EXT:
            break;
        case STRING_EXT:
            if (!cursor_read_u16(cursor, &size16)
                || !cursor_skip_bytes(cursor, size16)) {
                return 0;
            }
            break;
        case LIST_EXT:
            if (!cursor_read_u32(cursor, &size32)
                || !cursor_add_terms(cursor, &terms, size32)
                || !cursor_add_terms(cursor, &terms, 1)) {
                return 0;
            }
            break;
        case BINARY_EXT:
            if (!cursor_read_u32(cursor, &size32)
                || !cursor_skip_bytes(cursor, size32)) {
                return 0;
            }
            break;
        case BIT_BINARY_EXT:
            if (!cursor_read_u32(cursor, &size32)
                || !cursor_skip_bytes(cursor, 1 + (Uint64)size32)) {
                return 0;
            }
            break;
        case SMALL_BIG_EXT:
            if (!cursor_read_u8(cursor, &size8)
                || !cursor_skip_bytes(cursor, 1 + (Uint64)size8)) {
                return 0;
            }
            break;
        case LARGE_BIG_EXT:
            if (!cursor_read_u32(cursor, &size32)
                || !cursor_skip_bytes(cursor, 1 + (Uint64)size32)) {
                return 0;
            }
            break;
        case NEW_FUN_EXT:
            if (!cursor_skip_bytes(cursor, 25)
                || !cursor_read_u32(cursor, &size32)
                || size32 > MAX_ARG
                || !cursor_add_terms(cursor, &terms, 4)
                || !cursor_add_terms(cursor, &terms, size32)) {
                return cursor_fail(cursor);
            }
            break;
        case EXPORT_EXT:
            if (!cursor_add_terms(cursor, &terms, 3)) return 0;
            break;
        case MAP_EXT:
            if (!cursor_read_u32(cursor, &size32)
                || !cursor_add_terms(cursor, &terms, size32)
                || !cursor_add_terms(cursor, &terms, size32)) {
                return 0;
            }
            break;
        case RECORD_EXT:
            if (!cursor_read_u32(cursor, &size32)
                || !cursor_skip_bytes(cursor, 1)
                || !cursor_add_terms(cursor, &terms, 2)
                || !cursor_add_terms(cursor, &terms, size32)
                || !cursor_add_terms(cursor, &terms, size32)) {
                return 0;
            }
            break;
        default:
            return cursor_fail(cursor);
        }
        terms--;
    }
    return atom_extra_skip == 0 ? 1 : cursor_fail(cursor);
}

int
cursor_init(DistFilterCursor *cursor, const ErtsDistExternal *edep)
{
    byte version;

    if (edep == NULL || edep->data == NULL) {
        return 0;
    }
    cursor->edep = edep;
    cursor->fragments = edep->data[0].frag_id;
    if (cursor->fragments == 0) {
        cursor->fragments = 1;
    }
    cursor->invalid = 0;
    cursor_set_fragment(cursor, 0);

    if (edep->flags & ERTS_DIST_EXT_DFLAG_HDR) {
        DistFilterCursor copy = *cursor;
        if (!cursor_read_u8(&copy, &version) || version == VERSION_MAGIC) {
            return cursor_fail(cursor);
        }
    } else if (!cursor_read_u8(cursor, &version) || version != VERSION_MAGIC) {
        return cursor_fail(cursor);
    }
    return 1;
}

int
cursor_read_tuple(DistFilterCursor *cursor, Uint32 *arity)
{
    DistFilterCursor copy = *cursor;
    byte size8;
    byte tag;

    if (!cursor_read_u8(&copy, &tag)) {
        cursor->invalid = 1;
        return 0;
    }
    if (tag == SMALL_TUPLE_EXT) {
        if (!cursor_read_u8(&copy, &size8)) {
            cursor->invalid = 1;
            return 0;
        }
        *arity = size8;
    } else if (tag == LARGE_TUPLE_EXT) {
        if (!cursor_read_u32(&copy, arity) || *arity > ERTS_MAX_TUPLE_SIZE) {
            cursor->invalid = 1;
            return 0;
        }
    } else {
        return 0;
    }
    *cursor = copy;
    return 1;
}

int
cursor_read_atom(DistFilterCursor *cursor, Eterm *atom)
{
    DistFilterCursor copy = *cursor;
    ErtsAtomEncoding encoding;
    byte bytes[MAX_ATOM_SZ_LIMIT];
    Uint size;
    Uint16 size16;
    byte index;
    byte size8;
    byte tag;

    if (!cursor_read_u8(&copy, &tag)) {
        cursor->invalid = 1;
        return 0;
    }
    if (tag == ATOM_CACHE_REF) {
        if (!cursor_read_u8(&copy, &index)
            || !(cursor->edep->flags & ERTS_DIST_EXT_ATOM_TRANS_TAB)
            || index >= cursor->edep->attab.size) {
            cursor->invalid = 1;
            return 0;
        }
        *atom = cursor->edep->attab.atom[index];
        *cursor = copy;
        return 1;
    }

    if (tag == ATOM_EXT || tag == ATOM_UTF8_EXT) {
        if (!cursor_read_u16(&copy, &size16)) {
            cursor->invalid = 1;
            return 0;
        }
        size = size16;
    } else if (tag == SMALL_ATOM_EXT || tag == SMALL_ATOM_UTF8_EXT) {
        if (!cursor_read_u8(&copy, &size8)) {
            cursor->invalid = 1;
            return 0;
        }
        size = size8;
    } else {
        return 0;
    }

    encoding = tag == ATOM_EXT || tag == SMALL_ATOM_EXT
        ? ERTS_ATOM_ENC_LATIN1
        : ERTS_ATOM_ENC_UTF8;
    if (size > sizeof(bytes) || !cursor_read_bytes(&copy, bytes, size)) {
        cursor->invalid = 1;
        return 0;
    }
    *atom = erts_atom_put(bytes, size, encoding, 0);
    if (is_non_value(*atom)) {
        cursor->invalid = 1;
        return 0;
    }
    *cursor = copy;
    return 1;
}

static int
cursor_peek_tag(DistFilterCursor *cursor, byte *tag)
{
    DistFilterCursor copy = *cursor;

    if (!cursor_read_u8(&copy, tag)) {
        cursor->invalid = 1;
        return 0;
    }
    return 1;
}

int
cursor_is_atom(DistFilterCursor *cursor)
{
    byte tag;

    return cursor_peek_tag(cursor, &tag)
        && (tag == ATOM_EXT || tag == SMALL_ATOM_EXT
            || tag == ATOM_UTF8_EXT || tag == SMALL_ATOM_UTF8_EXT
            || tag == ATOM_CACHE_REF);
}

int
cursor_skip_atom(DistFilterCursor *cursor)
{
    return cursor_is_atom(cursor) && cursor_skip_terms(cursor, 1);
}

int
cursor_is_integer(DistFilterCursor *cursor)
{
    byte tag;

    return cursor_peek_tag(cursor, &tag)
        && (tag == SMALL_INTEGER_EXT || tag == INTEGER_EXT
            || tag == SMALL_BIG_EXT || tag == LARGE_BIG_EXT);
}

int
cursor_skip_integer(DistFilterCursor *cursor)
{
    return cursor_is_integer(cursor) && cursor_skip_terms(cursor, 1);
}

int
cursor_is_pid(DistFilterCursor *cursor)
{
    byte tag;

    return cursor_peek_tag(cursor, &tag)
        && (tag == PID_EXT || tag == NEW_PID_EXT);
}

int
cursor_skip_pid(DistFilterCursor *cursor)
{
    return cursor_is_pid(cursor) && cursor_skip_terms(cursor, 1);
}

int
cursor_is_fun(DistFilterCursor *cursor)
{
    byte tag;

    return cursor_peek_tag(cursor, &tag)
        && (tag == NEW_FUN_EXT || tag == EXPORT_EXT);
}

int
cursor_is_list(DistFilterCursor *cursor)
{
    byte tag;

    return cursor_peek_tag(cursor, &tag)
        && (tag == NIL_EXT || tag == STRING_EXT || tag == LIST_EXT);
}
