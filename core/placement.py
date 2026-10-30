import json
import os
import time
from pathlib import Path

SETTLE_SECS = 3.0
MATCH_TOLERANCE_PX = 8

# app_ids known to throw up a splash/loading/login window before their real
# main window maps. Rather than guessing which window is the splash from its
# title (title text isn't reliable - e.g. OrcaSlicer's splash and main window
# both report 'unknown'), the first window-map seen for one of these app_ids
# since it was last fully closed is treated as unconfirmed and suppressed
# entirely; the second is trusted as the real window.
# 'steam', 'steam_app_*' and 'factorio' used to be listed here. They opt out
# of placement entirely now (see is_ignored, which returns before the splash
# gate runs), so listing them would have no effect - a window we never place
# doesn't need its splash telling apart from its real window.
SPLASH_APPS = {'discord', 'Audacity', 'OrcaSlicer', 'org.kde.kdenlive'}

# How long to wait for a second window-map before giving up on splash
# detection and treating the first window as the real one. Discord's
# splash behaviour is non-deterministic (depends on cache state,
# updates, etc.) - sometimes there's no splash at all.
SPLASH_TIMEOUT_SECS = 2.0

# SPLASH_APPS whose first window is interactive (Kdenlive's project picker),
# so it can sit open far longer than SPLASH_TIMEOUT_SECS. Timing out would
# promote the picker to the real window and re-place it mid-use. These never
# time out - the first window stays unplaced until a second one maps.
SPLASH_NO_TIMEOUT = {'org.kde.kdenlive'}

# app_ids known to be generic/placeholder rather than a real app identity -
# e.g. some GTK dialogs report "GTK Application" instead of inheriting their
# owning app's real app_id, when they also don't set a formal Wayland parent
# (resonance.c's xdg_toplevel.parent check already filters out the vast
# majority of dialogs correctly - see server_new_xdg_toplevel's dialog
# handling; this only covers the rare unparented remainder). Scoped
# narrowly on purpose: only these specific app_ids get the "one window at a
# time" gate below - every other app_id (including ones that legitimately
# open multiple simultaneous windows, like a terminal) is tracked
# independently per-window, not collapsed into one. Add to this list if
# another generic/unparented offender turns up.
# Thunar is here because its rename/properties dialogs come through
# unparented under the same app_id, so they'd otherwise get floored and
# resized into a remembered main-window slot.
GENERIC_APP_IDS = {'GTK Application', 'thunar', 'Thunar'}

# app_ids that opt out of placement entirely - never repositioned, never
# fullscreened by us, never written to placement memory. They map where they
# like and stay there.
#
# Everything Steam: the client itself, and every game launched through it
# (games report 'steam_app_<id>', or fall back to 'steam_app_default' when
# they report nothing usable). Neither wants what the floor system offers.
# A game wants one whole output to itself, uninset and undecorated, and
# fights floor padding and floor-maximize when it doesn't get it; the Steam
# client wants to be dead center, not tucked into a floor slot. Both were
# previously placed as ordinary apps, which is how a remembered
# 'fullscreen': true leaked onto windows that should never have been
# fullscreen - Steam's own dialogs inheriting the client's saved state, and
# every unidentified game sharing the single 'steam_app_default' slot.
IGNORED_APP_PREFIXES = ('steam',)

# Games that report their own name instead of a steam_app_<id>, so the prefix
# above doesn't catch them. There's no reliable way to tell a game from any
# other app by app_id alone, so this is just a hand-kept list - add to it as
# games turn up.
IGNORED_APP_IDS = {'factorio', 'cyberpunk2077.exe'}


def is_ignored(app_id):
    """Whether this app_id opts out of placement. Steam's own app_ids are
    prefix-matched so games we've never seen before are covered on sight;
    everything else has to be named in IGNORED_APP_IDS."""
    return app_id.startswith(IGNORED_APP_PREFIXES) or app_id in IGNORED_APP_IDS


