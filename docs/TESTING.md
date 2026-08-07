# Testing ami2ha under FS-UAE

Notes for getting a fresh emulated machine to the point where ami2ha runs.

## What the Amiga actually needs

| | Needed for | Note |
|---|---|---|
| `bsdsocket.library` | everything | Provided by the emulator; no TCP/IP stack install required — see below |
| MUI 3.8+ | `GUI` mode only | The command line modes (`LIST`, `GET`, `WATCH`) do not open MUI at all |
| Kickstart 2.04+ | everything | 3.x recommended |

The library bases are opened lazily, so a machine with no MUI installed can
still run the command line client. That makes it a useful first test: if
`LIST` works, the whole network and protocol stack is proven, and anything
that fails afterwards is a UI problem.

## bsdsocket.library without a TCP/IP stack

FS-UAE and WinUAE can emulate `bsdsocket.library` directly, mapping Amiga
socket calls onto the host's network stack. That avoids installing Roadshow
or AmiTCP entirely, and it is much faster than a real stack under emulation.

In your FS-UAE configuration:

```ini
bsdsocket_library = 1
```

With this enabled there is no `SANA-II` driver, no `AmiTCP:` assign and no
`startnet` script — `bsdsocket.library` simply exists. Note the tradeoff:
the emulated machine has no IP address of its own and shares the host's,
which is fine for a client like ami2ha.

## Getting files in

The simplest route is a host directory mounted as a hard drive:

```ini
hard_drive_0 = /Users/you/amiga/dh1
```

Anything you drop in that directory appears on the Amiga, so `make` on the
host and the new binary is immediately available in the emulator.

## Building for the target

```sh
make CPU=68000     # runs on everything
make CPU=68020     # smaller and faster; needs an 020 or better
```

## First run

Put your long-lived access token in a file rather than passing it on the
command line, where it would end up in the shell history and be visible in
the task list:

```
1> ami2ha homeassistant.local TOKENFILE=S:ha.token LIST
```

If that prints your entities, everything below the UI works. Then generate
a dashboard and open it:

```
1> ami2ha homeassistant.local TOKENFILE=S:ha.token WRITECONFIG=S:ami2ha.cfg
1> ami2ha CONFIG=S:ami2ha.cfg GUI
```

## Troubleshooting

**"no TCP/IP stack running"** — `bsdsocket_library = 1` is missing from the
FS-UAE configuration, or a real stack is installed but not started.

**"host not found"** — the emulated machine resolves names through the host,
so `homeassistant.local` only works if the host can resolve it. An mDNS
`.local` name may not; try the IP address.

**"server refused the connection (HTTP 401)"** — the token was rejected.
Check for a trailing newline or truncation in the token file; ami2ha stops
the token at the first whitespace, so a wrapped file will not work.

**"cannot open muimaster.library"** — MUI is not installed. The command line
modes still work.

**Window opens but stays empty** — the configuration parsed but contains no
widgets, or none of the configured entities exist. Cross-check the entity
IDs against `LIST`.

## Driving it from the host with amimcp

[amimcp](https://github.com/thomas-luebker/amimcp) puts an agent on the
Amiga and exposes it over MCP, which makes the loop of "build, push, run,
screenshot" tractable without leaving the host. Its agent needs the same
`bsdsocket.library`, so the setup above covers it.

Because UAE's bsdsocket emulation maps Amiga sockets onto host sockets, an
agent listening on the Amiga should be reachable from the host on the same
port — worth checking rather than assuming, but it means no port forwarding
should be needed.
