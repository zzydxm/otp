#ifndef DIST_FILTER_CURSOR_H__
#define DIST_FILTER_CURSOR_H__

#include "external.h"

typedef struct {
    const ErtsDistExternal *edep;
    Uint64 fragment;
    Uint64 fragments;
    const byte *p;
    const byte *end;
    int invalid;
} DistFilterCursor;

int cursor_has_data(const ErtsDistExternal *edep);
int cursor_init(DistFilterCursor *cursor, const ErtsDistExternal *edep);
int cursor_read_tuple(DistFilterCursor *cursor, Uint32 *arity);
int cursor_read_atom(DistFilterCursor *cursor, Eterm *atom);
int cursor_skip_terms(DistFilterCursor *cursor, Uint32 terms);

int cursor_is_atom(DistFilterCursor *cursor);
int cursor_skip_atom(DistFilterCursor *cursor);
int cursor_is_integer(DistFilterCursor *cursor);
int cursor_skip_integer(DistFilterCursor *cursor);
int cursor_is_pid(DistFilterCursor *cursor);
int cursor_skip_pid(DistFilterCursor *cursor);
int cursor_is_fun(DistFilterCursor *cursor);
int cursor_is_list(DistFilterCursor *cursor);

#endif
