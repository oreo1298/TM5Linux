# How TM5 for Linux tests memory

This page explains what happens during a test and how every setting of a TM5 config file is
interpreted. TestMem5 itself is closed source, so TM5 for Linux is an **independent
reimplementation**: it uses the same config files, test functions, sequence, cycles and parameters,
but the exact data patterns and timings are its own.

## The big picture

1. **Reserve memory.** By default TM5 for Linux takes all *available* memory (`MemAvailable`) minus
   a safety reserve of 384 MiB + 2 % of your RAM (or the config's
   `Reserved Memory for Windows (Mb)`, whichever is larger). You can choose another amount.
2. **Lock it.** The memory is locked with `mlock()` so Linux cannot move it to swap or zram while
   it is being tested. This needs root or the `CAP_IPC_LOCK` capability (the installers grant it).
   Transparent huge pages are requested, so the memory is mapped in 2 MiB pages where possible.
3. **Split it.** Every CPU thread (`Cores=0` = all threads) gets its own slice and is pinned to one
   CPU. Threads never touch each other's memory, so every mismatch is a real memory error.
4. **Run the sequence.** All threads run the same test at the same time. When every thread has
   finished, the next test of `Test Sequence` starts. After the last one, the next cycle begins.
5. **Compare every bit.** Each test writes data that can be recomputed later, reads it back and
   compares all 64 bits of every word. A difference is reported with its address, the expected and
   actual value, and which bits flipped.

### Bypassing the CPU caches

Writes use **non-temporal (streaming) stores** (`MOVNTDQ` / `MOVNTI` on x86-64). They go straight
to the memory controller and evict the line from all caches, so the following reads must come from
DRAM. Without this a 4 MiB test block would fit in the L3 cache and the RAM would hardly be tested.
On non-x86 CPUs ordinary stores are used.

### "Read error" or "wrong value stored in RAM"?

When an error is found, the word is flushed from the cache and read again twice straight from DRAM:

| What the re-read shows     | Reported as                       | Typical causes                                          |
|----------------------------|-----------------------------------|---------------------------------------------------------|
| the correct value          | **read error**                    | timings too tight, signal integrity, too little voltage, frequency too high |
| the same wrong value again | **wrong value stored in RAM**     | refresh timings (tREFI / tRFC), weak cells, heat, a bad module |

The physical address is shown when the engine runs as root (Linux hides it from normal users).

## Duration ("Time (%)")

```
passes = round( config Time (%) / 100  ×  test Time (%) / 100  ×  duration scale / 100  ×  4 )
```

with a minimum of one pass. A *pass* means the whole slice of every thread is processed once by that
test. For Extreme1 (`Time (%)=500`, tests at 100 %) every SimpleTest step does 20 passes and every
MirrorMove128 step 20 passes. The **Test duration** setting in the app (or `--time-scale`) is the
duration scale; *Normal* (100 %) keeps the config's own timing.

The total time grows with the amount of memory and shrinks with memory bandwidth. As a rough guide,
Extreme1 with 3 cycles takes about 30–90 minutes for 16 GB, depending on memory speed. The app
measures the real speed during the first tests and shows the estimated time remaining.

## Test functions

### SimpleTest

Write a pattern, then read everything back and compare.

* `Test Block Size (Mb)` > 0: memory is handled one block at a time - each block is written and
  checked immediately. `0` means the whole slice is written first and only then checked, so the data
  has to survive for several seconds.
