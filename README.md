# uvdb

This is an intra-process GDB stub for the PSVita that should (for the most part) "just work".

The reason I created this is because [kvdb](https://github.com/DaveeFTW/kvdb) only supports firmware 3.60 (I'm using 3.65), lists hilarious bugs on its README, and is apparently not that easy to setup. On contrary, uvdb contains no kernel-mode code and uses a well-established kernel plugin (kubridge) for the heavy lifting, and thus should not cause as many stability issues.

## Installation

Add [kubridge](https://github.com/bythos14/kubridge/releases) to your PSVita's ur0:tai/config.txt.

Replace `$VITASDK/arm-vita-eabi/kubridge.h` with the one from the above-linked repository.

Type `make` to build.

(Optionally, but this will make your life easier) Copy `uvdb.h` to `$VITASDK/arm-vita-eabi/include`, and `libuvdb.a` to `$VITASDK/arm-vita-eabi/lib`.

## Usage

Link your project with `libuvdb.a` (`-luvdb` linker flag), then call `uvdb_enter()` wherever in your code you need a software breakpoint.

Then run `arm-vita-eabi-gdb program.elf -ex 'target remote PS.VITA.IP.ADDR:1234'` to connect GDB to the console and start debugging.

Working features:

* Memory reading/writing
* Breakpoints
* Single-stepping
* ASLR defeat (base address of the program is resolved)

Non-working features:

* Watchpoints
* `catch syscall`
* Resolving base addresses of libraries (most Vita homebrew is single-binary anyway)
* Probably anything else...

## Bugs and caveats

Unlike x86, ARM does not support proper single-stepping and hardware breakpoints. To overcome this, GDB parses the instruction itself and sets temporary (software) breakpoints at all possible branch targets. This is fine, albeit slow (unfortunately, GDB's serial debugging protocol was never designed to run over TCP), in single-threaded programs, but in multi-threaded programs you may miss some breakpoints.

uvdb uses kubridge's exception handling feature to catch exceptions. If your homebrew installs its own exception handler, you will have problems. If you want to do so, register them *after* `uvdb_enter()` has been called at least once, and save and call the original handler once you determine that you can't handle the exception.

Also (obviously?) uvdb does not work in kernel mode, thus you can't use it to debug kernel plugins.

Anything else? Feel free to [file a bug report](https://github.com/sleirsgoevy/vita-uvdb/issues/new).

## Render96 hardening branch

This branch preserves the original `uvdb_enter()` entry point and adds:

* Explicit configuration, state inspection, and shutdown APIs.
* A bounded packet buffer (256 KiB by default).
* Socket, message-pipe, allocation, and exception-handler cleanup.
* Disconnect detection and support for reconnecting at a later debugger entry.
* GDB `Z0`/`z0` software breakpoints for ARM and Thumb code.
* GDB `s`/`S` single-step packets using temporary software breakpoints.
* Branch-aware stepping for common ARM and 16-bit Thumb control flow.
* Structured details for the most recently intercepted exception.
* Fixes for partial stdio writes, wildcard register writes, qXfer ranges, and
  failed safe-memory transfers.

Configure the library before its first breakpoint if non-default settings are
needed:

```c
struct uvdb_config config = {
    .port = 1234,
    .max_packet_buffer = 256 * 1024,
};
uvdb_configure(&config);
uvdb_enter();
```

The diagnostic ELF on the PC must match the executable running on the Vita.
Compile application and library objects with `-g`; the packaged Vita executable
may still be stripped by the normal VPK packaging tools.

### Current limitations

* The debugger is process-local and requires an explicit `uvdb_enter()` call.
* Stepping is not yet thread-aware. Another application thread can encounter a
  temporary breakpoint first.
* Thumb-2 32-bit branches and uncommon instructions that write to `pc` still
  need dedicated target decoding.
* Hardware breakpoints and watchpoints are not implemented.
* Resuming a real memory fault without correcting its cause generally faults
  again.
