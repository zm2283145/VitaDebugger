# Vita TCP stream sink

`vitaprofiler_tcp_vita.h` is an opt-in Vita-side TCP adapter for the existing
`vp_stream_writer`. It connects one caller-owned profiler sink to the PC
receiver and makes the writer's complete-buffer callback contract explicit:
zero means every byte reached the socket; any other result permanently fails
that capture.

This is telemetry transport, not a control protocol. It is **not authenticated
or encrypted**. Use it only on a trusted development LAN, restrict access at
the host firewall, and run the PC receiver with its `--source` filter. That
filter reduces accidental/untrusted peers but is not cryptographic identity.

## Ownership and prerequisites

The adapter owns only the socket and epoll handle it creates. The application
must load `SCE_SYSMODULE_NET`, allocate the SceNet pool, call `sceNetInit()`, and
keep that global network lifetime valid until `vp_vita_tcp_sink_close()` has
finished. The adapter never loads/unloads a module and never calls
`sceNetInit()` or `sceNetTerm()`. Initialize backend storage with
`VP_VITA_TCP_SCE_NET_BACKEND_INITIALIZER` (or explicit zero initialization)
before its first `vp_vita_tcp_sce_net_ops_init()` call. Do not overwrite it;
reinitialization returns `VP_ERROR_BUSY` until all retained socket/epoll cleanup
has succeeded.

All adapter state, stream-writer state, and dictionary wire storage are supplied
by the caller. Zero-initialize the sink before its first initialization. Run
connect, drain, and close on one dedicated consumer thread. In particular, do
not run them on a render/game producer thread: each writer callback has its own
send deadline and may wait for socket writability up to that bound.

Start the receiver before connecting:

```sh
python tools/vitaprofiler_trace.py receive game.vptrace \
  --bind 0.0.0.0 --port 18195 --source 192.168.1.42
```

After the application has initialized SceNet:

```c
#include <vitaprofiler_tcp_vita.h>

static struct vp_vita_tcp_sce_net_backend tcp_backend =
    VP_VITA_TCP_SCE_NET_BACKEND_INITIALIZER;
static struct vp_vita_tcp_sink tcp_sink; /* static storage starts zeroed */
static struct vp_stream_writer stream_writer;
static uint8_t dictionary_wire[8192];

int profiler_stream_start(uint64_t capture_start_us)
{
    struct vp_vita_tcp_sink_config tcp;
    struct vp_stream_writer_config stream = {0};

    vp_vita_tcp_sink_config_init(&tcp);
    tcp.endpoint.ipv4[0] = 192;
    tcp.endpoint.ipv4[1] = 168;
    tcp.endpoint.ipv4[2] = 1;
    tcp.endpoint.ipv4[3] = 10; /* development computer */
    tcp.endpoint.port = 18195;
    if (vp_vita_tcp_sce_net_ops_init(&tcp_backend, &tcp.ops) !=
            VP_RESULT_OK)
        return VP_ERROR_PLATFORM;
    tcp.ops_user = &tcp_backend;

    if (vp_vita_tcp_sink_init(&tcp_sink, &tcp) != VP_RESULT_OK)
        return VP_ERROR_IO;
    if (vp_vita_tcp_sink_connect(&tcp_sink) != VP_RESULT_OK) {
        /* connect already tried to clean up. This call retries any retained
         * descriptor; if it still fails, keep this storage and SceNet alive
         * and retry close again later. */
        if (vp_vita_tcp_sink_close(&tcp_sink) != VP_RESULT_OK)
            return VP_ERROR_BUSY;
        return VP_ERROR_IO;
    }

    stream.context = &profiler;
    stream.names = &profiler_names;
    stream.write = vp_vita_tcp_sink_write;
    stream.write_user = &tcp_sink;
    stream.dictionary_buffer = dictionary_wire;
    stream.dictionary_buffer_capacity = sizeof(dictionary_wire);
    if (vp_stream_writer_init(&stream_writer, &stream) != VP_RESULT_OK ||
        vp_stream_writer_begin(&stream_writer, capture_start_us) !=
            VP_RESULT_OK) {
        (void)vp_vita_tcp_sink_close(&tcp_sink);
        return VP_ERROR_IO;
    }
    return VP_RESULT_OK;
}
```

