# Ideas not yet built

## Choosing entities from inside Home Assistant

Today the dashboard is a text file on the Amiga. It would be nicer to pick
the entities in Home Assistant itself and have the Amiga follow that, so a
change does not mean editing a file on a machine that may be in another
room.

Three ways to do it, cheapest first.

### Labels (no custom component) -- IMPLEMENTED

Built. Set `label <name>` in the dashboard file; see the README.

Measured on a real installation before choosing this route:

    config/entity_registry/list                 2,433,832 bytes (3494 entities)
    render_template label_entities('amiga')           164 bytes

The registry route is not merely wasteful, it is impossible: it exceeds the
2 MB WebSocket message cap on its own. What follows is the original note.



Home Assistant has labels. Tag entities with `amiga`, and ami2ha asks the
entity registry which entities carry that label:

```
{"id":N,"type":"config/entity_registry/list"}
```

then filters on `labels` and feeds the result straight into
`subscribe_entities`. Nothing to install on the HA side, and the selection
UI already exists -- it is the normal label picker.

The registry list is large on a big installation (it describes every
entity), so it would want fetching once at startup and parsing streamed
rather than buffered whole. Widget *kind* would still have to be inferred
from the domain, as the generator already does, or overridden locally.

### A group or input_text helper

Put the entity ids in a `group`, or a comma-separated `input_text`. Both are
tiny to read and need no custom code. Cruder than labels, but the payload is
tiny and the parsing trivial.

### A custom integration with a config flow

A proper `ami2ha` integration with a config flow: pick entities, choose
widget kinds and gauge ranges, and expose the lot as one JSON document the
Amiga fetches. The nicest to use and by far the most work -- and it puts a
Python component between the Amiga and a protocol it can already speak.

### Worth keeping either way

The local file should not go away. It works with no server-side setup, it
can be shared, and it is the only option if someone is pointing an Amiga at
a Home Assistant they do not administer. Server-side selection is better as
an additional source, not a replacement.
