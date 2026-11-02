// SPDX-License-Identifier: GPL-3.0-or-later
// kk_fakeinput -- drives a KWin session like a user, for the nested Wayland checks
// (scripts/wayland_session.sh with KK_COMPOSITOR=kwin). KWin in a container can only run with
// its virtual backend (its X11 / Wayland windowed backends need a GPU), which has no input
// devices; this client injects pointer and keyboard input through KWin's
// org_kde_kwin_fake_input protocol, so it goes through KWin's real input pipeline (implicit
// grabs, wl_data_device drags, xdg_toplevel.move) exactly like a mouse on Plasma.
// KWin only exposes the protocol to trusted clients: run KWin with
// KWIN_WAYLAND_NO_PERMISSION_CHECKS=1.
//
// Commands, one per line on stdin (or as arguments, ';'-separated):
//   move X Y        absolute pointer position in logical (compositor) pixels
//   down / up       left button press / release      (rdown / rup: right button)
//   drag X0 Y0 X1 Y1 [STEPS]   press at X0,Y0, move in STEPS steps, release at X1,Y1
//   key CODE        press + release a Linux evdev key code (e.g. 1 = Esc, 29 = left Ctrl)
//   keydown CODE / keyup CODE
//   sleep MS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wayland-client.h>

#include "fake-input-client-protocol.h"

static struct org_kde_kwin_fake_input* g_fake;

static void registryGlobal(void* data, struct wl_registry* registry, uint32_t name,
    const char* interface, uint32_t version)
{
    (void)data;
    if (strcmp(interface, org_kde_kwin_fake_input_interface.name) == 0)
        g_fake = wl_registry_bind(
            registry, name, &org_kde_kwin_fake_input_interface, version < 4 ? version : 4);
}

static void registryRemove(void* data, struct wl_registry* registry, uint32_t name)
{
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener kRegistryListener = {registryGlobal, registryRemove};

enum { kBtnLeft = 0x110, kBtnRight = 0x111 };

static void sleepMs(int ms)
{
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

static void move(struct wl_display* display, double x, double y)
{
    org_kde_kwin_fake_input_pointer_motion_absolute(
        g_fake, wl_fixed_from_double(x), wl_fixed_from_double(y));
    wl_display_roundtrip(display);
}

static void button(struct wl_display* display, uint32_t code, int pressed)
{
    org_kde_kwin_fake_input_button(g_fake, code, pressed ? 1 : 0);
    wl_display_roundtrip(display);
}

static int run(struct wl_display* display, char* line)
{
    char cmd[32] = {0};
    double a = 0, b = 0, c = 0, d = 0, e = 0;
    const int n = sscanf(line, "%31s %lf %lf %lf %lf %lf", cmd, &a, &b, &c, &d, &e);
    if (n < 1 || cmd[0] == '#')
        return 0;
    if (!strcmp(cmd, "move") && n >= 3) {
        move(display, a, b);
    } else if (!strcmp(cmd, "down")) {
        button(display, kBtnLeft, 1);
    } else if (!strcmp(cmd, "up")) {
        button(display, kBtnLeft, 0);
    } else if (!strcmp(cmd, "rdown")) {
        button(display, kBtnRight, 1);
    } else if (!strcmp(cmd, "rup")) {
        button(display, kBtnRight, 0);
    } else if (!strcmp(cmd, "drag") && n >= 5) {
        const int steps = n >= 6 && e > 0 ? (int)e : 20;
        move(display, a, b);
        sleepMs(150);
        button(display, kBtnLeft, 1);
        sleepMs(100);
        for (int i = 1; i <= steps; ++i) {
            move(display, a + (c - a) * i / steps, b + (d - b) * i / steps);
            sleepMs(30);
        }
        sleepMs(300);
        button(display, kBtnLeft, 0);
        sleepMs(300);
    } else if (!strcmp(cmd, "key") && n >= 2) {
        org_kde_kwin_fake_input_keyboard_key(g_fake, (uint32_t)a, 1);
        org_kde_kwin_fake_input_keyboard_key(g_fake, (uint32_t)a, 0);
        wl_display_roundtrip(display);
    } else if (!strcmp(cmd, "keydown") && n >= 2) {
        org_kde_kwin_fake_input_keyboard_key(g_fake, (uint32_t)a, 1);
        wl_display_roundtrip(display);
    } else if (!strcmp(cmd, "keyup") && n >= 2) {
        org_kde_kwin_fake_input_keyboard_key(g_fake, (uint32_t)a, 0);
        wl_display_roundtrip(display);
    } else if (!strcmp(cmd, "sleep") && n >= 2) {
        sleepMs((int)a);
    } else {
        fprintf(stderr, "kk_fakeinput: unknown command: %s\n", line);
        return 1;
    }
    return 0;
}

int main(int argc, char** argv)
{
    struct wl_display* display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "kk_fakeinput: cannot connect to the Wayland display\n");
        return 2;
    }
    struct wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &kRegistryListener, NULL);
    wl_display_roundtrip(display);
    if (!g_fake) {
        fprintf(stderr, "kk_fakeinput: no org_kde_kwin_fake_input (KWin with "
                        "KWIN_WAYLAND_NO_PERMISSION_CHECKS=1 needed)\n");
        return 3;
    }
    org_kde_kwin_fake_input_authenticate(g_fake, "kk_fakeinput", "Kinoklip Wayland tests");
    wl_display_roundtrip(display);

    int rc = 0;
    if (argc > 1) {
        for (int i = 1; i < argc && !rc; ++i) {
            char* save = NULL;
            for (char* part = strtok_r(argv[i], ";", &save); part && !rc;
                 part = strtok_r(NULL, ";", &save))
                rc = run(display, part);
        }
    } else {
        char line[256];
        while (!rc && fgets(line, sizeof(line), stdin))
            rc = run(display, line);
    }
    wl_display_roundtrip(display);
    wl_display_disconnect(display);
    return rc;
}