The example assumes the profiler context and sealed name dictionary already
exist. It intentionally accepts only a numeric IPv4 endpoint: DNS lookup and
its extra blocking/failure policy stay outside the capture path.

Drain bounded batches on that same consumer thread. At shutdown, stop
producers, drain the ring completely, close the stream writer, and then close
the TCP sink:

```c
size_t drained;

while (vp_stream_writer_drain(&stream_writer, 64, &drained) == VP_RESULT_OK &&
       drained != 0) {
    /* Continue until empty after producers have stopped. */
}
if (vp_stream_writer_close(&stream_writer) == VP_RESULT_OK)
    (void)vp_vita_tcp_sink_close(&tcp_sink); /* clean EOF frames VPRF v1 */
else
    (void)vp_vita_tcp_sink_close(&tcp_sink); /* incomplete capture */
```

Only call `sceNetTerm()` after the sink is known closed. If close returns an
error, call it again: a failed close retains the descriptor as an explicit
cleanup obligation. A prior send/connect failure remains visible as `FAILED`
in statistics even after a cleanup retry succeeds.

## Bounds and failure behavior

The default configuration uses:

- a 5-second absolute connect deadline;
- a separate 2-second absolute deadline for each writer callback;
- 100-ms maximum writable-wait slices;
- 64 connect waits, 64 send waits, and 1,024 send calls per callback; and
- 16-KiB maximum individual sends.

Every limit is configurable and must remain nonzero. The call/wait caps prevent
an infinite loop even if an injected clock stalls. The adapter resamples the
monotonic clock after every socket operation and rejects a success returned at
or after the deadline. Low-level callbacks other than `wait_writable` must be
nonblocking; the adapter cannot preempt a broken callback that ignores that
contract. Public wait slices are milliseconds; the SceNet backend converts them
to its microsecond timeout and saturates oversized values at the signed API
limit instead of allowing integer wraparound.

Partial sends are normal and are retried until the complete writer buffer is
delivered. A zero-byte "success", byte over-report, clock regression, timeout,
call-limit exhaustion, or socket error fails the sink, shuts down/closes the
connection, and makes later writes fail. It never reconnects in the middle of a
capture because the peer may already have received an ambiguous prefix.

`vp_vita_tcp_sink_get_stats()` reports bytes actually accepted by `send`, write
and socket-call counts, partial sends, waits/would-blocks, cleanup errors,
terminal state/failure reason, and the last native SceNet error. Compare those
counters with `vp_stream_writer_get_stats()` and `vp_get_stats()` to distinguish
transport loss, a removed-but-undelivered event, and producer ring pressure.

## Test and hardware status

The state machine is host-tested through injected socket operations, including
immediate/asynchronous connect, timeout slices, partial sends, zero progress,
over-reporting, late success, stagnant/regressing clocks, call/wait caps,
native errors, shutdown failure, retryable close, and a full
`vp_stream_writer` integration capture. A fake-SceNet build also compiles the
production backend path and covers its timeout units, partial-open cleanup,
reinit exclusion while resources are live, staged epoll/socket close failures,
cleanup retry, and reinit after complete release.

The SceNet backend is wired into the Vita archive and a link-check target. It
subsequently passed the retail-3.65 stream, disconnect, and recovery gates, then
carried two complete 300-frame Render96EX captures with zero transport loss.
See the [transport hardware record](../../docs/hardware/profiler-tcp-stream-retail-3.65.md)
and [Render96EX capture record](../../docs/hardware/profiler-render96ex-head-baseline-2026-09-15.md).
Keep it opt-in because it remains an unauthenticated single-capture transport.
