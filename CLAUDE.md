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