PLACEMENT_PATH = Path.home() / '.local' / 'share' / 'resonance' / 'placement.json'

# Mirrors the hardcoded output layout in compositor/resonance.c's
# server_new_output - kept in sync manually since resonance doesn't send output
# identity over IPC. Used to tag remembered geometry with which monitor it
# belongs to, so a position recorded on one output never gets replayed onto
# a window that's actually mapped on a different one.
OUTPUTS = [
    {'name': 'DP-1', 'x': 0, 'y': 0, 'w': 2560, 'h': 1440},
    {'name': 'DP-2', 'x': 2560, 'y': 180, 'w': 1920, 'h': 1080},
]


def output_for_geometry(geo):
    """Which output's bounds fully contain this window - None if it straddles
    a boundary (e.g. mid-drag, or an app-side positioning glitch), since that's
    not a real, trustworthy position to either record or replay."""
    x0, y0 = geo['x'], geo['y']
    x1, y1 = x0 + geo['w'], y0 + geo['h']
    for output in OUTPUTS:
        ox0, oy0 = output['x'], output['y']
        ox1, oy1 = ox0 + output['w'], oy0 + output['h']
        if ox0 <= x0 and x1 <= ox1 and oy0 <= y0 and y1 <= oy1:
            return output['name']
    return None


def output_for_center(geo):
    """Which output's bounds contain this window's center point - unlike
    output_for_geometry, doesn't require full containment. Used to pick
    which floor bounds to clamp a dragged-past-the-edge window back into,
    where full containment is exactly the thing that's currently false."""
    cx = geo['x'] + geo['w'] // 2
    cy = geo['y'] + geo['h'] // 2
    for output in OUTPUTS:
        if (output['x'] <= cx < output['x'] + output['w'] and
                output['y'] <= cy < output['y'] + output['h']):
            return output['name']
    return None


state = {'app_geometry': {}}
pending = {}    # window id -> {app_id, geometry, updated_at}

# Whichever window for an app_id mapped first, while nothing else of that
# app_id was mapped, is the "main" window - placement memory only ever
# applies to or records from that one. Any other window sharing the same
# app_id (dialogs, popups, rename boxes, etc.) is left for the app to place
# itself, regardless of whether it happens to set a wlr transient parent.
primary_window = {}  # app_id -> win_id
window_app = {}      # win_id -> app_id, for every window seen via on_window_map

# For a SPLASH_APPS app_id, the (win_id, timestamp) of its first,
# unconfirmed window-map since the app was last fully closed - suppressed
# from placement entirely until a second, distinct window-map arrives or
# SPLASH_TIMEOUT_SECS elapses (see check_splash_timeouts).
splash_pending = {}  # app_id -> (win_id, monotonic_timestamp)

# Floor system: which floor each primary window currently lives on. Only
# primary windows get an entry - dialogs/secondary windows are left out on
# purpose (see decide_floor), same as they're already left out of placement
# memory. Basement is never assigned here; it's manual/hardcoded, outside
# this overlap logic entirely. Not persisted - re-decided fresh each time a
# window maps.
window_floor = {}  # win_id -> 'ground' | 'first' | 'second'

# Cascade order decide_floor promotes through - each floor is tried in
# order, and a window lands on the first one where it doesn't overlap
# another primary window already occupying that same floor.
FLOOR_ORDER = ['ground', 'first', 'second']

# The 3-axis LUT: state['app_geometry'][app_id][floor] is a list of
# remembered slots (each a geometry dict), one per simultaneously-open
# window of that app_id on that floor. A window claims the first unclaimed
# slot for its (app_id, floor) at map time, in open order - so if you
# always open your two ground-floor terminals in the same order, they each
# keep landing back in their own slot. Claims are in-memory only, per
# session - they reset on restart, so the cascade always starts fresh from
# slot 0 as windows reopen. Not persisted, unlike the slots themselves.
window_slot = {}     # win_id -> (app_id, floor, slot_index)
claimed_slots = {}   # (app_id, floor) -> set of claimed slot indices


