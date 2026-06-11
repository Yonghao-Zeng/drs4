# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This is a MIDAS frontend for DRS4 (Domino Ring Sampler 4) oscilloscope/digitizer boards from PSI. It provides:
- USB control of up to 8 DRS4 boards
- MIDAS DAQ integration with waveform readout into MIDAS banks
- Web-based control UI served via mhttpd
- ODB-based configuration watched in real-time
- Offline analysis: midas→root converter and waveform inspection notebook

## Working Style

Use the full set of available skills/tools for every operation — they exist
to keep work organized, parallelizable, and reversible. Don't fall back to
inline bash or off-the-cuff fixes when a dedicated mechanism is available.

- **Plan non-trivial work**: for multi-step tasks, new features, or changes
  that touch existing behavior, enter plan mode (`EnterPlanMode`) and get
  sign-off before writing code.
- **Track work in tasks**: use `TaskCreate` / `TaskUpdate` for any
  multi-step job. Mark each task in_progress when you start, completed
  when it's done — don't batch completions. The task list also gives
  the user real-time visibility into progress.
- **Clarify ambiguity early**: when a request has several reasonable
  interpretations, or a commit/refactor could reasonably go multiple
  ways, use `AskUserQuestion` to pick a direction before implementing.
- **Delegate when appropriate**: use the `Agent` tool with a specialized
  subagent (e.g. `Explore` for fast code lookup, `general-purpose` for
  multi-step research) to parallelize independent work or to keep large
  search results out of the main context. Write self-contained prompts —
  the subagent doesn't see this conversation.
- **Specialized tools over shell**: prefer `Read` / `Edit` / `Write` /
  `NotebookEdit` for file work; reserve `Bash` for shell-only operations
  (build, run, find, git). Avoid `cat`, `sed`, `awk`, `echo` in Bash.
- **Invoke named skills via `Skill`**: when the user types
  `/<skill-name>` or asks for something that matches a recognized skill,
  invoke it via the `Skill` tool — never guess the implementation.
- **Verify UI/frontend changes in a real browser** before reporting
  them complete. Type checking and unit tests verify code, not features;
  if you can't run the UI, say so explicitly rather than claiming success.
- **Test risky/uncertain work in the background** (`run_in_background`)
  and continue with other work rather than blocking. The harness
  notifies you when the task completes.
- **Be careful with hard-to-reverse actions** (push, force-push, reset,
  rm -rf, drop tables, send messages): confirm with the user before
  executing. Reversible local actions (file edits, tests) are fine
  without confirmation.

## Build Commands

```bash
# Requires MIDAS environment variable MIDASSYS pointing to MIDAS installation
make clean && make

# Or if MIDAS is already set:
make
```

**Dependencies**: MIDAS (libmidas.a, mfe.o), libusb-1.0, libzmq, pthreads

## Architecture

### Key Source Files

| File | Role |
|------|------|
| `src/drs_frontend.cxx` | MIDAS frontend entry point with standard callbacks (frontend_init/exit, begin/end_of_run, poll_event, read_trigger_event), crash signal handler |
| `src/drs_frontend_class.cxx` | `DRS4Frontend` class — board management, ODB watches, readout threads, snapshot writer |
| `src/DRS.cpp` / `include/DRS.h` | Pre-compiled DRS4 API (PSI distribution, no wxMutex) |
| `web/drs4.html` | Web control UI using mjsonrpc for ODB access |
| `analysis/drs4_midas2root.py` | Converts `.mid.lz4` recordings to ROOT files |
| `analysis/drs4_waveforms.ipynb` | Jupyter notebook: load `.root`, plot waveforms |

### Data Flow

1. **Live Preview**: `live_preview_loop()` thread continuously captures waveforms when no DAQ run is active, writes to `drs4_snapshot.json` for web display
2. **DAQ Run**: `readout_loop()` thread captures triggered events into MIDAS ring buffer; `poll_event()`/`fill_midas_banks()` drain the ring buffer into MIDAS banks (DR00, DR01, ...)
3. **Web UI**: Polls `drs4_snapshot.json` and ODB via mjsonrpc for live display
4. **Offline**: `drs4_midas2root.py` parses the .mid.lz4 file and emits a ROOT file; `drs4_waveforms.ipynb` plots it

### ODB Structure

