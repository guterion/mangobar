// tests/watcher_prune_test.c
// @guterion
// GPL-3.0-only
// Checks that the tray watcher drops the items of a client that left the bus.

// The check includes the source, so it reaches the static functions of the
// watcher. Run it under dbus-run-session, because it owns the watcher name.

#include "../tray.c"

#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define WATCHER_NAME "org.kde.StatusNotifierWatcher"
#define WAIT_STEPS 100
#define WAIT_STEP_NS 50000000L

static void no_redraw(void) {}

static void require(bool condition, const char *what) {
  if (condition)
    return;
  fprintf(stderr, "watcher prune check fails: %s\n", what);
  exit(1);
}

static void pause_one_step(void) {
  const struct timespec step = {.tv_nsec = WAIT_STEP_NS};
  nanosleep(&step, NULL);
}

/** Serves the watcher on this process until a signal ends the process. */
static void run_watcher(void) {
  MangobarTray *tray = tray_init(no_redraw);
  require(tray != NULL, "the tray did not start");
  struct pollfd fd = {.fd = tray_get_fd(tray)};
  for (;;) {
    fd.events = (short)tray_get_events(tray);
    poll(&fd, 1, 100);
    tray_dispatch(tray);
  }
}

/** Returns a new bus connection that registered the item at the path. */
static sd_bus *register_item(const char *path) {
  sd_bus *bus = NULL;
  require(sd_bus_open_user(&bus) >= 0, "the client did not connect");
  for (int step = 0; step < WAIT_STEPS; step++) {
    if (sd_bus_call_method(bus, WATCHER_NAME, WATCHER_PATH, WATCHER_NAME,
                           "RegisterStatusNotifierItem", NULL, NULL, "s",
                           path) >= 0)
      return bus;
    pause_one_step();
  }
  require(false, "the watcher did not accept the item");
  return NULL;
}

/** Returns true when the list holds one item, the /Live item of the bus. */
static bool list_holds_live_alone(sd_bus *bus) {
  const char *name = NULL;
  char **items = NULL;
  require(sd_bus_get_unique_name(bus, &name) >= 0, "the client has no name");
  require(sd_bus_get_property_strv(bus, WATCHER_NAME, WATCHER_PATH,
                                   WATCHER_NAME,
                                   "RegisteredStatusNotifierItems", NULL,
                                   &items) >= 0,
          "the watcher did not report its list");
  char *expected = fmt_str("%s/Live", name);
  bool holds = items && items[0] && strcmp(items[0], expected) == 0 && !items[1];
  free(expected);
  for (char **item = items; item && *item; item++)
    free(*item);
  free(items);
  return holds;
}

int main(void) {
  // The match stops at the end of the name, so :1.21 leaves :1.214 alone.
  require(item_id_has_service(":1.21", ":1.21"), "a bare service");
  require(item_id_has_service(":1.21/Item", ":1.21"), "a service with a path");
  require(!item_id_has_service(":1.214/Item", ":1.21"), "a longer service");
  require(!item_id_has_service(":1.2", ":1.21"), "a shorter service");

  pid_t watcher = fork();
  require(watcher >= 0, "the watcher process did not start");
  if (watcher == 0)
    run_watcher();

  sd_bus *live = register_item("/Live");
  sd_bus *dead = register_item("/Dead");
  sd_bus_flush_close_unref(dead);

  bool pruned = false;
  for (int step = 0; step < WAIT_STEPS && !pruned; step++) {
    pruned = list_holds_live_alone(live);
    if (!pruned)
      pause_one_step();
  }

  kill(watcher, SIGTERM);
  waitpid(watcher, NULL, 0);
  sd_bus_flush_close_unref(live);

  require(pruned, "the list holds an item other than /Live");
  puts("watcher prune check passes");
  return 0;
}