def claim_slot(win_id, app_id, floor):
    """Claims the first unclaimed existing slot for (app_id, floor), or a
    fresh index one past the end of the list if every existing slot is
    already claimed by other currently-open windows. Returns (index,
    remembered_geo_or_None) - None means this is a brand new slot with
    nothing recorded in it yet."""
    slots = state['app_geometry'].setdefault(app_id, {}).setdefault(floor, [])
    key = (app_id, floor)
    taken = claimed_slots.setdefault(key, set())
    for i, slot in enumerate(slots):
        if i not in taken:
            taken.add(i)
            window_slot[win_id] = (app_id, floor, i)
            return i, slot
    i = len(slots)
    taken.add(i)
    window_slot[win_id] = (app_id, floor, i)
    return i, None


def release_slot(win_id):
    """Frees this window's claimed slot for another window of the same
    (app_id, floor) to claim - the remembered geometry itself stays in the
    LUT untouched, only the in-session claim is released."""
    info = window_slot.pop(win_id, None)
    if info:
        app_id, floor, i = info
        claimed_slots.get((app_id, floor), set()).discard(i)


def record_slot(win_id, app_id, floor, geo):
    """Writes geo into this window's claimed slot. If the window's floor
    changed since it last claimed a slot (or it never claimed one), releases
    any old claim and claims a fresh slot under the current (app_id, floor)
    first. Returns True if the LUT changed."""
    info = window_slot.get(win_id)
    if not info or info[0] != app_id or info[1] != floor:
        release_slot(win_id)
        claim_slot(win_id, app_id, floor)
        info = window_slot[win_id]
    _, _, i = info
    slots = state['app_geometry'].setdefault(app_id, {}).setdefault(floor, [])
    while len(slots) <= i:
        slots.append(None)
    if slots[i] != geo:
        slots[i] = geo
        return True
    return False


def rects_overlap(a, b):
    return (a['x'] < b['x'] + b['w'] and b['x'] < a['x'] + a['w'] and
            a['y'] < b['y'] + b['h'] and b['y'] < a['y'] + a['h'])


# Per-floor padding from the output's edges, in pixels. Measured 2026-07-10:
# 30px kept a ground-floor Ghostty window's right edge comfortably clear of
# the display edge; each floor above adds another 30px so it reads as
# further "in" than the one below it. Basement has none - it's unrouted for
# now. Placeholder values, expected to get tuned by eye once floors are
# actually in daily use.
FLOOR_INSETS = {'ground': 30, 'first': 60, 'second': 90}


def floor_bounds(output_name, floor):
    """The on-screen rect a window on this floor is allowed to occupy -
    the output's bounds shrunk by that floor's inset on all four sides."""
    inset = FLOOR_INSETS.get(floor, 0)
    for output in OUTPUTS:
        if output['name'] == output_name:
            return {
                'x': output['x'] + inset,
                'y': output['y'] + inset,
                'w': output['w'] - 2 * inset,
                'h': output['h'] - 2 * inset,
            }
    return None


def clamp_to_floor(geo, floor, output_name):
    """Shrinks geo to fit inside this floor's padded bounds - intersection
    of the window's rect with the bounds, so it only ever shrinks a window
    that overflows, never grows one that's already smaller."""
    bounds = floor_bounds(output_name, floor)
    if not bounds:
        return geo
    left = max(geo['x'], bounds['x'])
    top = max(geo['y'], bounds['y'])
    right = min(geo['x'] + geo['w'], bounds['x'] + bounds['w'])
    bottom = min(geo['y'] + geo['h'], bounds['y'] + bounds['h'])
    return {
        'x': left,
        'y': top,
        'w': max(0, right - left),
        'h': max(0, bottom - top),
    }


