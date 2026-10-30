#define _GNU_SOURCE

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <wlr/types/wlr_keyboard.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>

#include "resonance.h"
#include "config.h"

void config_load_keybindings(struct resonance_server *server) {
	for (size_t i = 0; i < server->keybindings_len; i++) {
		free(server->keybindings[i].name);
		free(server->keybindings[i].action);
	}
	free(server->keybindings);
	server->keybindings = NULL;
	server->keybindings_len = 0;

	FILE *f = fopen("/tmp/resonance-keybindings.txt", "r");
	if (!f) {
		wlr_log(WLR_INFO, "No resonance-keybindings.txt file found. Keybindings won't be loaded.");
		return;
	}

	char line[256];
	size_t capacity = 8;
	server->keybindings = malloc(capacity * sizeof(struct resonance_keybinding));
	if (!server->keybindings) {
		wlr_log(WLR_ERROR, "Failed to allocate memory for keybindings!");
		fclose(f);
		return;
	}

	while (fgets(line, sizeof(line), f)) {
		size_t len = strlen(line);
		while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
			line[len - 1] = '\0';
			len--;
		}
		if (len == 0) continue;

		/* Split key combo from action on '=' */
		char *action = NULL;
		char *eq = strchr(line, '=');
		if (eq) {
			*eq = '\0';
			action = strdup(eq + 1);
			if (!action) {
				continue;
			}
			/* trim trailing whitespace from key part */
			size_t klen = strlen(line);
			while (klen > 0 && (line[klen-1] == ' ' || line[klen-1] == '\t')) {
				line[--klen] = '\0';
			}
			len = klen;
		}
		if (len == 0) { free(action); continue; }

		uint32_t modifiers = 0;
		char *key_name = NULL;
		char *orig_line = strdup(line);
		if (!orig_line) {
			free(action);
			continue;
		}

		char *token = strtok(line, "+");
		while (token != NULL) {
			char *next_token = strtok(NULL, "+");
			if (next_token == NULL) {
				key_name = token;
				break;
			}
			if (strcasecmp(token, "Super") == 0 || strcasecmp(token, "Logo") == 0 || strcasecmp(token, "Mod4") == 0) {
				modifiers |= WLR_MODIFIER_LOGO;
			} else if (strcasecmp(token, "Shift") == 0) {
				modifiers |= WLR_MODIFIER_SHIFT;
			} else if (strcasecmp(token, "Ctrl") == 0 || strcasecmp(token, "Control") == 0) {
				modifiers |= WLR_MODIFIER_CTRL;
			} else if (strcasecmp(token, "Alt") == 0 || strcasecmp(token, "Mod1") == 0) {
				modifiers |= WLR_MODIFIER_ALT;
			}
			token = next_token;
		}

		if (key_name) {
			if (strlen(key_name) == 1) {
				if (modifiers & WLR_MODIFIER_SHIFT) {
					key_name[0] = toupper((unsigned char)key_name[0]);
				} else {
					key_name[0] = tolower((unsigned char)key_name[0]);
				}
			}
			xkb_keysym_t keysym = xkb_keysym_from_name(key_name, XKB_KEYSYM_NO_FLAGS);
			if (keysym == XKB_KEY_NoSymbol) {
				if (strcasecmp(key_name, "Space") == 0) {
					keysym = XKB_KEY_space;
				} else if (strcasecmp(key_name, "Esc") == 0 || strcasecmp(key_name, "Escape") == 0) {
					keysym = XKB_KEY_Escape;
				} else if (strcasecmp(key_name, "Enter") == 0 || strcasecmp(key_name, "Return") == 0) {
					keysym = XKB_KEY_Return;
				} else {
					char *lower = strdup(key_name);
					if (lower) {
						for (int i = 0; lower[i]; i++) lower[i] = tolower((unsigned char)lower[i]);
						keysym = xkb_keysym_from_name(lower, XKB_KEYSYM_NO_FLAGS);
						free(lower);
					}
				}
			}

			if (keysym != XKB_KEY_NoSymbol) {
				if (server->keybindings_len >= capacity) {
					capacity *= 2;
					struct resonance_keybinding *temp = realloc(
						server->keybindings, capacity * sizeof(struct resonance_keybinding));
					if (!temp) {
						wlr_log(WLR_ERROR, "Failed to reallocate keybindings!");
						free(orig_line);
						free(action);
						break;
					}
					server->keybindings = temp;
				}
				server->keybindings[server->keybindings_len].modifiers = modifiers;
				server->keybindings[server->keybindings_len].keysym = keysym;
				server->keybindings[server->keybindings_len].name = orig_line;
				server->keybindings[server->keybindings_len].action = action;
				action = NULL;
				server->keybindings_len++;
				wlr_log(WLR_INFO, "Registered keybinding: %s (mods: 0x%x, keysym: 0x%x)", orig_line, modifiers, keysym);
			} else {
				wlr_log(WLR_ERROR, "Unknown keysym name in keybinding: %s", orig_line);
				free(orig_line);
				free(action);
			}
		} else {
			free(orig_line);
			free(action);
		}
	}
	fclose(f);
}
