#ifndef NEBULIGHTS_INHIBIT_H
#define NEBULIGHTS_INHIBIT_H

#include <stdbool.h>

/* Czy w ogóle mamy połączenie z magistralą sesji. */
bool inhibit_available(void);

/* Czy coś aktualnie blokuje zmianę ustawień ekranu (film, gra, prezentacja). */
bool inhibit_active(void);

void inhibit_fini(void);

#endif