def is_primary(win_id):
    """Every window is its own primary, tracked independently, except for
    GENERIC_APP_IDS and SPLASH_APPS - those still gate down to one confirmed
    window at a time via primary_window. GENERIC_APP_IDS because app_id
    alone can't tell their windows apart; SPLASH_APPS because they're
    already known to throw up extra non-main windows beyond the confirmed
    real one, and window_app gets set for those (including the suppressed
    splash window itself) before on_window_map's splash gate ever runs -
    checking window_app alone isn't enough to exclude it."""
    app_id = window_app.get(win_id)
    if app_id is None:
        return False
    if is_ignored(app_id):
        # Opted out of placement, so it has no floor of its own and never
        # acts as an obstacle in decide_floor either - otherwise a fullscreen
        # game covering DP-1 would cascade every window opened afterwards up
        # a floor to avoid overlapping it.
        return False
    if app_id in GENERIC_APP_IDS or app_id in SPLASH_APPS:
        return primary_window.get(app_id) == win_id
    return True


def decide_floor(win_id, geo, all_windows):
    """Ground floor by default. Cascades up through FLOOR_ORDER one tier at
    a time: a window lands on the first floor where it doesn't overlap any
    other primary window that's already sitting on that same floor - no
    importance/context weighing (that's a later problem). If every floor
    has a collision, it lands on the topmost one anyway rather than
    inventing a floor beyond FLOOR_ORDER.

    Only primary windows are ever obstacles - dialogs/popups/secondary
    windows are excluded, same as they're excluded from placement memory
    and never get a floor of their own. A window with no window_floor entry
    (e.g. a primary window that was already open before this process
    started, like after a brain.py restart) is treated as ground floor -
    mirrors resonance's own default, which is ground floor unless told
    otherwise - rather than being invisible to every tier's check."""
    for floor in FLOOR_ORDER:
        collision = False
        for other_id, other_geo in all_windows.items():
            if other_id == win_id:
                continue
            if not is_primary(other_id):
                continue
            if window_floor.get(other_id, 'ground') != floor:
                continue
            if rects_overlap(geo, other_geo):
                collision = True
                break
        if not collision:
            return floor
    return FLOOR_ORDER[-1]


def load():
    global state
    try:
        with open(PLACEMENT_PATH) as f:
            data = json.load(f)
        if isinstance(data, dict) and isinstance(data.get('app_geometry'), dict):
            state = data
    except Exception:
        state = {'app_geometry': {}}

    # Backfill/validate entries recorded before output-awareness (or the
    # floor system, or the slot-list LUT) existed, migrating each older
    # shape forward: tag entries with the output they actually fit in, or
    # drop them if they straddle a boundary - untagged entries would
    # otherwise bypass the monitor-match guard entirely.
    app_geometry = state.get('app_geometry', {})
    for app_id in list(app_geometry):
        entry = app_geometry[app_id]
        if not isinstance(entry, dict):
            app_geometry.pop(app_id)
            continue
        # Migrate pre-floor-system entries (app_id -> geometry directly) into
        # the current per-floor form (app_id -> {floor: ...}), assuming
        # ground floor for anything recorded before floors existed.
        if 'x' in entry:
            entry = {'ground': entry}
            app_geometry[app_id] = entry
        for floor in list(entry):
            floor_entry = entry[floor]
            # Migrate pre-LUT entries (a single geometry dict per floor) into
            # the current slot-list form (a list of geometry dicts, one per
            # simultaneously-open window of this app on this floor).
            if isinstance(floor_entry, dict):
                floor_entry = [floor_entry]
            if not isinstance(floor_entry, list):
                del entry[floor]
                continue
            slots = []
            for slot in floor_entry:
                if not isinstance(slot, dict):
                    continue
                if 'output' not in slot:
                    output = output_for_geometry(slot)
                    if output is None:
                        continue
                    slot['output'] = output
                slot.setdefault('fullscreen', False)
                slots.append(slot)
            if slots:
                entry[floor] = slots
            else:
                del entry[floor]
        if not entry:
            app_geometry.pop(app_id)


