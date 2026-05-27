# Setup

Tested on Ubuntu 22.04 with kernel 5.15+, DPDK 23.11, and radioconda's
GNU Radio 3.10. Other configurations may need path / version tweaks below.

## Prerequisites

```bash
sudo apt update
sudo apt install -y build-essential pkg-config cmake git ethtool kmod \
                    linux-headers-$(uname -r) python3 python3-pip
```

- **DPDK**: install `libdpdk-dev` from the distro (`sudo apt install libdpdk-dev`)
  or build from source. Verify with `pkg-config --modversion libdpdk`.
- **GNU Radio**: install [radioconda](https://github.com/ryanvolz/radioconda)
  (recommended) **or** the system package `gnuradio-dev`. This repo's OOT
  module is currently built against radioconda — see the
  [hardcoded path note](#hardcoded-radioconda-path) below.

## 1. Hugepages

DPDK requires hugepages. Allocate ~2 GB of 2 MB pages (adjust to taste):

```bash
sudo sysctl -w vm.nr_hugepages=1024
# Persistent across reboots:
echo "vm.nr_hugepages=1024" | sudo tee /etc/sysctl.d/99-hugepages.conf
sudo mkdir -p /mnt/huge
sudo mount -t hugetlbfs nodev /mnt/huge
# To mount persistently, add to /etc/fstab:
#   nodev /mnt/huge hugetlbfs defaults 0 0
```

Verify: `cat /proc/meminfo | grep Huge`.

## 2. Bind the NIC to vfio-pci

The DPDK app takes the NIC away from the kernel. Identify your NIC's PCI
address (e.g., `0000:b3:00.1` on `wcsng-901`):

```bash
sudo modprobe vfio-pci
sudo dpdk-devbind.py --status               # find your NIC
sudo dpdk-devbind.py -b vfio-pci 0000:b3:00.1
```

To rebind to the kernel driver later: `sudo dpdk-devbind.py -b <orig_driver> <pci_addr>`.

## 3. RS-FEC on the NIC (before running)

The DPDK driver in use does not expose FEC config — set it via ethtool first,
on the **kernel-side interface name** (before binding to vfio-pci, *or* on a
sister interface):

```bash
sudo ethtool --set-fec enp179s0f1 encoding rs
```

## 4. radioconda + the "conda deactivate" quirk

The OOT module's binding links against radioconda's `gr_python.cpython-*.so`
(see `gr-custom_dpdk/python/custom_dpdk/bindings/CMakeLists.txt`). But the
**build itself** must use system `gcc`, not conda's, because of ABI
incompatibilities. So: **deactivate conda before invoking cmake/make**, then
reactivate (or use radioconda's `gnuradio-companion`) at runtime.

```bash
conda deactivate                  # use system gcc for the build
cd gr-custom_dpdk
mkdir -p build && cd build
cmake ..
make -j$(nproc)
sudo make install
sudo ldconfig
cd ../..
```

### Hardcoded radioconda path

⚠️ The path
`/home/wcsng-901/radioconda/lib/python3.12/site-packages/gnuradio/gr/gr_python.cpython-312-x86_64-linux-gnu.so`
is hardcoded in `gr-custom_dpdk/python/custom_dpdk/bindings/CMakeLists.txt`.
If your radioconda lives elsewhere, uses a different Python version, or you
are on a different machine, edit that line accordingly.

## 5. Build the DPDK bridge

Independent of the GR build — just gcc + pkg-config:

```bash
cd dpdk_bridge
make
```

Produces `./dpdk_slot_transmission` (an ELF binary, gitignored).

## 6. Run order

The bridge connects to the GR uplink source's socket and then blocks on
`accept()` for the GR downlink sink's socket. So the GR flowgraph must be
running first.

1. Open your flowgraph in `gnuradio-companion` (use radioconda's, not system's,
   if that's what the OOT was linked against). Set the `NCO Freq (MHz)`,
   `Slot Schedule (20)` parameters in the block properties. **Run.**
2. In another terminal:

   ```bash
   sudo taskset -c 5 ./dpdk_bridge/dpdk_slot_transmission \
        --proc-type=auto -a 0000:b3:00.1
   ```

You should see `CONFIG: ...` lines confirming the NCO and 20-slot schedule
the bridge picked up from `/dev/shm/dpdk_*.conf`, followed by
`Producer: Connected!` / `Consumer: Producer connected!` and slot timing
prints.

## Config flow

The GR blocks write three files to `/dev/shm/` at construction (i.e., when
the flowgraph starts), which the bridge reads once in `main()` after
`rte_eal_init()`:

| File | Written by | Contents |
|---|---|---|
| `/dev/shm/dpdk_slot_schedule.conf` | `dpdk_downlink_sink` | 20 ints, each `0`/`1`/`2`/`3` (idle/UL/DL/both) |
| `/dev/shm/dpdk_nco_ul_mhz.conf` | `dpdk_uplink_source` | one `double`, MHz |
| `/dev/shm/dpdk_nco_dl_mhz.conf` | `dpdk_downlink_sink` | one `double`, MHz |

If a file is missing the bridge falls back to its compiled-in defaults
(useful for standalone tests without the GR flowgraph).

## Known issues

- The bridge's `log_dump()` writes `sizeof(uint64_t) * log_idx` bytes from a
  `uint16_t` buffer, so `total_dl_latency.bin` is 4× the intended size and
  contains stale data past the real samples. Pre-existing in the team's
  version; preserved here verbatim.
- See the hardcoded radioconda path note in §4.

## FPGA assumptions

This bridge expects a specific FPGA configuration (RFSoC 4x2, 4915.2 MHz
sample rate, the slot-tick / control-packet protocol described in the
`dpdk_slot_transmission.c` comments). The FPGA design (Vivado projects, BD,
HLS) is not part of this repo — it lives in a separate RadioStream tree.
