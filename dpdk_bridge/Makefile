# dpdk_bridge - build the DPDK slot-transmission app
#
# Requires libdpdk available via pkg-config:
#     pkg-config --modversion libdpdk
#
# Build:   make
# Run:     sudo ./dpdk_slot_transmission --proc-type=auto -a 0000:XX:XX.X
# Clean:   make clean

CC         ?= gcc
CFLAGS     ?= -O3 -Wall
PKG_CONFIG ?= pkg-config

DPDK_CFLAGS := $(shell $(PKG_CONFIG) --cflags libdpdk)
DPDK_LIBS   := $(shell $(PKG_CONFIG) --libs libdpdk)

TARGET := dpdk_slot_transmission
SRC    := dpdk_slot_transmission.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(DPDK_CFLAGS) -o $@ $< $(DPDK_LIBS)

clean:
	rm -f $(TARGET) total_dl_latency.bin

.PHONY: all clean