def save():
    try:
        PLACEMENT_PATH.parent.mkdir(parents=True, exist_ok=True)
        tmp_path = PLACEMENT_PATH.with_suffix('.json.tmp')
        with open(tmp_path, 'w') as f:
            json.dump(state, f)
        os.replace(tmp_path, PLACEMENT_PATH)
    except Exception:
        pass


def geometry_from_entry(entry):
    return {
        'x': int(entry['x']),
        'y': int(entry['y']),
        'w': int(entry['w']),
        'h': int(entry['h']),
    }


def geometry_matches(a, b):
    return all(abs(int(a[k]) - int(b[k])) <= MATCH_TOLERANCE_PX for k in ('x', 'y', 'w', 'h'))


def on_window_map(win_id, entry, all_windows, send_place, send_fullscreen,
                  splash_promoted=False):
    app_id = entry.get('app_id') or ''
    if not app_id:
        return
    if app_id == 'unknown':
        window_app[win_id] = 'unknown'
        return
    window_app[win_id] = app_id

    # Opted out of placement - bookmarked above so unmap still cleans up
    # after it, but never positioned, floored, or remembered.
    if is_ignored(app_id):
        return

    if app_id in SPLASH_APPS and app_id not in primary_window:
        pending = splash_pending.get(app_id)
        if pending is None:
            # First sign of life since this app was last fully closed - could
            # be a splash/loading/login window. Don't trust it; wait for a
            # second, distinct window-map before touching placement. If none
            # arrives within SPLASH_TIMEOUT_SECS, check_splash_timeouts
            # promotes it to the real window.
            splash_pending[app_id] = (win_id, time.monotonic())
            return
        pending_id, _ = pending
        if pending_id == win_id:
            # Duplicate map for the same still-unconfirmed window.
            return
        # A second, distinct window-map for this app_id - confirmed real.
        del splash_pending[app_id]

    if app_id not in primary_window:
        # First window seen for this app_id - remembered regardless of
        # whether app_id is gated, so a later window of a SPLASH_APPS app
        # (e.g. a second Discord window) doesn't re-trigger splash
        # suppression above. For SPLASH_APPS this is always the confirmed
        # real window, never the suppressed splash one - that one returned
        # early above and never reaches this line.
        primary_window[app_id] = win_id
    elif (app_id in GENERIC_APP_IDS or app_id in SPLASH_APPS) and primary_window[app_id] != win_id:
        # A window for this app_id is already open, and this app_id is
        # known to only ever have one real tracked window at a time -
        # either a generic/placeholder identity (an unparented dialog riding
        # on the same placeholder app_id) or a SPLASH_APPS app (which throws
        # up extra non-main windows beyond the confirmed real one). Leave it
        # alone entirely, including floor - it's excluded from decide_floor's
        # overlap check too (see is_primary). Every other app_id tracks each
        # window independently.
        return

    current = geometry_from_entry(entry)

    # Floor: decided from this window's live on-screen position at map time
    # (freshly mapped/centered, before any remembered geometry is applied) -
    # it's about what's actually overlapping right now, not a hypothetical
    # remembered spot. Only once the floor is known do we look up that
    # floor's own remembered geometry, below.
    #
    # Exception: SPLASH_APPS windows never map where they belong. Whether the
    # splash was promoted on timeout or confirmed by a second window-map, the
    # real window sat suppressed while resonance's 250ms grace timer centered it
    # on whichever output the cursor happened to be on. That position is
    # stale either way, so the overlap check would decide the floor from
    # cursor noise. Decide from the remembered ground-floor geometry instead,
    # so the check runs against the layout the window is actually heading
    # toward.
    floor_geo = current
    if app_id in SPLASH_APPS:
        ground_slots = state['app_geometry'].get(app_id, {}).get('ground', [])
        if ground_slots and ground_slots[0] is not None:
            floor_geo = geometry_from_entry(ground_slots[0])
    floor = decide_floor(win_id, floor_geo, all_windows)
    window_floor[win_id] = floor
    floor_changed = floor != 'ground'  # 'ground' is resonance's own default

    _, remembered = claim_slot(win_id, app_id, floor)

    # Display first: nothing below matters until we know which output this
    # app belongs on.
    if not remembered:
        clamped = clamp_to_floor(current, floor, output_for_geometry(current))
        if floor_changed or clamped != current:
            send_place(win_id, clamped['x'], clamped['y'], clamped['w'], clamped['h'], floor)
        return
    target_output = remembered.get('output')

    currently_fullscreen = bool(entry.get('fullscreen'))
    remembered_fullscreen = bool(remembered.get('fullscreen'))

    # Fork: fullscreen, or not?
    if remembered_fullscreen:
        # Exact x/y/w/h stop mattering - fullscreen overrides them - only
        # which output it ends up fullscreen on does.
        needs_move = bool(target_output) and output_for_geometry(current) != target_output

        if not needs_move and currently_fullscreen and not floor_changed:
            return

        if needs_move or floor_changed:
            # resonance drops fullscreen as a side effect of a place: command,
            # so a move always needs a fullscreen: right after it to undo
            # that - never trust currently_fullscreen once we've moved it.
            send_place(win_id, remembered['x'], remembered['y'], remembered['w'], remembered['h'], floor)
        send_fullscreen(win_id, True)
        return

    # Not fullscreen: fall through to exact size and position - including
    # monitor, since a fresh map with the wrong monitor is exactly the case
    # we're supposed to correct.
    target = clamp_to_floor(geometry_from_entry(remembered), floor, target_output)
    apply_geo = not geometry_matches(current, target)
    apply_fullscreen = currently_fullscreen != remembered_fullscreen

    if not apply_geo and not apply_fullscreen and not floor_changed:
        return

    if apply_geo or floor_changed:
        send_place(win_id, target['x'], target['y'], target['w'], target['h'], floor)
    if apply_fullscreen:
        send_fullscreen(win_id, False)


