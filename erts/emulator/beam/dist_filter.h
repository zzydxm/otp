#ifndef DIST_FILTER_H__
#define DIST_FILTER_H__

#include "external.h"

extern Eterm dist_filter_spawn_module;
extern Eterm dist_filter_spawn_fun;

void dist_filter_init(void);

int dist_filter_message(ErtsDistExternal *edep, Eterm *controlp);

#endif
