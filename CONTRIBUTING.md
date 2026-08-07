# Contributing to ami2ha

Contributions are welcome — code, testing on real hardware, or just telling
us what breaks.

## Getting set up

```sh
./tools/setup-toolchain.sh
make && make test
```

If the setup script fails on your machine, that is a bug worth reporting;
it is meant to work from a clean install.

## Where code goes

```
src/core/     pure C99, no Amiga headers, no OS calls -- must have tests
              (this is most of the project: protocol, parsing, entity store)
src/net/      bsdsocket.library transport, TLS
src/ui/       MUI interface
src/rexx/     ARexx host port
src/config/   preferences
src/main.c    command line front end
```

Before writing something, ask whether it genuinely needs the operating
system. Parsing, protocol state machines, encoding and buffer handling do
not, and they belong in `src/core/` where they can be tested in a second
instead of through an emulator. This is the single most useful convention in
the project; please keep it intact.

## Style

- C99. No compiler-specific extensions in `src/core/`.
- Four spaces, no tabs. Braces on the same line for control flow, on their
  own line for functions.
- Amiga-side files include `ami2ha/compat.h` first.
- Use the SDI macros for hooks and register conventions rather than writing
  per-compiler variants by hand.
- Comments explain *why*. What the code does should be evident from reading
  it; what it is working around usually is not.

## Memory

The target is a machine with a couple of megabytes free. Assume every
allocation matters:

- Prefer fixed-size buffers and caller-provided storage over `malloc` in hot
  paths.
- Do not build intermediate copies of large payloads. The JSON reader is
  zero-allocation by design; keep it that way.
- Free on every path, including error paths. There is no process teardown to
  clean up after you — a leak persists until reboot.

## Tests

Anything in `src/core/` needs tests in `tests/`. The harness is deliberately
tiny (`tests/tinytest.h`); add a `suite_yourthing()` and call it from
`tests/main.c`.

Where a specification supplies test vectors — RFC 4648 for Base64, RFC 6455
for the WebSocket handshake, FIPS 180-1 for SHA-1 — use them rather than
values you generated with the implementation under test.

## Portability

The primary target is AmigaOS 3.x on 68k, but the sources are meant to build
for AmigaOS 4, MorphOS and AROS. Avoid OS3-only idioms where a portable one
exists, and keep platform differences inside `compat.h` rather than scattered
through `#ifdef`s.

Reports of build failures or misbehaviour on OS4, MorphOS and AROS are
particularly valuable, since day-to-day development happens against OS3.

## Pull requests

- One logical change per PR.
- `make test` must pass, and the m68k build must be clean.
- Say what you tested on. "Builds and tests pass, not tried on hardware" is
  a perfectly good and useful note.