def on_window_geometry(win_id, entry):
    app_id = entry.get('app_id') or ''
    if not app_id:
        pending.pop(win_id, None)
        return
    # A window already open when brain.py started never went through
    # on_window_map, so window_app has no entry for it - is_primary
    # returns False and decide_floor treats it as absent. Bookmark it
    # on the first geometry update so it becomes a real obstacle again.
    if win_id not in window_app and app_id != 'unknown':
        window_app[win_id] = app_id
    if is_ignored(app_id) or not is_primary(win_id):
        # Opted out, or a secondary/child window - never recorded into
        # placement memory. Staying out of `pending` also keeps tick() from
        # settling it later.
        pending.pop(win_id, None)
        return

    geo = geometry_from_entry(entry)

    pending[win_id] = {
        'app_id': app_id,
        'floor': window_floor.get(win_id, 'ground'),
        'geometry': geo,
        'fullscreen': bool(entry.get('fullscreen')),
        'floor_maximized': bool(entry.get('floor_maximized')),
        'updated_at': time.monotonic(),
    }


def on_window_unmap(win_id, all_windows=None, send_place=None, send_fullscreen=None):
    pending.pop(win_id, None)
    window_floor.pop(win_id, None)
    release_slot(win_id)
    app_id = window_app.pop(win_id, None)
    if app_id is not None:
        lost_primary = primary_window.get(app_id) == win_id
        if lost_primary:
            del primary_window[app_id]
        if splash_pending.get(app_id, (None,))[0] == win_id:
            del splash_pending[app_id]

        # A gated app's primary just closed while another of its windows is
        # still open - e.g. Kdenlive's project picker -> loading screen ->
        # editor, where the loading screen gets confirmed as primary and the
        # editor maps while it's still up, so it's skipped as secondary.
        # Without this the editor stays untracked (never floored or clamped)
        # for the rest of its life. Hand primary to the survivor and place it.
        if (lost_primary and all_windows is not None
                and (app_id in GENERIC_APP_IDS or app_id in SPLASH_APPS)):
            for other_id, other_app in window_app.items():
                if other_app == app_id and other_id in all_windows:
                    primary_window[app_id] = other_id
                    on_window_map(other_id, all_windows[other_id], all_windows,
                                  send_place, send_fullscreen)
                    break

        # A closed window can leave whatever was raised above it floating
        # over empty space.
        if send_place is not None:
            demote_uncovered(all_windows, send_place)


