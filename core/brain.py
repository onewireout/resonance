#!/usr/bin/env python3
import socket
import subprocess
import os
import sys
import json
import tomllib
from pathlib import Path
import placement

SOCKET_PATH = '/tmp/resonance.sock'
WINDOWS_PATH = '/tmp/resonance-windows.json'
CONFIG_PATH = Path(__file__).parent.parent / 'resonance.toml'

windows_by_id = {}  # numeric window id -> {app_id, title, fullscreen, x, y, w, h}
                    # the one source of truth for "what windows exist" - id
                    # is the key everywhere; title/app_id are just fields
                    # riding along for display or app-level grouping, never
                    # used to look a window up. Written to WINDOWS_PATH
                    # as-is for consumers like Spyglass's picker.


def load_config():
    with open(CONFIG_PATH, 'rb') as f:
        return tomllib.load(f)


def write_keybindings_file():
    try:
        config = load_config()
        keybindings = config.get('keybindings', {})
        with open('/tmp/resonance-keybindings.txt', 'w') as f:
            for kb, action in keybindings.items():
                f.write(f"{kb}={action}\n")
    except Exception as e:
        print(f"Error writing keybindings: {e}")



def save_windows():
    try:
        with open(WINDOWS_PATH, 'w') as f:
            json.dump(windows_by_id, f)
    except Exception:
        pass


def broadcast(msg, clients, exclude_conn=None):
    payload = (msg + '\n').encode()
    for client in list(clients):
        if client == exclude_conn:
            continue
        try:
            client.sendall(payload)
        except Exception:
            pass


def make_send_place(clients):
    def send_place(wid, px, py, pw, ph, floor):
        print(f"[placement] send_place id={wid} x={px} y={py} w={pw} h={ph} floor={floor}", file=sys.stderr, flush=True)
        broadcast(f"place:{wid}\t{px}\t{py}\t{pw}\t{ph}\t{floor}", clients)
    return send_place


def make_send_fullscreen(clients):
    def send_fullscreen(wid, fullscreen):
        print(f"[placement] send_fullscreen id={wid} fullscreen={fullscreen}", file=sys.stderr, flush=True)
        broadcast(f"fullscreen:{wid}\t{1 if fullscreen else 0}", clients)
    return send_fullscreen


