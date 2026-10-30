#ifndef CONFIG_H
#define CONFIG_H

/* Configuration loading for the Resonance compositor.
 *
 * Extracted from resonance.c (see Documentation/Resonance-Refactor-Plan.md).
 * Parses the runtime keybinding table from /tmp/resonance-keybindings.txt
 * (written by the hub). Static data — loaded once at startup, never
 * changes at runtime. */

struct resonance_server;

void config_load_keybindings(struct resonance_server *server);

#endif /* CONFIG_H */
