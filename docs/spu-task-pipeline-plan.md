# SPU-Task Pipeline — Implementation Plan

**Goal:** get *The Simpsons Arcade Game* rendering by making its CRI/SPURS SPU
task pipeline genuinely execute, instead of band-aiding the synchronization.

This is a **dedicated, multi-week effort**. It was confirmed (cont. 42–50) that the
game routes its *entire* execution — including rendering — through CRI middleware
running as SPURS job2 SPU tasks. Every band-aid attempt fails the same way: faking
the synchronization feeds **invalid data** downstream (e.g. the render generator
infinite-loops reading the task struct as a GCM command list). There is no shortcut;
the pipeline must produce valid command lists.

---

## 1. What we know (reversed data model)

### Threads (PPU)
| Thread | Entry | Role | Blocks on |
|---|---|---|---|
| `crCommandQueue` | `0x189288` (func `0x130A70`) | CRI command processor; **executes GCM/render** | sem=6 (command-available), then the completion gate |
| `crTaskQueue` | `0x189290` (func `0x131710`) | CRI task queue | sem=2 |
| main | (entry thread) | init / coordinator | sem=1/2 |
| render-gen | (in `func_00135624`) | walks CRI cmd stream → GCM methods | sem=3 (no consumer) |
| DRM / License | `0x187A10`/`0x187A20` | one-shot checks | exit early |

### The completion handshake (crCommandQueue)
`func_00130A70` → `func_00130AD0` loop:
1. `loc_00130AE0`: `sem_WAIT sem=6` — wait for a command.
2. `loc_00130B58`: process the command list (`func_00131460` = ready predicate).
3. `loc_00130BDC`: **producer/consumer counter wait** — spin `usleep(0x12C)` until
   `[0x22A6BC] == [0x22A6B8]` (consumer catches producer).
4. `func_00130DF0` → `func_00130E24`: **per-task `0xC1` busy gate** —
   `vm_read16(task+8) == 0xC1` (task `@0xCCE280`); if clear → completion.
5. `func_00130CB0`: calls `func_001355A0` (render gen) **then** `sem_post(0x5E)` the
   completion sem → wakes the waiter → game proceeds.

So crCommandQueue posts its **own** completion once it detects done (counter + flag).
No separate SPURS event handler is needed. **But** `func_00130CB0` runs the render
gen first, and the render gen needs valid command data.

### The render path
`func_00130CB0` → `func_001355A0` → `func_00135624` (the CRI command interpreter):
- Reads a command stream at `gpr[5] = [gpr16+0x18]`.
- Jump-table dispatch at `loc_001356C8`: `opcode = vm_read16(stream) - 128`,
  table base `[toc-0x3B3C]`, handlers are the `.word` offsets after the dispatch.
- Generates RSX/GCM method words (e.g. `0x00041710`, `0x00041D6C`) into the cmd buffer.
- `func_00010780` = the **FIFO put-advance** (`cellGcmGetControlRegister` +
  `cellGcmAddressToOffset` → write put).
- `func_000107D8` = command-buffer **reserve** (`usleep(30)` until a GPU/worker sets
  `[ptr+8] == needed`).

### CellGcmContextData (the GAME's layout — FIXED in gcm_bridge)
`begin@+0, end@+4, current@+8, callback@+0xC`. (The old `callback@+0` layout caused
garbage indirect calls + spurious flushes.)

### The SPU jobs (CRI middleware — `crTaskRunJobList`)
- `cellSpursJobChainAttributeInitialize` (NID `0x3548F483`): r4 = cmd list EA
  (`0x330000`, empty), r6 = descriptors array (`0x22A6E8`), name = `crTaskRunJobList`.
- Descriptors array `@0x22A6E8`: 2 jobs — `0xCD0380`→binary `0x168100`,
  `0x22C880`→binary `0x172500`.
- Job descriptors are **sparse**: `0xCD0380` has only `eaBinary@+4`, `+8=0x0A400008`,
  `eaHList@+0x34=0xCD0480` (empty); user-data `+0xF8 = 0`. The work-count gate at
  SPU `0x9040` reads `desc[+0x0A]` → computes **0 work**.
- The SPU job (lifted `cri_job.c`) RUNS and returns cleanly (`0x7428`), but does no
  work because the descriptor/context is empty.

### Job2 ABI (SPU side, proven cont.33)
- `r3` = `CellSpursJobContext2*` (LS, non-null, 8-aligned)
- `r4` = `CellSpursJob256*` (LS, 8-aligned)
- entry = CellSpursJob2 header (4×`ila`) → real code at `0x71D0`
- gates: `r3==0`→exit; `(r4&7)==0`→work `0x9040`; `(desc[0x0A]-derived & 8191)==0`→no-work

### Root cause (deepest point, deterministic)
The descriptors/command-lists are **empty/sparse** because the SPURS+CRI setup is
stubbed, so the SPU jobs have nothing real to do, so no valid command list is
produced, so the render gen loops on garbage. **The whole pipeline must run.**

---

## 2. Architecture decision: HLE the SPURS job2 dispatch (rpcs3-style)

Two options were explored:
- **LLE** (run the firmware SPU kernel/policy): the kernel runs and dispatches, but
  the policy (`spu_0001`) is a nested scheduler that self-recurses via `bisl 0xA00`
  (it never rewrites `LS[0x3FF80]` bytes 4-7 to a job EA on the reachable path). The
  launch-context plumbing is solved (gpr[2]=instance EA, gpr[4]=arg) but the
  job-chain-walk is gated behind a returning `bisl 0xA00` that never returns.
