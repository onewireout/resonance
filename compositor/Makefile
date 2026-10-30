PKG_CONFIG?=pkg-config

PKGS="wlroots-0.20" wayland-server xkbcommon pangocairo pixman-1
CFLAGS_PKG_CONFIG!=$(PKG_CONFIG) --cflags $(PKGS)
CFLAGS+=$(CFLAGS_PKG_CONFIG)
LIBS!=$(PKG_CONFIG) --libs $(PKGS)

all: resonance

resonance.o: resonance.c resonance.h config.h decorations.h gestures.h ipc.h resonance_buffer.h bootintro.h
	$(CC) -c $< -g -Werror $(CFLAGS) -I. -DWLR_USE_UNSTABLE -o $@
bootintro.o: bootintro.c bootintro.h resonance_buffer.h
	$(CC) -c $< -g -Werror $(CFLAGS) -I. -DWLR_USE_UNSTABLE -o $@
config.o: config.c config.h resonance.h
	$(CC) -c $< -g -Werror $(CFLAGS) -I. -DWLR_USE_UNSTABLE -o $@
decorations.o: decorations.c decorations.h resonance.h resonance_buffer.h
	$(CC) -c $< -g -Werror $(CFLAGS) -I. -DWLR_USE_UNSTABLE -o $@
gestures.o: gestures.c gestures.h resonance.h ipc.h
	$(CC) -c $< -g -Werror $(CFLAGS) -I. -DWLR_USE_UNSTABLE -o $@
ipc.o: ipc.c ipc.h resonance.h
	$(CC) -c $< -g -Werror $(CFLAGS) -I. -DWLR_USE_UNSTABLE -o $@
resonance: resonance.o bootintro.o config.o decorations.o gestures.o ipc.o
	$(CC) $^ -g -Werror $(CFLAGS) $(LDFLAGS) $(LIBS) -ljpeg -o $@

clean:
	rm -f resonance resonance.o bootintro.o config.o decorations.o gestures.o ipc.o

.PHONY: all clean
