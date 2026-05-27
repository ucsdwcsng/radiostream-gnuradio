# dpdk_bridge

The DPDK app that bridges the `gr-custom_dpdk` GNU Radio OOT blocks to the
RFSoC 4x2 FPGA over 100 GbE.

Originally authored by Jeeva Keshav S; lightly extended (`AARON config:`
markers in the source) to read runtime configuration from `/dev/shm/` —
specifically the per-direction NCO frequency and the 20-slot UL/DL schedule
that the GR blocks write at construction.

## Build

```bash
make
```

Requires `libdpdk` discoverable via `pkg-config`. Produces the ELF
`./dpdk_slot_transmission` (gitignored).

## Run

```bash
sudo taskset -c 5 ./dpdk_slot_transmission --proc-type=auto -a 0000:b3:00.1
```

`-l`/`-a`/`--proc-type` etc. are DPDK EAL options. `-a` selects the NIC PCI
address; pin to a dedicated isolated core with `taskset`.

NIC must already be bound to `vfio-pci` and have RS-FEC enabled — see the
top-level [SETUP.md](../SETUP.md) for the full host setup.

## Runtime config (read once at startup)

After `rte_eal_init()`, the bridge calls `load_runtime_config()`, which reads:

| File | Written by | Format | Compiled-in fallback |
|---|---|---|---|
| `/dev/shm/dpdk_slot_schedule.conf` | GR `dpdk_downlink_sink` | 20 ints, each `0`/`1`/`2`/`3` | all UL `{1×20}` |
| `/dev/shm/dpdk_nco_ul_mhz.conf` | GR `dpdk_uplink_source` | one `double`, MHz | `0x2FAAAAA9F460` (≈ −4 GHz) |
| `/dev/shm/dpdk_nco_dl_mhz.conf` | GR `dpdk_downlink_sink` | one `double`, MHz | `0x1A0AAAAAB7B0` (500 MHz) |

Schedule semantics: `0` = idle, `1` = UL, `2` = DL, `3` = both UL and DL.

Each `.conf` file is **optional**: if absent, the compiled-in default is
used (which lets standalone runs without a GR flowgraph behave exactly as
the team's baseline did). If present but malformed (e.g., wrong number of
schedule entries, out-of-range value), the bridge `rte_exit`s with a clear
message.

NCO MHz → 48-bit FPGA tuning word conversion is
`word = round(f_MHz / 4915.2 × 2⁴⁸) mod 2⁴⁸` (`mhz_to_word()` in the source).
The low 16 bits of the original hardcoded literals are don't-care, so the
MHz-derived word reproduces the carrier to better than 1 Hz.

## Startup ordering (important)

The bridge cannot finish initializing without the GR flowgraph:

1. Bridge `connect()`s to `/dev/shm/ul_data.sock` — created by the GR
   `dpdk_uplink_source` block — and **retries until that socket exists**.
2. Bridge then `bind()` + `listen()` on `/dev/shm/dl_data.sock` and
   **blocks on `accept()`** until the GR `dpdk_downlink_sink` connects.

So the GR flowgraph must be running *before* the bridge is launched, and
both source and sink blocks must be present.

## Output

- **stdout**: per-slot latency prints
  (UL `FPGA→DPDK`, UL `DPDK→Consumer`, DL `Producer→DPDK`, etc.) and the
  `CONFIG: ...` summary at startup.
- **`total_dl_latency.bin`**: binary log of per-slot DL latency in
  microseconds, written at exit.

  ⚠️ **Known pre-existing bug**: `log_dump()` writes
  `sizeof(uint64_t) * log_idx` bytes from a `uint16_t` buffer, so the file
  is 4× the intended size and contains stale memory past the real samples.
  Preserved verbatim from the team's version — fix at your discretion.
