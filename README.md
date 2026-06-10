# radiostream-gnuradio

GNU Radio integration for RadioStream RFSoC 4x2 FPGA project; a slot-based
(5G NR-style, 20 slots per SFN) bridge between a GNU Radio flowgraph and a DPDK
app that talks to the FPGA over a 100 Gbps NIC.

## Architecture

```
   ┌────────────────────────┐                                  ┌─────────────────────────┐
   │  GNU Radio flowgraph   │   /dev/shm/dl_data.sock          │  dpdk_bridge            │
   │                        │   ────────────────────────────►  │  (DPDK app, runs as     │
   │  ┌──────────────────┐  │                                  │   root on the host NIC) │       ┌──────────────┐
   │  │ dpdk_downlink_   │  │   /dev/shm/ul_data.sock          │                         │  ───► │              │
   │  │ sink (DL TX)     │  │   ◄────────────────────────────  │  - reads /dev/shm/      │       │  RFSoC 4x2   │
   │  └──────────────────┘  │                                  │    dpdk_*.conf for NCO  │       │  FPGA        │
   │                        │                                  │    + 20-slot schedule   │  ◄─── │  (100 GbE)   │
   │  ┌──────────────────┐  │                                  │  - constructs control + │       │              │
   │  │ dpdk_uplink_     │  │   GR blocks write the NCO +      │    data packets         │       └──────────────┘
   │  │ source (UL RX)   │  │   schedule .conf files at        │                         │
   │  └──────────────────┘  │   construction.                  │                         │
   └────────────────────────┘                                  └─────────────────────────┘
```

## Components

- **[`gr-custom_dpdk/`](gr-custom_dpdk/)**: GNU Radio OOT module exposing two
  blocks in GRC:
  - `dpdk_uplink_source`: FPGA -> host (RX). Parameters: socket path,
    bytes/slot, CPU core, NCO freq (MHz).
  - `dpdk_downlink_sink`: host -> FPGA (TX). Parameters: socket path,
    bytes/slot, CPU core, NCO freq (MHz), 20-element slot schedule
    (0 = idle, 1 = UL, 2 = DL, 3 = both).
- **[`dpdk_bridge/`](dpdk_bridge/)**: the DPDK app the blocks talk to.
  Reads the NCO + schedule from `/dev/shm/*.conf` at startup; builds control
  and data packets for the FPGA.

## Quick start

Full prerequisites and gotchas are in **[SETUP.md](SETUP.md)** (hugepages, NIC
binding to `vfio-pci`, RS-FEC, the `conda deactivate` build dance). TL;DR
on a properly prepared Ubuntu box:

```bash
# 1. Build the OOT module (with conda deactivated so system gcc is used)
conda deactivate
cd gr-custom_dpdk && mkdir -p build && cd build && cmake .. && make && sudo make install
cd ../..

# 2. Build the DPDK bridge
cd dpdk_bridge && make && cd ..

# 3. Start the GR flowgraph first (it creates /dev/shm/ul_data.sock)
#    (open your .grc in gnuradio-companion, then Run)

# 4. Then start the bridge on its dedicated core, pinned to the NIC
sudo taskset -c 5 ./dpdk_bridge/dpdk_slot_transmission \
     --proc-type=auto -a 0000:b3:00.1
```

The bridge will print `CONFIG: ...` lines showing which NCO / schedule values
it picked up from `/dev/shm`, or that it fell back to compiled-in defaults.

## Repo layout

```
radiostream-gnuradio/
├── README.md
├── SETUP.md
├── LICENSE                 # GPL-3.0
├── .gitignore
├── gr-custom_dpdk/         # GNU Radio OOT module (build with CMake)
└── dpdk_bridge/            # DPDK app (build with Makefile)
```

The FPGA bitstreams, Vivado projects, MATLAB analysis, and the older
standalone `producer_dpdk`/`consumer_dpdk` test clients all live in their
own (separate) RadioStream tree — they are *not* part of this repo. See
SETUP.md for the assumed FPGA configuration this bridge expects.

## License

[GPL-3.0-or-later](LICENSE). Required by GNU Radio's licensing; the OOT
module sources already carry `SPDX-License-Identifier: GPL-3.0-or-later`.
