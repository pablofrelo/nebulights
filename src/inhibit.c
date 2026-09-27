/*
 * Wykrywanie blokad bezczynności zgłoszonych przez D-Bus.
 *
 * Istnieją dwa równoległe mechanizmy: waylandowy idle-inhibit-unstable-v1
 * i D-Busowy org.freedesktop.ScreenSaver. Protokół ext-idle-notify-v1
 * respektuje tylko ten pierwszy, a przeglądarki i część odtwarzaczy
 * używają drugiego — stąd wygaszacz wskakujący w trakcie filmu.
 *
 * PowerDevil zbiera zgłoszenia z obu dróg i wystawia HasInhibition,
 * więc pytamy jego. Poza Plasmą po prostu nie ma na czym odpowiedzieć
 * i wtedy nie blokujemy niczego.
 */

#include <stdbool.h>
#include <stdio.h>

#include "inhibit.h"

#ifdef HAVE_SDBUS

#include <systemd/sd-bus.h>

/* PowerDevil::PolicyAgent::RequiredPolicies — nas interesuje ekran */
#define POLICY_CHANGE_SCREEN_SETTINGS 2

static sd_bus *bus = NULL;
static bool   tried = false;
static bool   warned = false;

bool inhibit_available(void)
{
	if (!tried) {
		tried = true;
		if (sd_bus_open_user(&bus) < 0)
			bus = NULL;
	}
	return bus != NULL;
}

bool inhibit_active(void)
{
	if (!inhibit_available())
		return false;

	sd_bus_error err = SD_BUS_ERROR_NULL;
	sd_bus_message *reply = NULL;
	int screen = 0;

	int r = sd_bus_call_method(bus,
		"org.kde.Solid.PowerManagement.PolicyAgent",
		"/org/kde/Solid/PowerManagement/PolicyAgent",
		"org.kde.Solid.PowerManagement.PolicyAgent",
		"HasInhibition",
		&err, &reply, "u", (unsigned)POLICY_CHANGE_SCREEN_SETTINGS);

	if (r < 0) {
		/* Brak PowerDevila (inne środowisko) — nie jest to błąd,
		   ale powiedzmy o tym raz, żeby nie było zagadki. */
		if (!warned) {
			warned = true;
			fprintf(stderr, "nebulights: PowerDevil not found on D-Bus, "
			        "inhibitors reported there will not be seen\n");
		}
		sd_bus_error_free(&err);
		return false;
	}

	if (sd_bus_message_read(reply, "b", &screen) < 0)
		screen = 0;

	sd_bus_message_unref(reply);
	sd_bus_error_free(&err);
	return screen != 0;
}

void inhibit_fini(void)
{
	if (bus) {
		sd_bus_unref(bus);
		bus = NULL;
	}
}

#else  /* zbudowane bez libsystemd */

bool inhibit_available(void) { return false; }
bool inhibit_active(void)    { return false; }
void inhibit_fini(void)      { }

#endif
