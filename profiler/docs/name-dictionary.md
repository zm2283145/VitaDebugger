# Profiler name dictionary

VitaProfiler event records keep a 32-bit `name_id` instead of copying text into
the recording ring. The name dictionary added here turns those IDs back into
useful zone, frame, counter, memory, process, and thread names in a captured
trace without changing the version-1 event ABI.

## Lifecycle

The application owns a fixed entry array and text arena. Registration happens
during initialization, before profiler producer threads start:

```c
static struct vp_name_dictionary names;
static struct vp_name_entry name_entries[128];
static char name_text[4096];

static uint32_t frame_id;
static uint32_t update_id;

static int init_names(void)
{
    struct vp_name_dictionary_config config = {
        .entries = name_entries,
        .entry_capacity = 128,
        .text = name_text,
        .text_capacity = sizeof(name_text),
    };

    if (vp_name_dictionary_init(&names, &config) != VP_RESULT_OK ||
        vp_name_dictionary_register(&names, "main frame", &frame_id) !=
            VP_RESULT_OK ||
        vp_name_dictionary_register(&names, "update", &update_id) !=
            VP_RESULT_OK)
        return -1;

    return vp_name_dictionary_seal(&names);
}
```

`vp_name_dictionary_register()` copies each name, returns the same ID as
`vp_name_id()`, and is idempotent when the same name is registered twice. If
two different strings have the same 32-bit FNV-1a value, the second
registration returns `VP_ERROR_NAME_CONFLICT`; it never silently relabels an
event. Entry or text exhaustion returns `VP_ERROR_CAPACITY` without partially
modifying the dictionary.

Initialization, registration, sealing, and deinitialization are quiescent
operations. Once sealed, the dictionary is immutable, so any number of threads
may call the lookup, snapshot, size, and encoding functions concurrently. The
application must join those readers before deinitializing or reusing the
caller-owned storage.

## Capturing names with events

The dictionary is a separate block. This intentionally preserves the existing
32-byte `vp_event` records and version-1 `VPRF` header. An exporter can either:

- save the `VPNM` block as a sidecar next to the `VPRF` event stream; or
- write `VPNM` first and `VPRF` immediately afterward. The name header's
  `total_size` gives the offset of the following event header.

Encoding is sized and all-or-nothing:

```c
size_t required;
size_t written;

if (vp_name_dictionary_wire_size(&names, &required) == VP_RESULT_OK &&
    required <= sizeof(name_buffer) &&
    vp_encode_name_dictionary_le(&names, name_buffer,
                                  sizeof(name_buffer), &written) ==
        VP_RESULT_OK) {
    write_capture(name_buffer, written);
}
```

If the output is too short, the encoder returns
`VP_ERROR_BUFFER_TOO_SMALL`, puts the required size in `written`, and leaves
the output buffer untouched.

## Receiver API

`vp_name_wire_cursor_init()` fully validates a block before exposing any
entries. Validation checks the magic and version, all declared bounds, strict
ID ordering, duplicate IDs, built-in identities, FNV IDs for application
names, and zero padding. It accepts trailing bytes so a combined `VPNM` +
`VPRF` capture can be parsed directly.

```c
struct vp_name_wire_info info;
struct vp_name_wire_cursor cursor;
struct vp_name_view name;

if (vp_name_wire_cursor_init(&cursor, capture, capture_size, &info) ==
    VP_RESULT_OK) {
    while (vp_name_wire_cursor_next(&cursor, &name) == VP_RESULT_OK) {
        /* name.name points into capture and is not NUL-terminated. */
        consume_name(name.name_id, name.name, name.name_length);
    }

    /* The VPRF event header begins at capture + info.total_size. */
}
```

For a simple receiver, `vp_name_wire_lookup_le()` resolves one numeric ID.
For a live in-process display, `vp_name_dictionary_lookup()` resolves against
the sealed caller-owned dictionary. `vp_name_dictionary_entry_at()` exposes a
stable sorted snapshot suitable for another serializer.

The input block must remain immutable and alive while a cursor or returned
wire view is in use.

## Wire format version 1

All integers are little endian.

### Header (24 bytes)

| Offset | Size | Meaning |
| ---: | ---: | --- |
| 0 | 4 | `VPNM` magic (`0x4d4e5056`) |
| 4 | 2 | format version (`1`) |
| 6 | 2 | header size (`24`) |
| 8 | 2 | entry header size (`8`) |
| 10 | 2 | flags (`1` means little endian) |
| 12 | 4 | total entry count |
| 16 | 4 | total dictionary-block size |
| 20 | 4 | reserved, must be zero |

### Entry

| Offset | Size | Meaning |
| ---: | ---: | --- |
| 0 | 4 | numeric `name_id` |
| 4 | 2 | UTF-8 name byte length, excluding a terminator |
| 6 | 2 | flags (`VP_NAME_FLAG_BUILTIN` or zero) |
| 8 | variable | name bytes, followed by zero padding to four bytes |

Entries are strictly sorted by ID. Names contain no embedded NUL and are at
most 255 bytes. The block contains at most 4096 caller names plus the eight
built-in names.

## Built-in Vita names

The following records are included automatically and consume no caller entry
or text storage:

| ID | Name |
| --- | --- |
| `0xfff00001` | `vita.memory.free_user_bytes` |
| `0xfff00002` | `vita.memory.free_cdram_bytes` |
| `0xfff00003` | `vita.memory.free_phycont_bytes` |
| `0xfff00004` | `vita.process.time_us` |
| `0xfff00005` | `vita.thread.run_clocks` |
| `0xfff00006` | `vita.thread.stack_free_bytes` |
| `0xfff00007` | `vita.thread.preemptions` |
| `0xfff00008` | `vita.thread.interrupt_preemptions` |

## Current limits

- Dynamic registration after sealing is intentionally unsupported. Games that
  discover names at runtime must register a bounded superset up front; a
  versioned dictionary-update/epoch record can be added later if required.
- Names omitted by the application remain numeric in a trace. The library
  cannot infer a source-level label from an ID alone.
- The stable ID is 32-bit FNV-1a, not a cryptographic identifier. Registration
  and wire validation detect collisions present in one dictionary.
- Dictionary lookup is allocation-free, but full wire validation is intended
  for capture setup or receiver ingestion, not for every profiler event.
- The updated Vita probe has been cross-built. Its live hardware gate remains
  pending until the new VPK is run on a Vita.