* `Parameter` is the **stride** in 64-byte lines (TM5's `Operation Block, byts=64`). `0` or `1`
  walks memory in order (maximum bandwidth). Larger values visit lines that far apart
  (e.g. `256` → 16 KiB jumps), forcing the RAM to open a new row for almost every access, which
  stresses tRCD, tRP, tRAS and tRC. Every line is still covered exactly once per pass.
* Odd passes write the bit-inverted data of the previous pass and walk memory backwards, so every
  bit is tested as both 0 and 1 in both directions.

### MirrorMove and MirrorMove128

Copy tests that mix reads and writes all the time:

1. Fill the first half of the block with data.
2. Copy it to the second half in *mirrored* order: the half is cut into chunks of `Parameter` lines
   (`Parameter` × 64 bytes) and the chunk order is reversed; inside each line the 64-bit words
   (MirrorMove) or 128-bit lanes (MirrorMove128) are reversed and every bit is inverted.
3. Verify the copy against freshly computed data (bad words are repaired, so one fault is counted
   once).
4. Copy it back the same way and verify the first half again.

MirrorMove uses 64-bit streaming moves (`MOVNTI`), MirrorMove128 128-bit SSE moves (`MOVNTDQ`).
Pattern mode 0 uses random data here so that every copied line is unique.

### RefreshStable

Fill the slice, leave it completely untouched for 4 seconds so the DRAM has to keep its contents with
refresh alone, then verify. Finds refresh problems (tREFI too long, tRFC too short) and weak cells.
(Extreme1 defines it as Test 0 but does not use it in its sequence.)

## Pattern modes

| `Pattern Mode` | Data                                                                                       |
|----------------|--------------------------------------------------------------------------------------------|
| 0              | Rotates through classic patterns: own address, all 0 / all 1, checkerboard, alternating lines, walking ones, byte/word stripes, random … - a different one per pass, test and cycle |
| 1              | Random data. `Pattern Param0` / `Pattern Param1` are mixed into the seed                     |
| 2              | Arithmetic sequence: start value `Param1`, step `Param0` (both mirrored into the upper 32 bits so that all 64 data lines toggle) |

All patterns depend on the position in memory, so neighbouring threads, blocks and passes never see
identical data. Random data uses a new seed each run unless `--seed` is given.

## Supported config keys

| Section                 | Key                                    | Used for                                    |
|-------------------------|----------------------------------------|---------------------------------------------|
| `[Main Section]`        | `Config Name`, `Config Author`         | display                                     |
|                         | `Cores`                                | threads (0 = all)                           |
|                         | `Tests`                                | number of `[TestN]` sections                |
|                         | `Time (%)`                             | duration, see above                         |
|                         | `Cycles`                               | number of cycles (0 = until stopped)        |
|                         | `Test Sequence`                        | order of tests in one cycle                 |
| `[Global Memory Setup]` | `Reserved Memory for Windows (Mb)`     | minimum memory left free                    |
| `[TestN]`               | `Enable`, `Time (%)`, `Function`       | test selection and duration                 |
|                         | `Pattern Mode`, `Pattern Param0/1`     | data pattern                                |
|                         | `Parameter`                            | stride / mirror chunk size                  |
|                         | `Test Block Size (Mb)`                 | block size (0 = whole slice)                |

Windows-specific keys (`Testing Window Size`, `Lock Memory Granularity`, `Channels`,
`Interleave Type`, `Single DIMM width`, `Use DMA`, `DLL Name`, `Window Position`, `Debug Level`,
`Capable`, `Language`) are read but have no effect: Linux does not need TM5's AWE memory windows.
Comments after `;` and CRLF / LF / CR line endings are handled. Unknown test functions run as
SimpleTest and a warning is shown.

## Bundled profiles

| Preset         | Config                  | Author  | Steps per cycle | Cycles | Notes                                  |
|----------------|-------------------------|---------|-----------------|--------|----------------------------------------|
| `extreme`      | Extreme1                | anta777 | 31              | 3      | default; the popular OC stability test |
| `absolut`      | ABSOLUT (01102021)      | anta777 | 31              | 3      | heavier, longer                        |
| `universal2`   | Universal2              | LMHz    | 31              | 3      | quicker, broad                         |
| `pcbdestroyer` | PCBDestroyer            | Mechan0 | 22              | 32     | very long, copy-heavy                  |

The files are the originals as collected by
[integralfx/MemTestHelper](https://github.com/integralfx/MemTestHelper/tree/oc-guide/TM5-Configs).
Put any other TM5 `.cfg` file into `~/.config/tm5linux/configs/` (or use *Load a TM5 config file…*
in the app) and it appears in the list.

## Verifying the engine

`tm5linux-cli --selftest` runs every test function on a small buffer twice: once normally (no error
may be reported) and once with simulated bit flips injected after the write phases (every flip must
be found, no more, no less). The installers and packages run it before installing.