def dispatch(msg, config, conn, clients):
    if msg.startswith('env:'):
        parts = msg[len('env:'):].split('=', 1)
        if len(parts) == 2:
            os.environ[parts[0]] = parts[1]
        broadcast(msg, clients, exclude_conn=conn)
        return
    if msg.startswith('window-map:') or msg.startswith('window-geometry:'):
        is_map = msg.startswith('window-map:')
        rest = msg.split(':', 1)[1]
        parts = rest.split('\t')
        if len(parts) >= 8:
            app_id, title, x, y, w, h, win_id_s, fullscreen_s = parts[:8]
            win_id = int(win_id_s)
            fullscreen = fullscreen_s == '1'
            floor_maximized = False
            if len(parts) >= 9:
                floor_maximized = parts[8].strip() == '1'
            geo = {'x': int(x), 'y': int(y), 'w': int(w), 'h': int(h)}

            entry = {'app_id': app_id, 'title': title, 'fullscreen': fullscreen, 'floor_maximized': floor_maximized, **geo}
            old_entry = windows_by_id.get(win_id)
            was_unknown = old_entry and old_entry.get('app_id') == 'unknown'

            windows_by_id[win_id] = entry
            save_windows()
            if is_map:
                placement.on_window_map(win_id, entry, windows_by_id,
                                         make_send_place(clients), make_send_fullscreen(clients))
            else:
                if was_unknown and app_id != 'unknown':
                    print(f"[brain] Window {win_id} transitioned from 'unknown' to '{app_id}'. Remapping.", file=sys.stderr, flush=True)
                    placement.on_window_unmap(win_id)
                    windows_by_id[win_id] = entry
                    placement.on_window_map(win_id, entry, windows_by_id,
                                             make_send_place(clients), make_send_fullscreen(clients))
                else:
                    placement.on_window_geometry(win_id, entry)
        broadcast(msg, clients, exclude_conn=conn)
        return
    if msg.startswith('release:'):
        # Interactive drag/resize just ended (mouse released) - see
        # reset_cursor_mode in resonance.c. Re-decide floor and re-clamp padding
        # right now instead of waiting on placement.tick()'s idle timeout.
        win_id_s = msg[len('release:'):].strip()
        if win_id_s.isdigit():
            placement.on_window_release(int(win_id_s), windows_by_id, make_send_place(clients))
        broadcast(msg, clients, exclude_conn=conn)
        return
    if msg.startswith('window-unmap:'):
        win_id_s = msg[len('window-unmap:'):].strip()
        if win_id_s.isdigit():
            win_id = int(win_id_s)
            windows_by_id.pop(win_id, None)
            placement.on_window_unmap(win_id, windows_by_id,
                                      make_send_place(clients), make_send_fullscreen(clients))
            save_windows()
        broadcast(msg, clients, exclude_conn=conn)
        return
    if msg.startswith('place:') or msg.startswith('fullscreen:'):
        broadcast(msg, clients, exclude_conn=conn)
        return
    if msg.startswith('cursor-pos:'):
        # Global cursor position feed from resonance - general-purpose, not
        # tied to any one consumer. Pure pass-through broadcast.
        broadcast(msg, clients, exclude_conn=conn)
        return

    # Handle volume actions asynchronously but blocking so changes commit before broadcast
    if msg == 'volume:up':
        subprocess.run(['wpctl', 'set-volume', '-l', '1.0', '@DEFAULT_AUDIO_SINK@', '5%+'],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        broadcast('volume_changed', clients)
        return
    if msg == 'volume:down':
        subprocess.run(['wpctl', 'set-volume', '@DEFAULT_AUDIO_SINK@', '5%-'],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        broadcast('volume_changed', clients)
        return
    if msg == 'volume:mute':
        subprocess.run(['wpctl', 'set-mute', '@DEFAULT_AUDIO_SINK@', 'toggle'],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        broadcast('volume_changed', clients)
        return

    cmd = None
    if msg.startswith('keybind:'):
        cmd = config.get('keybindings', {}).get(msg[len('keybind:'):])
    elif msg.startswith('gesture:'):
        cmd = config.get('gestures', {}).get(msg[len('gesture:'):])
    if cmd:
        subprocess.Popen(cmd, shell=True, start_new_session=True)


def main():
    import selectors
    write_keybindings_file()
    if os.path.exists(SOCKET_PATH):
        os.unlink(SOCKET_PATH)

    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(SOCKET_PATH)
    srv.listen(5)
    srv.setblocking(False)

    sel = selectors.DefaultSelector()
    sel.register(srv, selectors.EVENT_READ, data=None)

    clients = {}  # conn -> buffer

    while True:
        try:
            events = sel.select(timeout=0.5)
            placement.tick(windows_by_id, make_send_place(clients))
            placement.check_splash_timeouts(windows_by_id, make_send_place(clients), make_send_fullscreen(clients))
            placement.check_unknown_transitions(windows_by_id, make_send_place(clients), make_send_fullscreen(clients))
            for key, mask in events:
                if key.data is None:
                    # Accept new connection
                    conn, _ = key.fileobj.accept()
                    conn.setblocking(False)
                    sel.register(conn, selectors.EVENT_READ, data=conn)
                    clients[conn] = ""
                else:
                    conn = key.data
                    try:
                        data = conn.recv(256)
                        if not data:
                            # Connection closed
                            sel.unregister(conn)
                            conn.close()
                            clients.pop(conn, None)
                            continue
                        clients[conn] += data.decode(errors='ignore')
                        buf = clients[conn]
                        while '\n' in buf:
                            line, buf = buf.split('\n', 1)
                            line = line.strip()
                            if line:
                                config = load_config()
                                dispatch(line, config, conn, clients.keys())
                        clients[conn] = buf
                    except Exception:
                        sel.unregister(conn)
                        try:
                            conn.close()
                        except Exception:
                            pass
                        clients.pop(conn, None)
        except Exception:
            pass


if __name__ == '__main__':
    main()
