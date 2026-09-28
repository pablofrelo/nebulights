#ifndef NEBULIGHTS_INHIBIT_H
#define NEBULIGHTS_INHIBIT_H

#include <stdbool.h>

/* Whether we have a connection to the session bus at all. */
bool inhibit_available(void);

/* Whether something currently inhibits screen changes (video, game, presentation). */
bool inhibit_active(void);

void inhibit_fini(void);

#endif
