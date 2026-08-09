# ARexx interface

> Implemented and tested on AmigaOS 3.2. `SUBSCRIBE`/`UNSUBSCRIBE` are the
> exception: they are still a design sketch, and are marked as such below.

ami2ha hosts an ARexx port named `AMI2HA`. If more than one copy is running,
subsequent instances take `AMI2HA.1`, `AMI2HA.2` and so on, following the
usual Amiga convention.

Two directions are supported:

- **Inbound** — other programs, or scripts you run by hand, send commands to
  `AMI2HA` to read values and control entities.
- **Outbound** — ami2ha runs an ARexx script when a subscribed entity
  changes, letting you push values into any other application that has a
  port of its own. *(designed, not implemented — see Notifications below)*

## Conventions

Results are returned in `RESULT`, so scripts must `OPTIONS RESULTS`. Commands
set `RC` to 0 on success, 5 when the request was understood but could not be
answered (an unknown entity, say), and 10 for a malformed command.

After a failure, `LASTERROR` returns the reason:

```rexx
GET sensor.nope
IF RC ~= 0 THEN DO
    LASTERROR
    SAY 'failed:' RESULT        /* -> no such entity: sensor.nope */
END
```

**Arguments do not need quoting.** ARexx uppercases unquoted words in a
command clause, so `GET switch.kitchen` arrives as `GET SWITCH.KITCHEN`.
Since Home Assistant ids, domains, services and attribute names are always
lower case, ami2ha folds them back. The one thing you *must* quote is the
JSON passed to `DATA`, whose keys are case-sensitive:

```rexx
CALL light turn_on ENTITY light.kitchen DATA '{"brightness":200}'
```

Remember too that every ARexx script must begin with a `/* comment */`, or
the interpreter will not recognise it as one.

Commands that return several values put one per line in `RESULT`; `STEM` is
not implemented. Entity IDs are always the full Home Assistant form, e.g.
`light.kitchen`.

## Commands

### State access

```
GET <entity-id>
```
Returns the current state as a string.

```
ATTR <entity-id> <attribute>
```
Returns one attribute value, e.g. `ATTR light.kitchen brightness`.

```
LIST [DOMAIN <domain>]
```
Lists known entity IDs, one per line.

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

### Notifications (not implemented yet)

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
STATUS      connection state, server version and entity count, space separated
COUNT       number of entities held
LASTERROR   reason for the most recent failure
VERSION     ami2ha version string
QUIT        exit the application
```

`CONNECT`, `DISCONNECT` and `REFRESH` are not implemented.

## The port name

The port is `AMI2HA`. A second instance takes `AMI2HA.1`, a third `AMI2HA.2`
and so on, so `SHOW('P')` is worth checking if you run more than one:

```
ports: WORKBENCH DEFICONS REXX AREXX ... AMI2HA AMI2HA.1 ...
```

## Example

This is a real session, run on AmigaOS 3.2 against a live Home Assistant:

```rexx
/* ami2ha: full control round trip from ARexx */
OPTIONS RESULTS
ADDRESS AMI2HA

GET switch.workshop_outlet
SAY 'before  -> ' || RESULT          /* before  -> off */

ON switch.workshop_outlet
SAY 'ON  rc=' || RC                  /* ON  rc=0       */

GET switch.workshop_outlet
SAY 'during  -> ' || RESULT          /* during  -> on  */

OFF switch.workshop_outlet
GET switch.workshop_outlet
SAY 'after   -> ' || RESULT          /* after   -> off */
```

The state read back after switching comes from the WebSocket push, not from
a re-query: ami2ha updates its store as Home Assistant reports the change.

## Open questions

- Should `SUBSCRIBE` survive a restart by being stored in the preferences
  file, or stay session-only? Session-only is simpler and probably right,
  with persistent automations left to Home Assistant itself.
- Whether to expose a `WAIT <entity-id> [TIMEOUT <secs>]` command that blocks
  until a change arrives. Convenient for scripts, but it holds an ARexx
  message open, which needs care in the event loop.