```
/Equipment/DRS4/Settings/         — global Status, Refresh trigger, SnapshotWaveform
/Equipment/DRS4/Settings/Boards/BoardN/  — per-board config: Sampling Rate, TriggerLevel,
                                           TriggerLogic (OR/AND), EnableCH1..4, EnableEXT,
                                           TriggerDelayNs, DominoActive, ReadoutMode, etc.
/Equipment/DRS4History/Variables/ — Temp_BN, TrgRate_BN for history logging
/Custom/DRS4&  →  /home/muon/midasExp/drs4/web/drs4.html  (mhttpd page link)
```

### Key Constants

- `DRS4_MAX_BOARDS = 8`
- `DRS4_NCHANNELS = 4` (physical inputs per board)
- `DRS4_NSAMPLES = 1024` (samples per waveform)

### Threading Model

- `live_preview_loop()` — runs when not in DAQ run; handles continuous oscilloscope capture
- `readout_loop()` — runs during DAQ run; writes to ring buffer
- `update_trigger_rates()` — helper called by both loops (throttled to 5 Hz from `readout_loop`, every iteration from `live_preview_loop`); keeps ODB TrgRate_B{0,1,2,3,4} fresh during a run
- These threads are mutually exclusive on the same `m_readout_thread` pointer (joined at begin/end_of_run transitions)
- **Destructor joins the thread before destroying `m_boards`/`m_drs`**. Without this, Ctrl-C (or any path to `frontend_exit` that doesn't go through `end_of_run`) leaves the loop running and dereferences the dangling board pointers → SIGSEGV in `m_boards[i]->GetScaler()`. Set `m_in_end_of_run = true` and `join()` before freeing hardware state.

### Trigger Source (REG_TRG_CONFIG)

Written by `SetTriggerSource(src_mask)` (DRS.cpp:2304). For board type 8/9:

- OR  path: bit 0=CH1, bit 1=CH2, bit 2=CH3, bit 3=CH4, bit 4=EXT
- AND path: bit 8=CH1, bit 9=CH2, bit 10=CH3, bit 11=CH4, bit 12=EXT
- TRANSP: bit 15 (transparent mode for analog trigger)

`EnableCH1..4` set the CH bits at the active path's offset (0 for OR, 8 for AND). `EnableEXT` sets bit 4 (OR) or bit 12 (AND). Typical use is one or the other (channel triggers vs. external), but the hardware supports mixing — `apply_board_config` simply ORs all enabled sources into `src_mask`.

### DRS4 Mode Notes

- **TriggerMode "Auto"** = free-running (DominoMode=1, continuous)
- **TriggerMode "Normal"** = triggered single sweep (DominoMode=0, waits for hardware trigger)
- Transparent mode (`TranspMode=true`) required for analog trigger
- Channel indexing: physical CH1-4 maps to DRS chip channels 0, 2, 4, 6 (even channels)

### Critical: Readout Mode, Calibration, and Time Axis

**ReadoutMode must be `FROM_STOP`** (ODB default). The DRS4 calibration system
(cellDT per-cell timing, fCellOffset2 secondary offset calibration) is designed
for FROM_STOP readout.  `FROM_FIRST_BIN` produces misaligned time/waveform
arrays that cause signal jumping.

**GetWave parameters** (matching official DRS4 software `Osci.cpp:769`):
- `adjustToClock=false` — waveform in raw readout order (no rotation)
- `offsetCalib=true` — enables secondary per-cell offset correction
  (`fCellOffset2`), eliminating cell-to-cell pattern noise

**GetTime parameters**:
- `rotated=true` — time array aligned with FROM_STOP readout order
- `tcalibrated=true` — use per-cell timing calibration
- `tc=stop_cell` — for DRS4, GetTriggerCell returns GetStopCell

**Chronological reordering**: FROM_STOP readout order is
`[stop, stop+1, ..., stop-1]` which is `[newest, oldest, ..., second-newest]`.
Samples are reordered into chronological order (oldest→newest, left→right)
before display.

**Time axis**: Invariant `[0, total_ns]` regardless of trigger delay.
Left-aligned: `t=0` at the left edge of the screen, `t=total_ns` at the
right.

**TriggerDelayNs (UI semantics)**: The ODB value is the **T marker
position on the time axis** (oscilloscope model), not the raw DRS4 LUT
delay. For UI value 0 the T marker sits at the left edge of the screen
(time 0); for UI value 100 the T marker sits at time 100 ns; the
maximum UI value is `total_ns` (≈205 ns at 5 GHz).

The actual hardware LUT delay sent to the DRS4 is the inverse:
`true_delay = total_ns − user_delay`. The analog trigger fires, the
domino keeps running for `true_delay` ns (≈ LUT ticks × 6.2 ns), then
the stop cell latches — so the rising edge sits at
`time = total_ns − true_delay = user_delay` on the time axis. The
T marker is drawn at `user_delay`, so it lands on the rising edge.

**Hardware note (fixed offset)**: The official `SetTriggerDelayPercent`
adds a `23.5 + 28.2/freq` ns correction to `fTriggerDelayNs` to
approximate the comparator/FPGA propagation delay — that correction
is only applied on the *displayed* `fTriggerDelayNs`; the LUT ticks
written to the hardware are pure (no +29). Since this frontend calls
`SetTriggerDelayNs(delay)` (which stores `delay` directly as
`fTriggerDelayNs` and converts to ticks without the +29), the
`true_delay` we send is the raw LUT delay. No extra offset is needed
in the C++ formula — the +29 only matters when reading back
`GetTriggerDelayNs()` for display, which we don't do.

**Web UI time axis**: `timeToX` maps `t=0` to the left edge of the
screen (left-aligned, positive-only):
`return gridLeft + (t / screenNs) * screenWidth`.

### DRxx Bank Layout

Each MIDAS event contains one DRxx bank per board. The 44-byte header carries
trigger metadata; the per-channel block is 8196 bytes (4 + 1024·4·2). At
`DRS4_NCHANNELS=4` the total payload is 32828 bytes.

```
offset  0  uint32  board_id
offset  4  uint32  trigger_cell       (T-marker position on the time axis)
offset  8  uint32  n_channels         (always 4)
offset 12  float   freq               (true sampling frequency, GHz)
offset 16  uint64  tstamp_us          (Unix time, microseconds)
offset 24  uint32  user_delay_ns      (UI value of TriggerDelayNs at capture)
offset 28  uint32  scaler[4]          (per-channel hardware trigger counts)
offset 44  per-channel blocks:
              uint32  channel_id (0..3)
              float   time[1024]     (chronological, [0..total_ns])
              float   wave[1024]     (mV, after offsetCalib+extrapolation)
```

The legacy 24-byte header (no `user_delay_ns`, no `scaler`) is still
decodable: `drs4_midas2root.py` falls back to it if the buffer is too
short, filling the new fields with zeros, so old `.mid.lz4` files
convert cleanly.

### Atomic Snapshot Write

`do_snapshot()` writes `drs4_snapshot.json` atomically: dump JSON to a
sibling `.tmp`, `fsync()`, then `rename(2)` over the live path. `rename`
is atomic on POSIX, so mhttpd reads either the previous complete
snapshot or the new one — never a half-truncated file. Fixes the
recurring mhttpd errors of the form `read of N returned M < N`. Uses
raw POSIX I/O (`open`/`write`/`fsync`/`close`/`rename`) via `<unistd.h>`,
`<fcntl.h>`, `<sys/stat.h>`.

### DRS4 API Differences vs Official Software

| Aspect | Official (`drsosc`) | This frontend |
|--------|-------------------|---------------|
| Readout | FROM_STOP | FROM_STOP (same) |
| GetWave adjustToClock | `!m_rotated` = false | false (same) |
| GetWave offsetCalib | `m_calibrated2` = true | true (same) |
| GetTime rotated | `m_rotated` = true | true (same) |
| Extrapolation | first 2 samples | first 2 samples (same) |
| Baseline correction | none | none (same) |
| Display order | FROM_STOP raw | chronological reordered |
| Time axis | FROM_STOP order [0, total] | chronological [0, total] |
| T marker position | GetTriggerCell (= stop cell) | user_delay on time axis (UI value = T position) |

## Offline Analysis

`analysis/drs4_midas2root.py runXXXXX.mid.lz4` produces `runXXXXX.root`
with a single `t_wave` tree. Branches (per-channel mode, the default):
`brd` (I), `tstamp_us` (L), `freq` (F), `trigger_cell` (I),
`user_delay_ns` (I), `channel` (I), `scaler[4]` (i), `time[1024]` (F),
`wave[1024]` (F). Use `--per-event` for a row-per-event layout with
explicit `ch0..3` time/wave branches instead.

`analysis/drs4_waveforms.ipynb` loads the file with `uproot`, summarises
the run, and plots: (1) a single event with all 4 channels overlaid and
the T-marker, (2) N events overlaid + mean ± 1σ for one channel,
(3) a small-multiples grid of the first N events, (4) the trigger-cell
and `user_delay_ns` distributions. Uses `entry_start`/`stop` so each
event's arrays are loaded on demand, not all rows up front.
