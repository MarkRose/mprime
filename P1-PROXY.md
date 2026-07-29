# Sharing P-1 stage 1 residues with a ppp proxy

These are the changes in this tree that let Prime95/mprime hand its P-1 stage 1
residues to a local `ppp` proxy, so a machine with more memory can run stage 2
properly instead of the work being thrown away.

Implemented and verified end to end against 31.6, on top of a pristine import.
File references are to this tree; the proxy and the wire protocol it speaks are
documented separately, in that project's `docs/protocol.md`.

None of it engages when talking to PrimeNet directly. The proxy announces itself
by adding `AcceptsP1Uploads=1` to the `t=uc` reply; with no proxy in the path the
key is absent, the setting is cleared, and the client behaves exactly as stock.

## What changed

| File | Change |
|---|---|
| `primenet.h` | `p1_upload_capable` / `p1_download_capable` and the returned `accepts_p1_uploads` on the `uc` packet; `p1_download_url` on the `ga` packet |
| `primenet.c` | Send `p1up`/`p1dl` on `uc`; parse `AcceptsP1Uploads` and `URL` |
| `commonc.c/.h` | New `[PrimeNet]` keys, uploader thread, fetch a residue when a `w=3` assignment carries a `URL` |
| `ecm.cpp` | Stage the stage 1 residue for upload instead of unlinking it |
| `p1_transfer.c` | New. Upload (chunked, resumable) and download (streamed) |
| `*/os_routines.c`, `comm95c.c` | `P1FileNames` on all four POSIX targets plus Windows, so no platform build breaks |

## The blocker, and how it was solved

`ecm.cpp:13791-13793` unlinks the save file and only re-creates it for
`WORK_PMINUS1`:

```c
unlinkSaveFiles (&pm1data.write_save_file_state);
if (!QA_IN_PROGRESS && w->work_type == WORK_PMINUS1 && IniGetInt (INI_FILE, "KeepPminus1SaveFiles", 1))
        pm1_save (&pm1data);
```

PrimeNet assigns P-1 as `Pfactor=` (`WORK_PFACTOR`), so that branch never fires
and the residue is always destroyed. The added block re-saves and stages it,
gated on having actually finished stage 1 without stage 2 (`C <= B`) and on a
proxy having said it wants residues.

## Staging, and why not the save file name

A staged residue is named **`p1send.<exponent>`**, not `mNNNNNNN`.

The uploader scans the working directory, and anything matching a save file
pattern risks picking up a file belonging to work that is *still running* —
uploading a live save file would be a real bug. The plain decimal exponent also
avoids reversing `tempFileName`'s character mangling to recover it.

The rename from `mNNNNNNN` is atomic and free; the save file has already served
its purpose for a work unit that is about to complete.

## Configuration

New keys in the `[PrimeNet]` section of `prime.txt`:

| Key | Default | Meaning |
|---|---|---|
| `P1Uploads` | 1 | Master switch. Also gates the `p1up` capability we advertise. |
| `AcceptsP1Uploads` | 0 | **Written by mprime, not by you.** Rewritten on every `uc`, so removing the proxy clears it. |
| `P1UploadIP` | *(falls back to `MersenneIP`)* | Host for `/p1_upload/` and `/p1_download/`. |

`P1UploadIP` defaults to `MersenneIP` because the proxy is the same host serving
the v5 API — pointing a client at a proxy should not require configuring it
twice. It accepts a bare host, `host:port`, or a full URL.

Existing keys are reused rather than duplicated: `UploadRateLimit`,
`UploadChunkSize`, `UploadStartTime`/`UploadEndTime` and `DownloadRateLimit` all
apply to residue transfers exactly as they do to proof transfers.

## Verified behaviour

Built and linked on `linux64` against gwnum 31.6, then run against a live proxy.

**Registration.** mprime sent, with the parameters in the required order
(`p1up`/`p1dl` before `ss`/`sh`):

    t=uc&g=f4cb...&hg=099a...&a=Linux64%2CUntrusted%20Prime95%2Cv31.6%2Cbuild%201
    ...&m=31719&p1up=1&p1dl=1

The proxy logged `p1_upload=true p1_download=true`, and mprime persisted
`AcceptsP1Uploads=1` into `[PrimeNet]`. Real PrimeNet accepted the same request
shape, unknown parameters and all.

