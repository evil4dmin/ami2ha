# ami2ha

A native AmigaOS client for [Home Assistant](https://www.home-assistant.io/).

Control your smart home from a real Amiga: read sensors, watch entity states,
flip switches and dimmers — from a fully configurable MUI dashboard, with an
ARexx port so the rest of your Workbench can join in.

> **Status: early development.** The full stack works — TCP, WebSocket,
> authentication, live updates, and a MUI dashboard driven by a
> configuration file. The ARexx port and the in-app dashboard editor are
> not written yet. **None of it has been run against a real Home Assistant
> yet**, only against tests; see [Roadmap](#roadmap).

## Target systems

| | |
|---|---|
| **Primary** | AmigaOS 3.x, 68020+, MUI 3.8 or newer, a TCP/IP stack offering `bsdsocket.library` (Roadshow, AmiTCP, Miami) |
| **Minimum** | 68000 builds are produced, but JSON over TCP on a stock A500 will be slow |
| **Portable to** | AmigaOS 4, MorphOS and AROS — the sources avoid OS3-only idioms and use the SDI headers for register and hook conventions |

Roughly 2 MB of free RAM is expected for a medium-sized Home Assistant
installation; the exact figure depends on how many entities you subscribe to.

## Architecture at a glance

The project is split along one hard line:

```
src/core/     pure C99, no Amiga headers, no OS calls
              -> buffers, JSON reader, Base64, SHA-1, WebSocket framing
              -> HTTP handshake, Home Assistant client, entity store
              -> compiled into BOTH the Amiga binary and the host test runner

src/net/      bsdsocket.library transport, optional AmiSSL
src/ui/       MUI interface and the dashboard editor        (not written yet)
src/rexx/     ARexx host port                               (not written yet)
src/config/   preferences load/save                         (not written yet)
src/main.c    command line front end
```

Everything that can be tested without an Amiga *is* tested without an Amiga.
`make test` compiles `src/core/` with the host compiler and runs the suite in
about a second, so protocol bugs get caught long before an emulator boots.
The same sources are then cross-compiled for m68k unchanged.

A few decisions worth knowing about:

- **The JSON reader builds no document tree.** A `get_states` reply on a large
  installation is a few hundred kilobytes, and a node-per-value DOM would not
  fit comfortably on a 2 MB machine. Callers walk the document once and copy
  out only the fields they need; tokens are slices into the caller's buffer,
  so parsing allocates nothing.
- **Text is transcoded to Latin-1 on the way in.** Home Assistant speaks
  UTF-8; Amiga fonts do not. Codepoints outside Latin-1 are folded to
  sensible ASCII (curly quotes, dashes, `EUR`) rather than rendered as
  mojibake.
- **There is no floating point anywhere.** Numbers are parsed to fixed-point
  integers. A 68000 has no FPU, so `strtod` would mean pulling
  `mathieeedoubbas.library` into every numeric read; the binary depends on
  nothing but `bsdsocket.library` and `dos.library`.
- **The protocol client owns no socket.** It consumes and produces byte
  buffers, so the whole session — upgrade, handshake, auth, subscription,
  state application — is driven by tests with no network involved.

## Building

You need a Mac (Intel or Apple Silicon) or Linux box. One command sets up the
complete cross-toolchain — vbcc, vasm, vlink, the AmigaOS NDK, and the MUI
developer includes — under `~/opt/amiga`, with no root access required:

```sh
./tools/setup-toolchain.sh
```

Then:

```sh
make          # cross-compile build/ami2ha for m68k
make test     # build and run the portable core tests on this machine
```

See [docs/BUILDING.md](docs/BUILDING.md) for toolchain details, CPU and
optimisation options, and how to enable AmiSSL.

## The command line client

The first runnable milestone is a CLI that exercises the whole stack. It is
useful on its own, and scriptable:

```
ami2ha homeassistant.local TOKENFILE=S:ha.token LIST
ami2ha homeassistant.local TOKENFILE=S:ha.token DOMAIN=light LIST
ami2ha homeassistant.local TOKENFILE=S:ha.token GET=sensor.kitchen_temperature
ami2ha homeassistant.local TOKENFILE=S:ha.token TOGGLE=light.kitchen
ami2ha homeassistant.local TOKENFILE=S:ha.token WATCH
```

`WATCH` follows live state changes until Ctrl-C. Prefer `TOKENFILE` over
`TOKEN`: a token on the command line ends up in your shell history and is
visible in the task list.

## The dashboard

Widgets are bound to entities explicitly, one per line, so a dashboard is a
readable file you can diff and share:

```
group "Wohnzimmer"
    sensor sensor.wz_temperatur label "Temperatur"
    gauge  sensor.wz_co2 label "CO2" min 400 max 2000
    toggle light.wohnzimmer label "Licht"
end

group "Szenen"
    button scene.turn_on entity scene.gute_nacht label "Gute Nacht"
end
```

You should not have to type two hundred entity IDs, so generate a starting
point from your own instance and prune it:

```
ami2ha homeassistant.local TOKENFILE=S:ha.token WRITECONFIG=S:ami2ha.cfg
ami2ha CONFIG=S:ami2ha.cfg GUI
```

Widget kinds are `sensor`, `toggle`, `gauge`, `button` and `text`. See
[examples/dashboard.cfg](examples/dashboard.cfg) for a worked example.

The window updates from the WebSocket push, so readings change by
themselves without polling. While idle the application uses no CPU at all:
MUI hands its signal mask to `WaitSelect`, so one `Wait()` covers the GUI,
the socket and Ctrl-C together.

## Connecting

ami2ha talks to Home Assistant's
[WebSocket API](https://developers.home-assistant.io/docs/api/websocket) using
a long-lived access token, which you create under your Home Assistant profile.
The WebSocket API pushes state changes, so the Amiga is not polling.

By default the connection is plain HTTP and is intended for use on your own
LAN. **Your access token is sent in cleartext in that mode** — anyone able to
observe your local network can read it. Build with `USE_AMISSL=1` to enable
`https://` endpoints via [AmiSSL](https://github.com/jens-maus/amissl); note
that TLS on a plain 68k machine is slow.

## ARexx

ami2ha will expose an `AMI2HA` host port so other Amiga software can read
values and issue commands, and can run an ARexx script whenever a subscribed
entity changes. The command set is designed in
[docs/AREXX.md](docs/AREXX.md); it is not implemented yet.

## Roadmap

- [x] Cross-toolchain setup, reproducible from one script
- [x] Build system, host test harness
- [x] Portable core: buffers, JSON reader, Base64, SHA-1, WebSocket framing
- [x] `bsdsocket.library` transport, non-blocking, driven by `WaitSelect`
- [x] HTTP/1.1 upgrade and WebSocket handshake verification
- [x] Home Assistant client: authentication, `subscribe_events`, `get_states`, `call_service`
- [x] Entity store
- [x] Command line client (`LIST`, `GET`, `WATCH`, `TOGGLE`, `ON`, `OFF`)
- [x] Dashboard configuration format, parser and generator
- [x] MUI dashboard: sensors, gauges, toggles, buttons, live updates
- [ ] In-app dashboard editor
- [ ] Reconnect handling and connection status UI
- [ ] ARexx host port
- [ ] Optional AmiSSL support
- [ ] Installer, icons, documentation

## Contributing

Contributions are very welcome — see [CONTRIBUTING.md](CONTRIBUTING.md).
Especially useful right now: testing on real hardware and on
AmigaOS 4 / MorphOS / AROS, and anything that reduces memory use.

## License

MIT — see [LICENSE](LICENSE).

The bundled SDI headers in `include/SDI/` are public domain, from the
[adtools/SDI](https://github.com/adtools/SDI) project.
