#!/usr/bin/env python3
"""
Convert a DRS4 MIDAS .mid.lz4 file to a ROOT file with one tree:

  - t_wave: one entry per (board, channel) per DRS4 event. Branches:
        brd         (int32)   board index (0..N-1)
        tstamp_us   (int64)   Unix timestamp of the event, microseconds
        freq        (float)   true sampling frequency (GHz)
        trigger_cell(int32)   T-marker position on the time axis
        channel     (int32)   physical channel (0..3)
        time[1024]  (float32) chronological time array, [0, total_ns] ns
        wave[1024]  (float32) waveform in mV, offsetCalib + extrapolated

The DR<NN> bank payload is the same one written by DRS4Frontend::
fill_midas_banks() in src/drs_frontend_class.cxx (line 1516) and copied
verbatim from the readout ring buffer (see the comment at line 1299).

Usage:
    python drs4_midas2root.py run00004.mid.lz4 [-o output.root] [--per-channel]

  --per-channel (default): one row per (board, channel) per event
  --per-event:             one row per (board) per event with 4 channels of
                           (time[1024], wave[1024]) branches
"""

import sys, os, struct, re, argparse
import numpy as np
import lz4.frame

try:
    import ROOT as R
except ImportError:
    print("Error: PyROOT not found.", file=sys.stderr)
    sys.exit(1)


DRS4_NSAMPLES = 1024
DRS4_NCHANNELS = 4
BANK_FORMAT_VERSION = 0x01
BANK_FORMAT_32BIT   = 0x10
TID_BYTE            = 1


# ---------- MIDAS file parsing ----------

def decompress_midas(path):
    with open(path, 'rb') as f:
        return lz4.frame.decompress(f.read())


def parse_midas_events(raw):
    """Yield (event_id, trigger_mask, serial, timestamp, event_data) tuples.

    MIDAS event header on disk is 16 bytes:
        event_id   uint16
        trig_mask  uint16
        serial     uint32
        timestamp  uint32
        data_size  uint32
    followed by `data_size` bytes of payload.
    """
    offset = 0
    while offset + 16 <= len(raw):
        event_id  = struct.unpack_from('<H', raw, offset)[0]
        trig_mask = struct.unpack_from('<H', raw, offset + 2)[0]
        serial    = struct.unpack_from('<I', raw, offset + 4)[0]
        timestamp = struct.unpack_from('<I', raw, offset + 8)[0]
        data_size = struct.unpack_from('<I', raw, offset + 12)[0]

        if offset + 16 + data_size > len(raw):
            break

        data = raw[offset + 16 : offset + 16 + data_size]
        offset += 16 + data_size

        if event_id == 0x8000:
            continue  # file header (ODB JSON dump)

        yield event_id, trig_mask, serial, timestamp, data


def parse_banks(event_data):
    """Yield (bank_name, bank_type, bank_data) tuples from one MIDAS event.

    Handles both the legacy 16-bit MIDAS bank format and the current
    BANK32 format (the one used by bk_init32 / bk_create in this frontend).
    Detection is on the flags field at offset 4..7: 0x11 means
    BANK_FORMAT_VERSION | BANK_FORMAT_32BIT.

    BANK32 per-bank layout (12-byte header):
        name[4]  (4 bytes ASCII, no NUL)
        type     uint32
        size     uint32
        data     `size` bytes

    Legacy BANK per-bank layout (8-byte header):
        name[4]
        type     uint16
        size     uint16
        data     `size` bytes

    Both formats share an 8-byte event header:
        data_size uint32
        flags     uint32   (legacy: num_banks; BANK32: format flags)
    """
    if len(event_data) < 8:
        return

    flags = struct.unpack_from('<I', event_data, 4)[0]

    if (flags & 0xFF) == (BANK_FORMAT_VERSION | BANK_FORMAT_32BIT):
        # BANK32: walk the event, stopping when we run out of data.
        off = 8
        while off + 12 <= len(event_data):
            name = event_data[off:off+4].decode('ascii', errors='replace').rstrip('\x00')
            btype = struct.unpack_from('<I', event_data, off + 4)[0]
            dsize = struct.unpack_from('<I', event_data, off + 8)[0]
            if off + 12 + dsize > len(event_data):
                break
            bdata = event_data[off + 12 : off + 12 + dsize]
            off += 12 + dsize
            yield name, btype, bdata
    else:
        # Legacy 16-bit bank format. `flags` is actually num_banks.
        num_banks = flags
        off = 8
        for _ in range(num_banks):
            if off + 8 > len(event_data):
                break
            name = event_data[off:off+4].decode('ascii', errors='replace').rstrip('\x00')
            btype = struct.unpack_from('<H', event_data, off + 4)[0]
            dsize = struct.unpack_from('<H', event_data, off + 6)[0]
            if off + 8 + dsize > len(event_data):
                break
            bdata = event_data[off + 8 : off + 8 + dsize]
            off += 8 + dsize
            yield name, btype, bdata