- **HLE** (reimplement the dispatch in C++): run the lifted SPU *jobs* directly with
  a C++-built job2 context. Sidesteps the policy recursion. **Recommended.**

**Plan: HLE the SPURS job2 + crTask dispatch in C++, running the lifted SPU jobs.**

> **Revision (cont. 50):** rpcs3 *also runs the firmware SPU policy* for job chains —
> it does **not** reimplement the job-chain command-walk in C++ (its source has no
> CellSpursJobChain struct layout or command encoding, because the SPU code does the
> walk). This means a pure-C++ HLE of the command walk would require reversing the
> command format from scratch (no reference). The more faithful and better-referenced
> path is **LLE: run the lifted policy/job**, where the single key blocker is already
> identified:
>
> **THE policy self-dispatch recursion.** `spu_0001` (the job-chain policy) loops
> `1148(entry) → FE8 → 1120 → bisl 0xA00 → 1148 …`. In the runtime,
> `spu_indirect_branch` is a **recursive C call** (`fn(ctx); return;`), and `0xA00`
> resolves to the policy itself (because `LS[0x3FF80]` bytes 4-7 = the policy EA and
> nothing on the reachable path rewrites it to a job EA), so it self-recurses instead
> of loading+running a job and returning. The job-chain-read code (`B90`/`BF8`/`F04`)
> that *would* rewrite `LS[0x3FF80]` bytes 4-7 to the next job's binary EA sits
> **after** the `bisl 0xA00` return point — unreachable while the bisl never returns.
> Resolving this (understand how the firmware loads a *job* at `0xA00` that returns,
> so `B90` runs and advances the chain) is the crux of the LLE path. The launch
> context is already solved (gpr[2]=instance EA, gpr[4]=arg, both persist).
>
> **So the real first decision for the dedicated project is: LLE (solve the policy
> recursion / job-load-at-0xA00) vs. HLE (reverse the command format from scratch).**
> LLE is recommended — better referenced, and most of the firmware already lifts+runs.

---

## 3. Staged implementation

### Stage 0 — foundation (DONE this session)
- ✅ Determinism (in-thread counter converge made the boot reproducible).
- ✅ `CellGcmContextData` layout fix.
- ✅ In-thread completion primitives (`0xC1` clear, counter converge).
- ✅ Complete render-path map (this doc).
- ✅ Boot reaches full render init + live D3D12 window.

### Stage 1 — crTask data model (RE)
Reverse the CRI `crTask` manager structures so we can drive the SPU job with real input:
- Find the crTask manager EA (near `0x22A6xx`; the producer/consumer counters are
  `0x22A6B8`/`0x22A6BC`, descriptors `0x22A6E8`).
- Map the task ring + how a task references its work (the GCM command list to generate).
- Determine where the SPU job (`0x168100` = crTaskRunJobList SPU runtime) reads the
  ring from (its arg / user-data — currently 0, so the game's setup is incomplete
  under our stubs; may need to *not* stub the CRI init so the game builds it).

### Stage 2 — job2 context construction (C++)
- Build a valid `CellSpursJobContext2` in LS from the job descriptor's I/O DMA list
  (`eaHList`): DMA the inputs to LS, allocate output buffers, fill the context.
- Run the lifted SPU job (`cri_job.c`) with `r3`=context, `r4`=descriptor.
- Verify it takes the work path (`0x9040`) and DMAs real output.

### Stage 3 — completion (C++)
- After the job returns, perform the SPURS completion: advance the consumer counter,
  clear the task `0xC1` flag, post the workload completion signal — **coherently** so
  crCommandQueue's own loop posts its completion sem. (The in-thread primitives from
  Stage 0 are the model.)

### Stage 4 — GPU worker (present thread)
- Make the D3D12 present thread the **sem=3 GPU consumer**: process the FIFO
  (`rsx_process_command_buffer`) + advance the GCM-reserve completion counter
  (`func_000107D8`'s `[ptr+8]`), so the render gen finishes.

### Stage 5 — draws
- With valid command lists flowing, the render gen's GCM methods reach the FIFO →
  D3D12 backend → first frame.

---

## 4. Open questions / risks
- **Is the CRI init genuinely incomplete under our stubs, or do we read the wrong
  descriptor?** The descriptors are sparse at dispatch — confirm whether the game
  builds real ones later (it can't, because it's stuck) → may need to un-stub the CRI
  init path so real structures get built, then run the jobs.
- **Job2 context layout** for *this* libsre version (rpcs3's is a reference, not exact).
- **Multi-thread coherence** — the completion touches state on several threads;
  the in-thread/synchronous model avoids races but must be applied at every gate.
- **The second job `0x172500`** (descriptor `0x22C880`, `+8=0x330000` = the cmd list)
  — likely the one with real work; investigate alongside `0x168100`.

---

## 5. Cleanup debt (before/while building)
Strip or fold the bring-up scaffolding so the codebase is clean for the real impl:
- `ppu_recomp.cpp` probes: `[criQ]` (B58/COMPLETION/BDC/RENDER-GEN/CRI-cmd/FLUSH-CB/
  356B0/GCM-reserve), the in-thread `0xC1` clear (`func_00130E24`) + counter converge
  (`loc_00130BDC`) — **fold these into the lifter/generator** (a re-lift drops them).
- `vm_bridge.cpp`: the watchdog, `g_cri_wedged` sem-skips, usleep counter-force.
- `gcm_bridge.cpp`: keep the `CellGcmContextData` fix; the get=put fake + flip driver
  become real once the GPU-worker (Stage 4) is implemented.
- `spu_runner_spurs.cpp` + the SPURS kernel/policy LLE harness (if going HLE).
