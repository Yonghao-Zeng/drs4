# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This is a MIDAS frontend for DRS4 (Domino Ring Sampler 4) oscilloscope/digitizer boards from PSI. It provides:
- USB control of up to 8 DRS4 boards
- MIDAS DAQ integration with waveform readout into MIDAS banks
- Web-based control UI served via mhttpd
- ODB-based configuration watched in real-time

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
| `src/drs_frontend.cxx` | MIDAS frontend entry point with standard callbacks (frontend_init/exit, begin/end_of_run, poll_event, read_trigger_event) |
| `src/drs_frontend_class.cxx` | `DRS4Frontend` class — board management, ODB watches, readout threads |
| `src/DRS.cpp` / `include/DRS.h` | Pre-compiled DRS4 API (PSI distribution, no wxMutex) |
| `web/drs4.html` | Web control UI using mjsonrpc for ODB access |

### Data Flow

1. **Live Preview**: `live_preview_loop()` thread continuously captures waveforms when no DAQ run is active, writes to `drs4_snapshot.json` for web display
2. **DAQ Run**: `readout_loop()` thread captures triggered events into MIDAS ring buffer; `poll_event()`/`fill_midas_banks()` drain the ring buffer into MIDAS banks (DRS00, DRS01, ...)
3. **Web UI**: Polls `drs4_snapshot.json` and ODB via mjsonrpc for live display

### ODB Structure

```
/Equipment/DRS4/Settings/         — global Status, Refresh trigger, SnapshotWaveform
/Equipment/DRS4/Settings/Boards/BoardN/  — per-board config (Frequency, TriggerLevel, DominoMode, etc.)
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
- These threads are mutually exclusive (joined at begin_of_run, restarted at end_of_run)

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
maximum UI value is `total_ns − 29ns` (≈171 ns at 5.12 GHz).

The actual hardware LUT delay sent to the DRS4 is the inverse:
`true_delay = (total_ns − 29ns) − user_delay`. This way, the trigger
cell (and the actual signal rising edge it captures) sits at
`user_delay` on the time axis — so the T marker always lands on the
rising edge of the captured signal.

**Hardware note**: The DRS4 evaluation board has a minimum ~29ns fixed
comparator/FPGA propagation delay (`23.5 + 28.2/freq` in
`SetTriggerDelayPercent`). The hardware's trigger cell sits at
`total_ns − (true_delay + 29ns)`. With the UI mapping above,
`true_delay + 29ns = total_ns − user_delay`, so the trigger cell sits
at `user_delay` on the time axis — exactly where the T marker is drawn.

**Web UI time axis**: `timeToX` maps `t=0` to the left edge of the
screen (left-aligned, positive-only):
`return gridLeft + (t / screenNs) * screenWidth`.

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