# Vendored piHPSDR wire contract

`pihpsdr-client` speaks piHPSDR's internal client/server protocol. That protocol
is an **exact-match, packed-struct ABI** with no length fields on the wire — the
size of every payload is implied by its message type. To stay byte-compatible we
vendor piHPSDR's `client_server.h` **verbatim** rather than re-declaring the
structs by hand.

## Source of truth

| | |
|---|---|
| Upstream | https://github.com/dl1ycf/pihpsdr |
| Commit | `974acbac07fe7dd3e24f28f3956a9ffb3a1ebaf1` (`974acba`) |
| File | `src/client_server.h` |
| `CLIENT_SERVER_VERSION` | `0x01300005` |
| sha256 | `6defe2fc254dfaa40e07828dacdb21efc0f49189b9e9c7bfbc493bd012673ff6` |

`client_server.h` here is a **byte-for-byte copy** — the sha256 above must match
the upstream file. Do not edit it. On a piHPSDR update, re-copy the file, update
this table, and confirm the client still connects (the version byte is checked
exactly by the server during the handshake).

## Shims

`client_server.h` `#include`s three piHPSDR headers. It only needs them for a
block of `send_*()` prototypes that take `const RECEIVER *` / `const TRANSMITTER
*`. pihpsdr-client never calls those prototypes — it implements its own command
senders — so instead of vendoring the entire piHPSDR receiver/transmitter
subsystems we provide minimal shims:

- `receiver.h` — opaque `RECEIVER` type
- `transmitter.h` — opaque `TRANSMITTER` type
- `mode.h` — empty

The wire structs, the `_header_type_enum` message enum and the `to_*`/`from_*`
inline encoders are used **as-is from the verbatim header** — the shims do not
touch them, so ABI parity is preserved. `<gtk/gtk.h>` and `<opus/opus.h>` are
kept as-is (both are real project dependencies); the header is included headless
in the protocol modules without ever calling `gtk_init()`.

## discovered.h — the discovery data contract

Protocol-2 discovery (`src/engine/discovery_p2.c`, adapted from piHPSDR's
`new_discovery.c`) fills a `DISCOVERED discovered[]` table. `discovered.h`
defines that struct plus the device/protocol/status enums the radio reports, so
we vendor it **verbatim** rather than re-declaring it.

| | |
|---|---|
| Upstream | https://github.com/dl1ycf/pihpsdr |
| Commit | `974acbac07fe7dd3e24f28f3956a9ffb3a1ebaf1` (`974acba`) |
| File | `src/discovered.h` |
| sha256 | `5f6fc9bd6b2935e78ec0aca97c03412631652fb3961433db60ac6d09f6a62075` |

Verbatim copy — the sha256 must match upstream; do not edit. The globals it
declares (`devices`, `discovered[]`, `selected_device`) are **defined** in
`src/engine/engine_state.c`.

## Re-sync checklist

1. `cp <pihpsdr>/src/client_server.h vendor/pihpsdr/client_server.h`
   and `cp <pihpsdr>/src/discovered.h vendor/pihpsdr/discovered.h`
2. Update commit, version and sha256 in the tables above.
3. `meson compile -C build`; reconnect to a server built from the same commit,
   and re-run `./build/sdrfl-discover`.