# ---------- DRS4 bank decoder ----------

def decode_drs4(bdata):
    """Decode a DR<NN> bank into per-channel arrays.

    Layout (matches drs_frontend_class.cxx readout_loop + fill_midas_banks):

        uint32  board_id
        uint32  trigger_cell       (T-marker position on the time axis)
        uint32  n_channels
        float   freq               (GHz)
        uint64  tstamp_us          (Unix time, microseconds)
        per channel:
            uint32  channel_id
            float   time[1024]
            float   wave[1024]
    """
    if len(bdata) < 24:
        return None

    board_id     = struct.unpack_from('<I', bdata,  0)[0]
    trigger_cell = struct.unpack_from('<I', bdata,  4)[0]
    n_channels   = struct.unpack_from('<I', bdata,  8)[0]
    freq         = struct.unpack_from('<f', bdata, 12)[0]
    tstamp_us    = struct.unpack_from('<Q', bdata, 16)[0]

    ch_data_size = 4 + DRS4_NSAMPLES * 4 * 2
    expected = 24 + n_channels * ch_data_size
    if len(bdata) < expected or n_channels == 0 or n_channels > DRS4_NCHANNELS:
        return None

    channels = []
    for ch in range(n_channels):
        base = 24 + ch * ch_data_size
        channel_id = struct.unpack_from('<I', bdata, base)[0]
        time_arr = np.frombuffer(bdata, dtype=np.float32,
                                 count=DRS4_NSAMPLES,
                                 offset=base + 4).copy()
        wave_arr = np.frombuffer(bdata, dtype=np.float32,
                                 count=DRS4_NSAMPLES,
                                 offset=base + 4 + DRS4_NSAMPLES * 4).copy()
        channels.append({
            'channel_id': channel_id,
            'time': time_arr,
            'wave': wave_arr,
        })

    return {
        'board_id':     board_id,
        'trigger_cell': trigger_cell,
        'n_channels':   n_channels,
        'freq':         freq,
        'tstamp_us':    tstamp_us,
        'channels':     channels,
    }


# ---------- Tree builders ----------

def build_tree_per_channel(events, run_number, output_file):
    """One entry per (board, channel) per event."""
    rf = R.TFile(output_file, "RECREATE")
    R.TParameter(int)("RunNumber", run_number).Write()

    brd          = np.zeros(1, dtype=np.int32)
    tstamp       = np.zeros(1, dtype=np.int64)
    freq         = np.zeros(1, dtype=np.float32)
    trigger_cell = np.zeros(1, dtype=np.int32)
    channel      = np.zeros(1, dtype=np.int32)
    time_arr     = np.zeros(DRS4_NSAMPLES, dtype=np.float32)
    wave_arr     = np.zeros(DRS4_NSAMPLES, dtype=np.float32)

    tree = R.TTree("t_wave", "DRS4 waveforms (per channel)")
    tree.Branch("brd",         brd,          "brd/I")
    tree.Branch("tstamp_us",   tstamp,       "tstamp_us/L")
    tree.Branch("freq",        freq,         "freq/F")
    tree.Branch("trigger_cell", trigger_cell, "trigger_cell/I")
    tree.Branch("channel",     channel,      "channel/I")
    tree.Branch("time",        time_arr,     "time[1024]/F")
    tree.Branch("wave",        wave_arr,     "wave[1024]/F")

    n_entries = 0
    for ev in events:
        for ch in ev['channels']:
            brd[0]          = ev['board_id']
            tstamp[0]       = ev['tstamp_us']
            freq[0]         = ev['freq']
            trigger_cell[0] = ev['trigger_cell']
            channel[0]      = ch['channel_id']
            time_arr[:]     = ch['time']
            wave_arr[:]     = ch['wave']
            tree.Fill()
            n_entries += 1

    tree.Write()
    rf.Close()
    return n_entries