def _settle_window(win_id, app_id, floor, settled_geo, fullscreen, floor_maximized, windows, send_place):
    """Re-decides floor from where a window actually ended up (not just
    wherever it was at map time) and clamps it to that floor's padding, then
    records the result into placement memory. Shared by tick() (fires after
    SETTLE_SECS of no geometry change - the fallback path) and
    on_window_release() (fires immediately on mouse-up, the normal path).
    Returns True if placement memory changed (caller decides whether to
    save())."""
    if not fullscreen and not floor_maximized:
        # A window dragged onto something occupying its current floor
        # promotes one tier up, same as if it had opened there; dragged
        # clear of everything, it cascades back down toward ground.
        new_floor = decide_floor(win_id, settled_geo, windows)
        floor_changed = new_floor != floor
        if floor_changed:
            floor = new_floor
            window_floor[win_id] = floor

        # If it now hangs past this floor's padding, pull the offending
        # edge(s) back in. clamp_to_floor only touches edges that actually
        # overflow, so a window dragged partway over the line doesn't get
        # moved, just shrunk back to the boundary. Dragged past an edge
        # entirely (not just straddling two monitors) means
        # output_for_geometry can't find a fully-containing output - exactly
        # the case this clamp needs to fire for - so fall back to whichever
        # output the window's center is over.
        clamp_output = output_for_geometry(settled_geo) or output_for_center(settled_geo)
        if clamp_output is not None:
            clamped = clamp_to_floor(settled_geo, floor, clamp_output)
            # Only correct once the overshoot is more than a couple pixels -
            # same MATCH_TOLERANCE_PX used everywhere else in here, so a
            # barely-off drag doesn't trigger a pointless imperceptible snap.
            if floor_changed or not geometry_matches(clamped, settled_geo):
                send_place(win_id, clamped['x'], clamped['y'], clamped['w'], clamped['h'], floor)
                settled_geo = clamped

    # This window moving may have uncovered something raised above it.
    demote_uncovered(windows, send_place)

    output = output_for_geometry(settled_geo)
    if output is None:
        return False
    recorded = dict(settled_geo)
    recorded['output'] = output
    recorded['fullscreen'] = fullscreen
    recorded['floor_maximized'] = floor_maximized
    return record_slot(win_id, app_id, floor, recorded)


def demote_uncovered(windows, send_place):
    """Drops every raised window down to the lowest floor where it no longer
    overlaps anything - a floor is only ever re-decided for the window that
    moved, so without this a window left sitting over empty space (because
    whatever was under it moved or closed) would stay raised until touched.
    Demote-only: a still window never gets promoted by someone else's move,
    the push mechanic covers that side. Lower floors go first so a window
    dropping out of the way is already settled before the ones above it."""
    raised = [wid for wid, f in window_floor.items() if f != 'ground']
    raised.sort(key=lambda wid: FLOOR_ORDER.index(window_floor[wid]))
    for wid in raised:
        entry = windows.get(wid)
        if not entry or not is_primary(wid):
            continue
        if entry.get('fullscreen') or entry.get('floor_maximized'):
            continue
        geo = geometry_from_entry(entry)
        current = window_floor[wid]
        new_floor = decide_floor(wid, geo, windows)
        if FLOOR_ORDER.index(new_floor) < FLOOR_ORDER.index(current):
            window_floor[wid] = new_floor
            send_place(wid, geo['x'], geo['y'], geo['w'], geo['h'], new_floor)


