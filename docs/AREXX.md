# ARexx interface

> **Design document.** This describes the intended interface. None of it is
> implemented yet — see the roadmap in the README.

ami2ha hosts an ARexx port named `AMI2HA`. If more than one copy is running,
subsequent instances take `AMI2HA.1`, `AMI2HA.2` and so on, following the
usual Amiga convention.

Two directions are supported:

- **Inbound** — other programs, or scripts you run by hand, send commands to
  `AMI2HA` to read values and control entities.
- **Outbound** — ami2ha runs an ARexx script when a subscribed entity
  changes, letting you push values into any other application that has a
  port of its own.

## Conventions

Results are returned in `RESULT`, so scripts must `OPTIONS RESULTS`. Commands
set `RC` to 0 on success and non-zero on failure, with a human-readable
description in `AMI2HA.LASTERROR`.

Where a command can return several values, it accepts a `STEM` argument and
writes a set of stem variables instead of a single result. Entity IDs are
always the full Home Assistant form, e.g. `light.kitchen`.

## Commands

### State access

```
GET <entity-id> [STEM <stem>]
```
Returns the current state as a string. With `STEM`, writes `<stem>STATE`,
`<stem>NAME`, `<stem>UNIT`, `<stem>CLASS` and `<stem>CHANGED` instead.

```
ATTR <entity-id> <attribute>
```
Returns one attribute value, e.g. `ATTR light.kitchen brightness`.

```
LIST [DOMAIN <domain>] [STEM <stem>]
```
Lists known entity IDs. Writes `<stem>COUNT` and `<stem>.1` … `<stem>.n`.

### Control

```
TOGGLE <entity-id>
ON     <entity-id>
OFF    <entity-id>
```
Convenience wrappers over `homeassistant.toggle` / `turn_on` / `turn_off`.

```
CALL <domain> <service> [ENTITY <entity-id>] [DATA <json>]
```
The general form. `DATA` takes a raw JSON object merged into the service
call, so anything the REST or WebSocket API accepts is reachable:

```rexx
CALL light turn_on ENTITY light.kitchen DATA '{"brightness":128}'
```

### Notifications

```
SUBSCRIBE <entity-id> <script> [ARG <text>]
UNSUBSCRIBE <entity-id> [<script>]
```
Runs `<script>` whenever the entity changes state. The script is invoked with
the entity ID, the new state and the old state as arguments, followed by
`ARG` text if given. This is how values reach other applications: the script
addresses their port and does whatever it likes.

```rexx
/* called as: NotifyTemp sensor.kitchen 21.4 21.3 */
PARSE ARG entity newstate oldstate
ADDRESS 'SOMEEDITOR' 'INSERT TEXT "Kitchen now' newstate 'degrees"'
```

### Session and control

```
CONNECT [<url>] [TOKEN <token>]   open a connection, optionally overriding prefs
DISCONNECT                        close it
STATUS [STEM <stem>]              connection state, entity count, server version
REFRESH                           re-fetch all states
VERSION                           ami2ha version string
QUIT [FORCE]                      exit the application
```

## Example

```rexx
/* Turn on the desk lamp if the study is below 20 degrees. */
OPTIONS RESULTS
ADDRESS AMI2HA

GET sensor.study_temperature
IF RC ~= 0 THEN DO
    SAY 'lookup failed:' AMI2HA.LASTERROR
    EXIT 10
END

IF RESULT < 20 THEN DO
    CALL light turn_on ENTITY light.desk_lamp DATA '{"brightness":200}'
    SAY 'lamp on, study at' RESULT 'degrees'
END
```

## Open questions

- Should `SUBSCRIBE` survive a restart by being stored in the preferences
  file, or stay session-only? Session-only is simpler and probably right,
  with persistent automations left to Home Assistant itself.
- Whether to expose a `WAIT <entity-id> [TIMEOUT <secs>]` command that blocks
  until a change arrives. Convenient for scripts, but it holds an ARexx
  message open, which needs care in the event loop.

Feedback on both is welcome.
