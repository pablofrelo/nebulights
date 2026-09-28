/*
 * Detecting idle inhibitors reported over D-Bus.
 *
 * There are two parallel mechanisms: Wayland's idle-inhibit-unstable-v1
 * and D-Bus org.freedesktop.ScreenSaver. The ext-idle-notify-v1 protocol
 * only honours the first one, while browsers and some players use the
 * second — hence the screensaver kicking in during a video.
 *
 * PowerDevil collects inhibitors from both and exposes HasInhibition,
 * so we ask it. Outside Plasma there is nothing to answer, and then
 * we inhibit nothing.
 */

#include <stdbool.h>
#include <stdio.h>

#include "inhibit.h"

#ifdef HAVE_SDBUS

#include <systemd/sd-bus.h>

/* PowerDevil::PolicyAgent::RequiredPolicies — we care about the screen */
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
		/* No PowerDevil (another desktop) — not an error,
		   but say so once so it isn't a mystery. */
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

#else  /* built without libsystemd */

bool inhibit_available(void) { return false; }
bool inhibit_active(void)    { return false; }
void inhibit_fini(void)      { }

#endif
