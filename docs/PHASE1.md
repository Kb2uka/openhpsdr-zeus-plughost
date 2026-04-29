# Phase 1 — IPC skeleton + SIGKILL gate

Phase 1 is the smallest end-to-end thing that demonstrates the sidecar
architecture is workable before we commit to it. It carries no plugin
code on purpose; its only job is to prove the data plane and process
lifecycle are sound.

## In scope

- SPSC lock-free shared-memory ring, one per direction
  (`zeus -> plughost`, `plughost -> zeus`).
- 64-byte cache-line block header + planar float32 payload
  (`src/audio/block_format.h`).
- Fixed format: 256 frames @ 48 kHz, mono, 8-block ring depth.
- Control-channel stub (`src/ipc/control_pipe.*`) — accepts open / send /
  recv calls but does no real I/O yet.
- Wakeup primitive working on Linux (futex); compiling stubs on
  Windows / macOS.
- Pass-through audio loop (`src/audio/passthrough.cpp`) — read input
  ring, memcpy into output ring, log a 1 Hz heartbeat.

## Out of scope (Phase 2)

- Plugin scanning, loading, instantiation (VST3 / CLAP / VST2).
- Real shm_open / CreateFileMapping plumbing — Phase 1 uses an anonymous
  heap allocation behind the same API so the wire layout is locked in.
- CBOR-framed control messages, plugin parameter automation, host events.
- Working Windows / macOS wakeup primitives.

## SIGKILL gate

The point of running plugins out-of-process is to keep a misbehaving
third-party DLL from taking Zeus down. To validate that, Phase 1 has a
single load-bearing test:

1. Start the sidecar with valid `--shm-name` and `--control-pipe`
   arguments. Confirm the heartbeat log fires once per second.
2. From another terminal, send `SIGKILL` (`kill -9 <pid>`).
3. The sidecar must exit immediately with no zombie state in
   `/dev/shm/<name>.in` or `/dev/shm/<name>.out` left behind.
   (Phase 1's heap-backed mapping makes this trivially true; Phase 2 must
   re-prove it once shm_open is real.)
4. Restart the sidecar with the same arguments. It must come up cleanly
   without manual cleanup.

If any of those steps require human intervention to recover, the design
has failed and we revisit before Phase 2.

## Pass-through smoke test

Once a Zeus-side producer exists (Phase 1.5):

1. Producer writes blocks of known content (e.g. a 1 kHz sine, seq
   counter monotonic) into the input ring.
2. Sidecar pass-through copies them to the output ring.
3. Producer reads back from the output ring and asserts:
   - `seq` is preserved and monotonic.
   - Sample data round-trips bit-exactly.
   - Heartbeat counter `processed` matches the producer's send count
     within one block.

Until that producer lands, you can sanity-check the sidecar by running
it under `strace -e trace=futex` and confirming no other syscalls hit
during steady-state operation (i.e. the realtime contract holds).