def on_window_release(win_id, windows, send_place):
    """Fires the moment an interactive drag/resize ends (mouse release) -
    see reset_cursor_mode in resonance.c, which sends a release: IPC message
    right when cursor_mode returns to passthrough. Settles immediately
    instead of waiting on tick()'s idle timeout."""
    item = pending.pop(win_id, None)
    entry = windows.get(win_id)
    if not entry:
        return
    app_id = entry.get('app_id') or ''
    if not app_id or is_ignored(app_id) or not is_primary(win_id):
        return
    if item and item['app_id'] != app_id:
        item = None
    floor = item['floor'] if item else window_floor.get(win_id, 'ground')
    fullscreen = bool(entry.get('fullscreen'))
    floor_maximized = bool(entry.get('floor_maximized'))
    settled_geo = geometry_from_entry(entry)
    if _settle_window(win_id, app_id, floor, settled_geo, fullscreen, floor_maximized, windows, send_place):
        save()


def check_splash_timeouts(all_windows, send_place, send_fullscreen):
    """Promotes suppressed splash windows to real ones after
    SPLASH_TIMEOUT_SECS. Some apps (Discord especially) don't always
    show a splash - if no second window arrives to confirm, the first
    window IS the real one and placement should fire on it."""
    now = time.monotonic()
    for app_id in list(splash_pending):
        if app_id in SPLASH_NO_TIMEOUT:
            continue
        win_id, ts = splash_pending[app_id]
        if now - ts < SPLASH_TIMEOUT_SECS:
            continue
        del splash_pending[app_id]
        entry = all_windows.get(win_id)
        if entry:
            primary_window[app_id] = win_id
            on_window_map(win_id, entry, all_windows, send_place,
                          send_fullscreen, splash_promoted=True)


def check_unknown_transitions(windows, send_place, send_fullscreen):
    """A window that first mapped with app_id 'unknown' (see on_window_map)
    only gets a second chance via the geometry-resend path in brain.py's
    dispatch - but resonance only resends geometry when the window's size
    actually changes, which many games never do after their initial map.
    Catches that gap: on every tick, checks whether any window still
    bookmarked as 'unknown' has since resolved to a real app_id in the
    live windows dict (which brain.py updates unconditionally on every
    message, regardless of placement), and if so, replays on_window_map
    for it now instead of waiting forever for a resize that may never come."""
    for win_id, app_id in list(window_app.items()):
        if app_id != 'unknown':
            continue
        entry = windows.get(win_id)
        if not entry:
            continue
        real_app_id = entry.get('app_id') or ''
        if real_app_id and real_app_id != 'unknown':
            on_window_unmap(win_id)
            on_window_map(win_id, entry, windows, send_place, send_fullscreen)


def tick(windows, send_place):
    now = time.monotonic()
    changed = False
    for win_id, item in list(pending.items()):
        entry = windows.get(win_id)
        if not entry:
            pending.pop(win_id, None)
            continue
        if entry.get('app_id') != item['app_id']:
            pending.pop(win_id, None)
            continue
        current = geometry_from_entry(entry)
        current_fullscreen = bool(entry.get('fullscreen'))
        current_floor_maximized = bool(entry.get('floor_maximized'))
        if not geometry_matches(current, item['geometry']) or current_fullscreen != item['fullscreen'] or current_floor_maximized != item['floor_maximized']:
            pending[win_id] = {
                'app_id': item['app_id'],
                'floor': item['floor'],
                'geometry': current,
                'fullscreen': current_fullscreen,
                'floor_maximized': current_floor_maximized,
                'updated_at': now,
            }
            continue
        if now - item['updated_at'] < SETTLE_SECS:
            continue

        if _settle_window(win_id, item['app_id'], item['floor'], item['geometry'],
                           item['fullscreen'], item['floor_maximized'], windows, send_place):
            changed = True
        pending.pop(win_id, None)

    if changed:
        save()


load()