def build_tree_per_event(events, run_number, output_file):
    """One entry per board per event, with 4 channel branches."""
    rf = R.TFile(output_file, "RECREATE")
    R.TParameter(int)("RunNumber", run_number).Write()

    brd          = np.zeros(1, dtype=np.int32)
    tstamp       = np.zeros(1, dtype=np.int64)
    freq         = np.zeros(1, dtype=np.float32)
    trigger_cell = np.zeros(1, dtype=np.int32)
    n_channels   = np.zeros(1, dtype=np.int32)
    ch_id        = [np.zeros(1, dtype=np.int32)  for _ in range(DRS4_NCHANNELS)]
    ch_time      = [np.zeros(DRS4_NSAMPLES, dtype=np.float32) for _ in range(DRS4_NCHANNELS)]
    ch_wave      = [np.zeros(DRS4_NSAMPLES, dtype=np.float32) for _ in range(DRS4_NCHANNELS)]

    tree = R.TTree("t_wave", "DRS4 waveforms (per event)")
    tree.Branch("brd",         brd,          "brd/I")
    tree.Branch("tstamp_us",   tstamp,       "tstamp_us/L")
    tree.Branch("freq",        freq,         "freq/F")
    tree.Branch("trigger_cell", trigger_cell, "trigger_cell/I")
    tree.Branch("n_channels",  n_channels,   "n_channels/I")
    for c in range(DRS4_NCHANNELS):
        tree.Branch(f"ch{c}_id",   ch_id[c],   f"ch{c}_id/I")
        tree.Branch(f"ch{c}_time", ch_time[c], f"ch{c}_time[1024]/F")
        tree.Branch(f"ch{c}_wave", ch_wave[c], f"ch{c}_wave[1024]/F")

    n_entries = 0
    for ev in events:
        brd[0]          = ev['board_id']
        tstamp[0]       = ev['tstamp_us']
        freq[0]         = ev['freq']
        trigger_cell[0] = ev['trigger_cell']
        n_channels[0]   = ev['n_channels']
        for c in range(DRS4_NCHANNELS):
            ch_id[c][0]  = -1
            ch_time[c][:] = 0.0
            ch_wave[c][:] = 0.0
        for ch in ev['channels']:
            c = ch['channel_id']
            if 0 <= c < DRS4_NCHANNELS:
                ch_id[c][0]  = c
                ch_time[c][:] = ch['time']
                ch_wave[c][:] = ch['wave']
        tree.Fill()
        n_entries += 1

    tree.Write()
    rf.Close()
    return n_entries


# ---------- Main ----------

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("midas_file", help="Path to .mid.lz4 file")
    ap.add_argument("-o", "--output", help="Output .root path (default: <midas>.root)")
    ap.add_argument("--per-event", action="store_true",
                    help="One row per event with 4 channel branches (default: per-channel)")
    args = ap.parse_args()

    midas_file = args.midas_file
    output_file = args.output
    if output_file is None:
        base = os.path.splitext(os.path.splitext(midas_file)[0])[0]
        output_file = base + ".root"

    m = re.search(r'run(\d+)', midas_file, re.IGNORECASE)
    run_number = int(m.group(1)) if m else 0
    print(f"Run {run_number}: {midas_file} -> {output_file}")

    raw = decompress_midas(midas_file)
    print(f"Decompressed {len(raw):,} bytes")

    events = []
    n_skipped = 0
    for evid, trig_mask, serial, ts, evdata in parse_midas_events(raw):
        for bname, btype, bdata in parse_banks(evdata):
            if not (bname.startswith("DR") and bname[2:].isdigit()):
                continue
            ev = decode_drs4(bdata)
            if ev is None:
                n_skipped += 1
                continue
            events.append(ev)

    print(f"Decoded {len(events)} DRS4 events "
          f"({n_skipped} bank(s) skipped due to size/layout mismatch)")

    if not events:
        print("No DRxx banks found; nothing to write.", file=sys.stderr)
        sys.exit(1)

    boards = sorted({e['board_id'] for e in events})
    print(f"Boards seen: {boards}")

    if args.per_event:
        n = build_tree_per_event(events, run_number, output_file)
    else:
        n = build_tree_per_channel(events, run_number, output_file)

    print(f"  t_wave: {n} entries")
    print("Done.")


if __name__ == "__main__":
    main()