**Download.** Given a `w=3` assignment carrying a `URL=`, mprime logged:

    Got assignment AAAA...1111: P-1 M118000019
    Downloading P-1 stage 1 residue for M118000019
    Stage 1 residue for M118000019 is in place; resuming at stage 2
    P-1 on M118000019 with B1=1000000, B2=TBD

and wrote the worktodo line

    Pminus1=AAAA...1111,1,2,118000019,-1,1000000,100000000

**`B2=TBD` is the payoff.** mprime prints that only when `optimal_B2` is TRUE
(`ecm.cpp:11755-11757`), confirming the `B2 = 100 × B1` requirement works and
that mprime will compute its own optimum at stage 2 start, against the memory
actually available. Any other B2 would have printed a fixed number instead.

**Integrity.** The test residue was random bytes, and mprime rejected it rather
than trusting it:

    Error reading intermediate file: m118000019
    Renaming m118000019 to m118000019.bad1

That is `read_header`'s `k/b/n/c` check and `pm1_restore`'s checksum doing their
job, which is why the proxy relays residues byte for byte and never rewrites one.

**Upload.** A staged `p1send.118000031` was picked up, sent in two chunks
(2 MiB — the default at `UploadRateLimit=1.0` — then the 402,848-byte remainder),
and deleted locally:

    Sending stage 1 residue for M118000031 to the P-1 proxy (2500000 bytes)
    Stage 1 residue for M118000031 uploaded to the P-1 proxy

The stored file matched the original MD5 exactly.

## Verified with a real P-1 run

Not just synthetic residues. A worker with `[Worker #1] Memory=8` computed
stage 1 of M5000011 (77 s, B1=218000), and because `max_mem()` returns the lesser
of the global `MAX_MEM` and the per-worker value — and the per-worker value has
no 256 MB floor (`commonb.c:1483-1509`) — stage 2 was ruled out and the residue
was staged:

    Optimal P-1 factoring of M5000011 using up to 8MB of memory.
    Optimal bounds are B1=218000, B2=215000
    M5000011 completed P-1, B1=218000
    Stage 1 residue for M5000011 saved for upload to the P-1 proxy.

The result went out as `&k=0&...&B1=218000&B2=218000` — B2 equal to B1, the
discriminator the proxy keys on — the 625,084-byte residue (~n/8) uploaded in
two chunks, and a second worker with `Memory=4000` picked it up:

    P-1 on M5000011 with B1=218000, B2=TBD
    Resuming P-1 in stage 2 with B2=76219500
    M5000011 completed P-1, B1=218000, B2=76219500

It chose B2 = 350× B1, well past the 100× the proxy offered, which is the whole
point of leaving that decision to the client.

## Deployment constraint

The low-memory worker must reach the **hard** gate above, which reports a result.
The **soft** gates do not work: `MaxHighMemWorkers`, `LowMemWhileRunning` and
`OnlyRunStage2WithMaxMemory` all return `STOP_NOT_ENOUGH_MEM`, which makes the
worker defer and look for other work *without submitting anything*
(`commonb.c:3326-3329`). That leaves a larger `MIDSTAGE` file on disk and no `ar`
for the proxy to act on.

Harvesting that second path — arguably the cleaner source of residues, since
`MaxHighMemWorkers=0` is the existing knob for "this machine only does stage 1"
(`commonb.c:1160-1161`) — needs a different trigger and remains out of scope.

## Building

The tree needs `gwnum.a` and `polymult.a` at `gwnum/`, which are not shipped
there. `gwnum/linux64/gwnum.a` is prebuilt; the rest builds with:

    cd gwnum && make -f make64

`linux64/makefile` then links `-Wl,-Bstatic -lhwloc -lcurl`, which needs static
libraries most distributions no longer ship. On Arch the dynamic link works:

    cd linux64 && make            # compiles everything, fails at link
    gcc -o mprime prime.o menu.o cJSON.o ecm.o exponentiate.o pair.o pm1prob.o \
        factor64.o ../gwnum/gwnum.a ../gwnum/polymult.a \
        -lm -lpthread -lhwloc -lcurl -lrt -lstdc++ -ldl -lgmp

`pair.cpp` needs boost headers (`pacman -S boost`).

When running mprime for testing, pass **`-w<dir>`**. Without it mprime `chdir`s
to its own executable directory (`prime.c:200`) and reads the `prime.txt` there,
not the one you just wrote.
