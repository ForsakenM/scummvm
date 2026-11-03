# Archipelago bridge

A game-agnostic channel between a running ScummVM engine and an external
randomizer client (typically an [Archipelago](https://archipelago.gg) client).
ScummVM knows nothing about any game here: engines report low-level VM events,
the client decides what they mean. Everything game-specific (which events are
checks, how an item is granted) lives in the client's game definition.

## Enabling

In the launcher, select the game, click **Game Options…** and open the
**Archipelago** tab (shown for SCI and SCUMM games). Tick **Connect to an
Archipelago client**; the client address defaults to `127.0.0.1:38282`.
This is stored per game as `ap_bridge_enabled` / `ap_bridge_address`.

Alternatives for scripts and testing: a raw `ap_bridge=host:port` key, or the
`SCUMMVM_AP_BRIDGE=host:port` environment variable. Settings are read each
time a game starts.

The client listens on that port; ScummVM connects out (libcurl socket) when a
game starts and reconnects every 2 s if the client goes away. Builds without
libcurl warn and run normally.

## Wire format

Newline-delimited JSON, one object per line, both directions.

### Engine → client

| `type`   | Fields | Meaning |
|----------|--------|---------|
| `hello`  | `protocol`, `engine`, `gameid`, `extra`, `target` | Sent on connect and when a game starts. The client should (re)send its rules. |
| `match`  | `rule`, `kind`, `keys`, `args`, `ctx`, `action` | An event matched a `watch` rule. Queued while disconnected. |
| `trace`  | `kind`, `keys`, `args`, `ctx`, `action` | Every event of a traced kind (for mapping a new game). |
| `result` | `cmd`, `req`, `ok`, `value`, `error` | Reply to each command. |
| `bye`    | | The game is shutting down. |

`action` is true when the event was caused by the client's own `do` command.

### Client → engine

| `cmd`     | Fields | Meaning |
|-----------|--------|---------|
| `watch`   | `rule`, `kind`, `keys`, `args`, `ctx`, `veto`, `value`, `once` | Add or replace a rule. `keys` must match position by position, `args` as a prefix; `"*"` is a wildcard in both. `ctx` values must all match. `veto: true` makes the engine skip the event; `value` is the result a vetoed call returns. |
| `unwatch` | `rule` | Remove a rule. |
| `clear`   | | Remove all rules. |
| `trace`   | `kind`, `on` | Stream every event of a kind. Cleared on disconnect. |
| `do`      | `req`, `op`, `keys`, `args`, `asGame` | Run an engine action (below) at the next safe point. With `asGame: true` the action counts as the player's own (rules can veto what it causes), for test harnesses. |
| `say`     | `text` | Show an on-screen message. |
| `ping`    | `req` | Liveness check. |

Rules survive client disconnects, so suppressed pickups stay suppressed; they
are cleared when a new game starts. Events raised by a `do` action are reported
but never vetoed, so "give item" actions are not swallowed by "suppress
pickup" rules.

## SCI (Sierra SCI0–SCI1.1)

Events:

| kind      | keys | args |
|-----------|------|------|
| `call`    | `[script, export]` | procedure arguments |
| `send`    | `[object, selector, class]` | method arguments (vetoable) |
| `setprop` | `[object, selector, class]` | `[new value]` (vetoable: the write is skipped) |
| `getprop` | `[object, selector, class]` | none (vetoable: `value` replaces what the script reads) |
| `kernel`  | `[name]` | kernel call arguments |

`getprop` only sees reads made by message (`(obj prop:)`). An object's own
methods read their properties with direct VM opcodes, which are not reported.
That lets a client change what *other* scripts see, e.g. keep a room showing
an item whose inventory object has already been handed to the player, while
the inventory itself (`has:`, the inventory window) still sees the real value.

Context: `room` (global 11).

Actions: `getGlobal [n]`, `setGlobal [n, v]`, `getBit [base, bit]`,
`setBit [base, bit, 0|1]` (Sierra bit arrays: word `base + bit/16`, mask
`0x8000 >> bit%16`), `getProp`/`setProp keys [obj, selector]`,
`send keys [obj, selector] args [...]`, `room`. Objects are named directly,
as `name@script` when a name is ambiguous, or as `global:N`.

The tick runs inside `kGetEvent`, so `send` actions execute from kernel
context like any other kernel-invoked selector.

## SCUMM (all versions, v0–v8)

Events:

| kind     | keys | args |
|----------|------|------|
| `owner`  | `[object]` | `[newOwner, oldOwner, toEgo]` (vetoable). Every pickup in every SCUMM version goes through `putOwner`. |
| `var`    | `[index]` | `[new, old]` (vetoable) |
| `bitvar` | `[index]` | `[new, old]` (vetoable) |
| `room`   | `[room]` | |
| `script` | `[number]` | (vetoable) |

Context: `room`, `ego`.

Actions: `getVar`, `setVar`, `getBit`, `setBit`, `getOwner`,
`setOwner [obj, owner|-1]`, `give [obj, homeRoom]` (copies the object out of
its home room into the inventory), `take [obj]`, `runScript [n, args...]`,
`room`. The tick runs at the top of `scummLoop`.

## Example: King's Quest VI

```json
{"cmd":"watch","rule":1,"kind":"send","keys":["*","givePoints"]}
{"cmd":"watch","rule":2,"kind":"send","keys":["ego","get"],"veto":true}
{"cmd":"do","req":7,"op":"send","keys":["ego","get"],"args":[45]}
{"cmd":"do","req":8,"op":"setBit","args":[137,3,1]}
```

Rule 1 reports every point award (with `ctx.room`) as a location check, and
rule 2 suppresses the game's own item pickups. The first `do` gives the
spellbook (item 45), which passes rule 2 because it is the client's own action.
The second sets flag 3, which reveals the Isle of the Mists on the map.

## Adding an engine

Implement `APBridge::Adapter` (`engines/apbridge.h`): `engineId`,
`getContext`, `performAction`. Then call `Bridge::notify` from the VM's
natural choke points, `Bridge::tick` once per frame from a point where
running game code is safe, and `attach`/`detach` around the game's lifetime.
