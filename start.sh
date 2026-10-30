#!/bin/bash

RESONANCE_DIR="$(cd "$(dirname "$0")" && pwd)"
exec 2>>/tmp/resonance.log

# Pick the DRM device that actually has a connected display, rather than
# hardcoding a card number (card0/card1 assignment isn't guaranteed stable
# across boots when multiple GPUs share the same driver).
for status in /sys/class/drm/card*-*/status; do
    if [ "$(cat "$status")" = "connected" ]; then
        card="$(basename "$(dirname "$status")" | sed 's/-.*//')"
        export WLR_DRM_DEVICES="/dev/dri/$card"
        break
    fi
done
export WLR_NO_HARDWARE_CURSORS=1

export XCURSOR_THEME=Resonance
export XCURSOR_SIZE=24
export GTK_THEME=Resonance
export ELECTRON_OZONE_PLATFORM_HINT=auto
export MOZ_ENABLE_WAYLAND=1
export XDG_SESSION_TYPE=wayland
export LD_PRELOAD=/usr/lib/libgtk-nocsd.so.0

# Clean up any stale keybindings from previous runs
rm -f /tmp/resonance-keybindings.txt

# brain.py watchdog — restarts silently if it ever dies
while true; do
    python3 "$RESONANCE_DIR/core/brain.py"
    sleep 0.5
done &
BRAIN_WATCHDOG_PID=$!

# Wait for the watchdog to generate the keybindings file
while [ ! -f /tmp/resonance-keybindings.txt ]; do
    sleep 0.02
done

# Run the compositor — if it exits for any reason, kill the watchdog cleanly
"$RESONANCE_DIR/compositor/resonance" -s "python3 $RESONANCE_DIR/core/startup.py"

kill $BRAIN_WATCHDOG_PID 2>/dev/null
