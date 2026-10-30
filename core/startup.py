import tomllib
import subprocess
import os
from pathlib import Path

CONFIG_PATH = Path(__file__).parent.parent / "resonance.toml"

def load_config():
    with open(CONFIG_PATH, "rb") as f:
        return tomllib.load(f)

def launch_wallpaper(config):
    wallpaper = config.get("wallpaper", {})
    path = os.path.expanduser(wallpaper.get("path", ""))
    mode = wallpaper.get("mode", "fill")
    if path:
        subprocess.Popen(["swaybg", "-i", path, "-m", mode],
                         stdout=subprocess.DEVNULL,
                         stderr=subprocess.DEVNULL)

def unlock_keyring():
    subprocess.Popen([
        "gdbus", "call", "--session",
        "--dest", "org.freedesktop.secrets",
        "--object-path", "/org/freedesktop/secrets",
        "--method", "org.freedesktop.Secret.Service.Unlock",
        "[objectpath '/org/freedesktop/secrets/aliases/default']",
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

def launch_startup_apps(config):
    apps = config.get("startup", {}).get("apps", [])
    for app in apps:
        if app == "swaybg":
            launch_wallpaper(config)
        else:
            subprocess.Popen([app],
                             stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL)

def main():
    # Everything launched during a session inherits this working directory,
    # and apps that write relative-path config (SPT's launcher, for one) will
    # drop files into it. Anchor at $HOME so that lands somewhere harmless
    # instead of inside the Resonance repo. Nothing here reads relative
    # paths — CONFIG_PATH and friends are all built from __file__.
    os.chdir(Path.home())

    os.environ["XDG_CURRENT_DESKTOP"] = "Resonance"
    os.environ["XDG_SESSION_TYPE"] = "wayland"
    os.environ["MOZ_ENABLE_WAYLAND"] = "1"
    display = os.environ.get("DISPLAY", "")
    wayland_display = os.environ.get("WAYLAND_DISPLAY", "wayland-0")
    dbus_address = os.environ.get("DBUS_SESSION_BUS_ADDRESS", "")
    
    systemctl_env = [
        "systemctl", "--user", "set-environment",
        f"WAYLAND_DISPLAY={wayland_display}",
        "XDG_CURRENT_DESKTOP=Resonance",
        "XDG_SESSION_TYPE=wayland",
        "MOZ_ENABLE_WAYLAND=1",
        "LD_PRELOAD=/usr/lib/libgtk-nocsd.so.0",
        f"DBUS_SESSION_BUS_ADDRESS={dbus_address}"
    ]
    if display:
        systemctl_env.append(f"DISPLAY={display}")
    subprocess.run(systemctl_env)

    dbus_env = ["dbus-update-activation-environment", "WAYLAND_DISPLAY", "XDG_CURRENT_DESKTOP", "XDG_SESSION_TYPE", "MOZ_ENABLE_WAYLAND", "LD_PRELOAD"]
    if display:
        dbus_env.append("DISPLAY")
    subprocess.run(dbus_env)
    unlock_keyring()
    subprocess.run(["pkill", "-f", "xdg-desktop-portal"],
                   stderr=subprocess.DEVNULL)
    subprocess.run(["systemctl", "--user", "stop",
                    "xdg-desktop-portal-wlr",
                    "xdg-desktop-portal"])
    subprocess.run(["systemctl", "--user", "reset-failed",
                    "xdg-desktop-portal-wlr"],
                   stderr=subprocess.DEVNULL)
    # Kill any stale instance from a prior session; spyglass is now D-Bus
    # activated on-demand (see ~/.local/share/dbus-1/services/org.freedesktop.impl.portal.desktop.resonance.service)
    # instead of being launched persistently at boot.
    subprocess.run(["pkill", "-f", "spyglass.py"], stderr=subprocess.DEVNULL)

    # Flow daemon is managed by systemd (resonance-flow.service) — no manual launch needed.
    import time; time.sleep(0.5)
    subprocess.Popen(["systemctl", "--user", "start", "xdg-desktop-portal"])
    config = load_config()
    launch_startup_apps(config)


if __name__ == "__main__":
    main()
