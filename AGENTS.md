# AGENTS.md

## What this repo is
A single C program, `ibus-rime-remote.c` → `ibus-rime-remote` (the Makefile's `BIN`), built with
`make`. It uses **GDBus** (`glib2`) to read/set Rime's `ascii_mode` (中↔英) by calling the Rime
engine object's `org.freedesktop.IBus.Engine.PropertyActivate("InputMode", 0)`. It does **not**
switch IBus engines, does not depend on X11/Wayland, and writes no cache.

CLI: `{get,set}` only, plus `--verbose`. Crafted for scripting/editors: `get` prints exactly `on`
or `off` to stdout, `set` prints nothing on success, and all explanation is opt-in via `--verbose`
(stdout) — exit codes `0`/`1`/`2` (ok / runtime failure / usage). stderr is reserved for real
execution errors only.

The C source is deliberately beginner-friendly: GLib containers (`GPtrArray`/`GString`),
`GOptionContext` for arguments, and a long explanatory header comment.

## Behavior: acts on *one* engine object
- Each `/org/freedesktop/IBus/Engine/<id>` object owns its own Rime session, and `ascii_mode` is
  stored in that session — so it is **per-engine-object**. Verified: calling `PropertyActivate` on
  one object changes only that object.
- This environment has extra "orphan" objects (e.g. `Engine/2`, `Engine/3`) that never receive keys;
  the live one is the highest-id object (e.g. `Engine/6`). There is **no reliable API** to ask which
  object is focused: `InputContext.GetEngine` times out and `Engine.FocusId` is `false` on every
  object.
- The tool therefore acts on exactly **one** object: the one whose path has the **highest id**. That
  is the closest available proxy for "the focused object"; it keeps `get` a single value and avoids
  any "mixed". See `docs/gnome-wayland-ascii-mode-shared.md`.
- In this environment (GNOME/Wayland) all applications share **one** input context, hence one live
  engine object / one Rime session. So "every window changes together" is a property of the desktop
  integration, not of Rime or of this tool.

## Running
- `make` then `./ibus-rime-remote {get,set}` (`make clean` removes the binary).
  Runtime dep: GLib/GIO (`-lgio-2.0`); Arch: `pacman -S glib2`.
- `set` takes a target: `on|off` (aliases `en/zh`, `ascii/中文`, …; see `parse_target`).
- Options: `--engine-name` (default `im.rime.Rime`), `--prop` (default `InputMode`),
  `--address` (override bus address), `--verbose` (extra notes on stdout; silent by default).
- `get` reads the **live** state; `set` is truly idempotent (reads live state first).
- stdout holds only the value: `get` → `on`/`off`, `set` → empty. `--verbose` adds explanatory
  lines (prefixed `[verbose]`) to stdout; stderr is only for real errors.

## Architecture
- Flow: connect to the IBus bus → enumerate `/org/freedesktop/IBus/Engine/<id>` under bus name
  `im.rime.Rime` (introspect the parent node) → pick the highest-id object →
  `org.freedesktop.IBus.Engine.PropertyActivate("InputMode", 0)`.
- The engine bus name equals the component `<name>` in `/usr/share/ibus/component/rime.xml`.
- Bus discovery (`candidate_addresses`): `--address` → `IBUS_ADDRESS` env → files in
  `~/.config/ibus/bus/` → `DBUS_SESSION_BUS_ADDRESS`. Each is tried and a probe
  (`GetNameOwner`, falling back to `CurrentInputContext`) accepts the right bus.
- Live state read (`get`, and `set` before acting): IBus has no `ascii_mode` getter, but the engine
  re-emits `RegisterProperties` on `FocusIn`, and that payload contains the current `InputMode`
  property (`icon` = `.../abc.png` → 西文, `.../zh.png` → 中文; use `symbol` only as a fallback —
  `label`/`symbol` can be polluted by the composition). Procedure: manually `AddMatch` → subscribe
  `RegisterProperties` → `FocusIn` the chosen object → parse the `IBusPropList` (`(sa{sv}av)`, each
  item a serialized `IBusProperty`). Only the single highest-id object is used; `get` prints just
  `on`/`off` (stdout empty, exit 1, if unreadable). `set` flips only when not already at the target
  (idempotent). `FocusIn` does not change `ascii_mode` (it only triggers `RegisterProperties`).
- Degraded path: if no state can be read, the tool blind-flips its single chosen object once
  (best-effort, not idempotent) and only mentions it under `--verbose`.
- Manual equivalent (no tool):
  `busctl --address="$ADDR" call im.rime.Rime /org/freedesktop/IBus/Engine/<id> org.freedesktop.IBus.Engine PropertyActivate su InputMode 0`.
- C source note: `focused_engine_path()` returns a `g_strdup`'d path of the highest-id engine object
  (caller frees) or `NULL` when there are none; `read_engine_state()` subscribes and reads just that
  one object. `engine_paths()` is sorted ascending by id, so the "focused" one is the *last* element.

## Gotchas
- This environment (GNOME/Wayland, `ibus-daemon --panel disable`) does **not** answer the daemon's
  `InputContext.PropertyActivate` / `ProcessKeyEvent` / `GetEngine` / `FocusIn` — they time out even
  at 30s. Do not "fix" things by targeting the daemon IC or the Panel. The **engine object's own**
  `PropertyActivate` and `FocusIn` do work; the latter is what `get` uses.
- `PropertyActivate` always returns success — it is **not** a "state changed" signal. It affects the
  target engine object's own session whether or not that object is focused.
- GDBus's `signal_subscribe` sends its `AddMatch` automatically, but that does **not** work against
  IBus's mini-bus (you receive no signals). The tool sends `org.freedesktop.DBus.AddMatch` manually
  with the same rule before subscribing; the mini-bus does accept `AddMatch` (this is also how
  `dbus-monitor` falls back to eavesdropping).
- IBus's private mini-bus does not implement `ListActivatableNames`; `busctl list` reports a
  misleading `EBADR "Invalid request descriptor"`. Enumerate objects with `busctl tree im.rime.Rime`.
- `g_dbus_connection_new_for_address_sync` does not send the D-Bus `Hello` handshake; it must be sent
  manually, which is required against a standard `dbus-daemon`. The tool does this.
- Stale bus files may carry an expired `guid`; only sd-bus/`busctl` validates it and can fail with
  "Operation not permitted". GDBus ignores the guid, so this tool does **not** strip it.
- The number in the engine object path comes from a **per-process counter** in the engine
  (`ibusfactory.c`: `/org/freedesktop/IBus/Engine/%d`, `++factory->priv->id`). It is unrelated to the
  daemon's `InputContext_<N>` numbering — e.g. focused `InputContext_2` may be served by `Engine/6`.
