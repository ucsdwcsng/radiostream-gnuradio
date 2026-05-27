/*
 * Optimized DPDK Slot sender implementation
 * Author: Jeeva Keshav S
 *
 * Description: This code receives a slot tick, extracts the SFN and slot number, and transmit a slot worth of packets.
 *
 * Key optimizations based on Pktgen-DPDK analysis:
 * 1. Bulk mbuf allocation and transmission
 * 2. Pre-built header templates
 * 3. Automatic mbuf recycling via DPDK framework
 * 4. Optimized ring thresholds
 * 5. Removed per-packet syscalls
 *
 * Compile: gcc dpdk_slot_transmission.c -o dpdk_slot_transmission $(pkg-config --cflags --libs libdpdk)
 * Run: sudo ./dpdk_slot_transmission -l 4-7 -n 4 --proc-type=auto -a 0000:b3:00.0
 *
 gcc -O3 dpdk_slot_transmission.c -o dpdk_slot_transmission $(pkg-config --cflags --libs libdpdk); sudo taskset -c 5 ./dpdk_slot_transmission --proc-type=auto -a 0000:b3:00.1
 */

#include <rte_common.h>
#include <rte_log.h>
#include <rte_malloc.h>
#include <rte_memory.h>
#include <rte_memcpy.h>
#include <rte_eal.h>
#include <rte_launch.h>
#include <rte_cycles.h>
#include <rte_prefetch.h>
#include <rte_lcore.h>
#include <rte_per_lcore.h>
#include <rte_branch_prediction.h>
#include <rte_interrupts.h>
#include <rte_random.h>
#include <rte_debug.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_mempool.h>
#include <rte_mbuf.h>
#include <rte_string_fns.h>
#include <math.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <arpa/inet.h>
#include <errno.h>
#include <string.h>
#include <signal.h>

/* Socket parameters*/
// #define SOCKET_PATH_DL "/tmp/dl_data.sock"
// #define SOCKET_PATH_UL "/tmp/ul_data.sock"
#define SOCKET_PATH_DL "/dev/shm/dl_data.sock"
#define SOCKET_PATH_UL "/dev/shm/ul_data.sock"

// AARON config: runtime config files written by the gnuradio custom_dpdk blocks.
// schedule + dl nco come from dpdk_downlink_sink; ul nco from dpdk_uplink_source.
// each is optional - if absent we keep the compiled-in defaults below.
#define SCHED_CONF   "/dev/shm/dpdk_slot_schedule.conf"
#define NCO_UL_CONF  "/dev/shm/dpdk_nco_ul_mhz.conf"
#define NCO_DL_CONF  "/dev/shm/dpdk_nco_dl_mhz.conf"
#define NCO_FS_MHZ   4915.2   /* rfsoc 4x2 sample rate (MHz) for mhz->48-bit word */

#define FRAME_SIZE (64 * 1024)  // 64KB frames for high throughput
#define BUFFER_SIZE (8 * 1024 * 1024)  // 8MB socket buffer

/* Configuration Constants */
#define LAYER_NUM 4
#define MIMO_NUM 0x04
#define BW_FACTOR 1
#define NUM_MBUFS           8192*2        /* Must be power of 2 */
#define MBUF_CACHE_SIZE     512         /* Increased for better performance */
#define TX_RING_SIZE        2048        /* Power of 2, optimized size */
#define RX_RING_SIZE     4096   /* Power-of-2 for cache alignment */
#define MAX_BURST_SIZE          64          /* Multiple of 8, cache-aligned */
#define PORT_ID             0
#define RX_DATA_LEN         8960
#define ETH_HDR_LEN         14
#define COUNTER_LEN         4
#define TOTAL_BYTES_PER_SLOT   BW_FACTOR*(61440*32/8)        /* 1 slot 1 channel packets for testing */
#define NUM_SLOTS_PER_SFN 20 /* Number of slots per SFN */
#define MAX_NUM_SFN 1024
#define NUM_BYTES_PER_SAMPLE 64
#define NUM_BYTES_HDR_RESERVED 64
#define TX_DATA_LEN         (RX_DATA_LEN-NUM_BYTES_HDR_RESERVED) /* 8960 bytes minus 64 bytes for header and metadata */
#define NUM_SAMPLES_PER_SLOT LAYER_NUM*BW_FACTOR*61440

#define UL_STORE_SIZE   (1 << 27)  /* 1-GiB circular buffer for captured UL sample payloads (adjust size) */


#define TOTAL_MTUs_PER_SLOT 30 //This is an upper cap - just for the sake of memory allocation

// cmac_data_tdata[127:112] is the mask of the type of payload
// cmac_data_tdata[127:112] = {DATA_FRAME_INDICATOR, CONTROL_FRAME_INDICATOR, 0, SLOT_TICK_INDICATOR, DL_DATA_INDICATOR, UL_DATA_INDICATOR}
#define UL_DATA_INDICATOR_MASK 0X0001
#define DL_DATA_INDICATOR_MASK 0X0002
#define UL_DL_DATA_INDICATOR_MASK 0X0003
#define SLOT_TICK_INDICATOR_MASK 0X0004

#define CONTROL_FRAME_INDICATOR_MASK 0X0010
#define DATA_FRAME_INDICATOR_MASK 0X0020

#define min(a, b) ((a) < (b) ? (a) : (b))

/* Pre-built header template for fast packet construction */
static uint8_t hdr_template[ETH_HDR_LEN] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,  /* Destination MAC (broadcast) */
    0x55, 0x44, 0x33, 0x22, 0x11, 0x00,  /* Source MAC */
    0xAE, 0xFE                            /* Ethernet type (0xAEFE) */
};

/* Global variables */
static struct rte_mempool *mbuf_pool;
static struct rte_mbuf *tx_pkts[MAX_BURST_SIZE];
static struct rte_mbuf *rx_pkts[MAX_BURST_SIZE];
static volatile int run = 1;

 /* ------------------------  Global Storage  ------------------------- */
static uint8_t ul_store[UL_STORE_SIZE] __rte_cache_aligned;
static uint32_t ul_store_head = 0;          /* write cursor */
static uint64_t ul_total_bytes = 0;         /* life-time counter */

static struct timespec start_time, end_time, start_time_ul, end_time_ul;
static struct timespec fopen_start_time, fopen_end_time;

// static uint16_t sch_slot_array[NUM_SLOTS_PER_SFN] = {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}; // UL/DL selection array for 20 slots; `
static uint16_t sch_slot_array[NUM_SLOTS_PER_SFN] = {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1}; // UL/DL selection array for 20 slots;
// static uint16_t sch_slot_array[NUM_SLOTS_PER_SFN] = {2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2}; // UL/DL selection array for 20 slots;
// static uint16_t sch_slot_array[NUM_SLOTS_PER_SFN] = {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1}; // UL/DL selection array for 20 slots;
// static uint16_t sch_slot_array[NUM_SLOTS_PER_SFN] = {3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3}; // UL/DL selection array for 20 slots;

static uint32_t counter = 0;
/*
0 - No allocation
1 - UL slot
2 - DL slot
3 - Both UL and DL slot
*/

/* Statistics */
struct tx_stats {
    uint64_t tx_packets;
    uint64_t tx_bytes;
    uint64_t tx_errors;
    uint64_t tx_dropped;
    uint64_t rx_ticks;            /* number of slot_tick packets seen   */
    uint64_t missed_ticks;        /* loops where no tick arrived in time */
    uint64_t missed_packets;
};
static struct tx_stats stats = {0};
struct rte_eth_stats istats;
static uint64_t initial_ierrors = 0;  /* Track errors at start */
static uint64_t prev_ierrors = 0;     /* Track errors from previous check */

int sock_dl, sock_ul;

volatile int running = 1;

uint8_t *dl_data_from_socket;   //malloc done in the main function
uint8_t *ul_data_to_socket;     //malloc done in the main function


int cont_ul_rx_init = 0;
static uint8_t init_state = 0;
static uint8_t ul_recv_started = 0;

// 48-bit NCO tuning words
static const uint64_t nco_freq_list[] = {
    0x053555555020, // 100 MHz
    0x0A6AAAAA5220, // 200 MHz
    0x0FA000003E80, // 300 MHz
    0x14D55554A440, // 400 MHz
    0x1A0AAAAAB7B0, // 500 MHz
    0x1F40000007D0, // 600 MHz
    0x2475555557F0, // 700 MHz
};

static uint32_t freq_idx = 0;
static uint32_t pkt_cnt = 0;

#define NUM_FREQS (sizeof(nco_freq_list)/sizeof(nco_freq_list[0]))

// AARON config: active 48-bit nco tuning words. defaults = the values that were
// hardcoded in parse_slot_tick: UL = 0x2FAAAAA9F460 (-4 GHz), DL = 0x1A0AAAAAB7B0
// (500 MHz). load_runtime_config() overwrites these if the gnuradio blocks wrote
// an nco frequency (MHz) to /dev/shm; if no config file is present these defaults
// keep behavior bit-identical to the team's baseline.
static uint64_t ul_nco_word = 0x2FAAAAA9F460ULL & 0xFFFFFFFFFFFFULL;
static uint64_t dl_nco_word = 0x1A0AAAAAB7B0ULL & 0xFFFFFFFFFFFFULL;

#define MAX_LOGS (20ULL * 1000 * 1000)   // 10M samples = 80 MB

static uint16_t *logbuf = NULL;
static uint16_t log_idx = 0;

static void log_init(void)
{
    // 64B aligned is nice for cache; plain malloc is also fine
    logbuf = aligned_alloc(64, MAX_LOGS * sizeof(uint16_t));
    if (!logbuf) {
        perror("aligned_alloc logbuf");
        exit(1);
    }
}

static inline void log_push(uint16_t v)
{
    if (log_idx < MAX_LOGS) {
        logbuf[log_idx++] = v;
    }
    // else: drop silently (or you can stop the run / wrap around)
}

static void log_dump(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror("fopen log file");
        return;
    }

    size_t n = fwrite(logbuf, sizeof(uint64_t), (size_t)log_idx, f);
    if (n != log_idx) {
        fprintf(stderr, "fwrite: wrote %zu of %llu (errno=%d)\n",
                n, (unsigned long long)log_idx, errno);
    }
    fclose(f);

    printf("Dumped %llu samples to %s\n",
           (unsigned long long)log_idx, path);
}

static void log_free(void)
{
    free(logbuf);
    logbuf = NULL;
}

// AARON config: convert an nco frequency in MHz to the 48-bit tuning word.
// fs = 4915.2 MHz (rfsoc 4x2). negative frequencies wrap mod 2^48, matching the
// hardcoded literals (e.g. -4000 MHz aliases into band, same as 0x2FAAAAA9F460).
static uint64_t mhz_to_word(double f_mhz)
{
    long long w = llround((f_mhz / NCO_FS_MHZ) * (double)(1ULL << 48));
    return (uint64_t)w & 0xFFFFFFFFFFFFULL;
}

// AARON config: load runtime config written by the gnuradio custom_dpdk blocks
// into /dev/shm. each file is optional - if absent the compiled-in default is
// kept (so standalone runs behave exactly as before). called once at startup;
// not in any hot path. invalid (present but malformed) config is fatal.
static void load_runtime_config(void)
{
    // 20-slot ul/dl schedule (written by dpdk_downlink_sink). overrides the
    // compiled-in sch_slot_array.
    FILE *f = fopen(SCHED_CONF, "r");
    if (f) {
        uint16_t tmp[NUM_SLOTS_PER_SFN];
        int n = 0;
        for (; n < NUM_SLOTS_PER_SFN; n++) {
            int v;
            if (fscanf(f, "%d", &v) != 1) break;
            if (v < 0 || v > 3) {
                fclose(f);
                rte_exit(EXIT_FAILURE,
                         "%s: slot %d = %d out of range (must be 0,1,2,3)\n",
                         SCHED_CONF, n, v);
            }
            tmp[n] = (uint16_t)v;
        }
        fclose(f);
        if (n != NUM_SLOTS_PER_SFN) {
            rte_exit(EXIT_FAILURE, "%s: expected %d slot values, got %d\n",
                     SCHED_CONF, NUM_SLOTS_PER_SFN, n);
        }
        memcpy(sch_slot_array, tmp, sizeof(sch_slot_array));
        printf("CONFIG: loaded 20-slot schedule from %s\n", SCHED_CONF);
    } else {
        printf("CONFIG: %s not found, using compiled-in schedule\n", SCHED_CONF);
    }

    // ul nco (written by dpdk_uplink_source)
    f = fopen(NCO_UL_CONF, "r");
    if (f) {
        double mhz;
        if (fscanf(f, "%lf", &mhz) == 1) {
            ul_nco_word = mhz_to_word(mhz);
            printf("CONFIG: ul nco = %.6g MHz -> 0x%012llX (from %s)\n",
                   mhz, (unsigned long long)ul_nco_word, NCO_UL_CONF);
        }
        fclose(f);
    } else {
        printf("CONFIG: %s not found, using compiled-in ul nco 0x%012llX\n",
               NCO_UL_CONF, (unsigned long long)ul_nco_word);
    }

    // dl nco (written by dpdk_downlink_sink)
    f = fopen(NCO_DL_CONF, "r");
    if (f) {
        double mhz;
        if (fscanf(f, "%lf", &mhz) == 1) {
            dl_nco_word = mhz_to_word(mhz);
            printf("CONFIG: dl nco = %.6g MHz -> 0x%012llX (from %s)\n",
                   mhz, (unsigned long long)dl_nco_word, NCO_DL_CONF);
        }
        fclose(f);
    } else {
        printf("CONFIG: %s not found, using compiled-in dl nco 0x%012llX\n",
               NCO_DL_CONF, (unsigned long long)dl_nco_word);
    }
}


void signal_handler(int sig) {
    running = 0;
    printf("\n\nExiting safely!");

    free(dl_data_from_socket);
    free(ul_data_to_socket);
}

void setup_socket_buffers(int sock) {
    int bufsize = BUFFER_SIZE;
    if (setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize)) < 0) {
        perror("setsockopt SO_SNDBUF");
    }
    if (setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize)) < 0) {
        perror("setsockopt SO_RCVBUF");
    }
}

// Initialize DPDK port with optimized configuration
static int port_init(void)
{
    struct rte_eth_conf port_conf = {
        .rxmode = {
            .mq_mode = RTE_ETH_MQ_RX_NONE,
        },
        .txmode = {
            .mq_mode = RTE_ETH_MQ_TX_NONE,
            .offloads = RTE_ETH_TX_OFFLOAD_MULTI_SEGS,
        },
    };

    /* ============= OPTIMIZED TX CONFIGURATION ============= */
    struct rte_eth_txconf tx_conf = {
        .tx_thresh = {
            .pthresh = 0,   /*descriptor prefetch based on num of available mbufs */
            .hthresh = 0,   /* Host threshold */
            .wthresh = 0,   /* descriptor recycling */
        },
        .tx_free_thresh = 8,   /* Refill mbuf every 32 packets (was MAX_BURST_SIZE=64) */
        .tx_rs_thresh = 8,     /* Report status every 32 (was MAX_BURST_SIZE=64) */
        .offloads = 0,
    };

    /* ============= OPTIMIZED RX CONFIGURATION FOR 100 Gbps ============= */
    struct rte_eth_rxconf rx_conf = {
        .rx_thresh = {
            .pthresh = 8,  /* Prefetch when 8 descriptors available */
            .hthresh = 8,  /* Host threshold - fetch descriptors earlier */
            .wthresh = 4,  /* Writeback threshold */
        },
        .rx_free_thresh = 64,  /* Refill in larger batches (was 8) - CRITICAL for 100Gbps */
        .rx_drop_en = 0,
        .offloads = 0,
    };

    int ret;

    /* Configure the device */
    ret = rte_eth_dev_configure(PORT_ID, 1 /*rxq*/, 1 /*txq*/, &port_conf);
    if (ret < 0) {
        printf("Error: Device configuration failed\n");
        return ret;
    }

    /* Setup TX queue with optimized parameters */
    ret = rte_eth_tx_queue_setup(PORT_ID, 0, TX_RING_SIZE,
                                 rte_socket_id(), &tx_conf);
    if (ret < 0) {
        printf("Error: TX queue setup failed\n");
        return ret;
    }

    /* Setup RX queue with optimized parameters */
    ret = rte_eth_rx_queue_setup(PORT_ID, 0, RX_RING_SIZE,
                                 rte_socket_id(), &rx_conf, mbuf_pool);
    if (ret < 0) {
        printf("Error: RX queue setup failed\n");
        return ret;
    }

    /* Start the device */
    ret = rte_eth_dev_start(PORT_ID);
    if (ret < 0) {
        printf("Error: Device start failed\n");
        return ret;
    }

    /* Stop before MTU change */
    printf("Stopping device for MTU configuration...\n");
    rte_eth_dev_stop(PORT_ID);

    /* Set MTU to 9000 (jumbo frames) */
    ret = rte_eth_dev_set_mtu(PORT_ID, 9000);
    if (ret < 0) {
        printf("Warning: Failed to set MTU to 9000: %s (err=%d)\n",
               rte_strerror(-ret), ret);
    } else {
        printf("MTU set to 9000 (jumbo frames enabled)\n");
    }

    /* Restart device */
    ret = rte_eth_dev_start(PORT_ID);
    if (ret < 0) {
        printf("ERROR: Restart failed after MTU change\n");
        return ret;
    }

    printf("\n********* Device restarted after setting MTU to 9000 ************\n");

    /* Set promiscuous mode */
    ret = rte_eth_promiscuous_enable(PORT_ID);
    if (ret < 0) {
        printf("Warning: Promiscuous mode enable failed\n");
    }

    /* Print configuration summary */
    printf("\n=== Port Configuration Summary ===\n");
    printf("Port %d initialized successfully\n", PORT_ID);
    printf("TX Ring Size: %d\n", TX_RING_SIZE);
    printf("RX Ring Size: %d\n", RX_RING_SIZE);
    printf("TX free_thresh: %d (aggressive)\n", tx_conf.tx_free_thresh);
    printf("RX free_thresh: %d (aggressive)\n", rx_conf.rx_free_thresh);
    printf("TX wthresh: %d (aggressive writeback)\n", tx_conf.tx_thresh.wthresh);
    printf("RX wthresh: %d (aggressive writeback)\n", rx_conf.rx_thresh.wthresh);

    return 0;
}

static inline uint64_t time_diff_ns(struct timespec *start, struct timespec *end) {
    uint64_t sec_diff = end->tv_sec - start->tv_sec;
    int64_t nsec_diff = end->tv_nsec - start->tv_nsec;

    return (sec_diff * 1000000000ULL) + nsec_diff;
}

int send_frame(int sock, const void *data, uint32_t len) {
    uint32_t net_len = htonl(len);

    // Send length header
    ssize_t sent = send(sock, &net_len, sizeof(net_len), MSG_NOSIGNAL);
    if (sent != sizeof(net_len)) {
        if (errno != EPIPE) perror("send header");
        return -1;
    }

    // Send data
    sent = send(sock, data, len, MSG_NOSIGNAL);
    if (sent != len) {
        if (errno != EPIPE) perror("send data");
        return -1;
    }

    // // Wait for ACK
    // uint32_t ack;
    // ssize_t received = recv(sock, &ack, sizeof(ack), 0);
    // if (received != sizeof(ack)) {
    //     if (errno != ECONNRESET) perror("recv ack");
    //     return -1;
    // }

    // ack = ntohl(ack);
    // if (ack != len) {
    //     fprintf(stderr, "ACK mismatch: expected %u, got %u\n", len, ack);
    //     return -1;
    // }

    return 0;
}

int recv_frame_fast(uint8_t* data, int sock) {

    uint32_t net_len;

    // Single syscall for length
    if (recv(sock, &net_len, sizeof(net_len), MSG_WAITALL) != sizeof(net_len))
    {
        printf("Consumer: Producer disconnected or error occurred while receiving length header\n");
        return -1;
    }

    uint32_t len = ntohl(net_len);
    if (len > 10 * 1024 * 1024){
        return -1;  // Sanity check
        fprintf(stderr, "Frame too large: %u bytes\n", len);
    }

    // Single syscall for ALL data ← KEY OPTIMIZATION
    if (recv(sock, data, len, MSG_WAITALL) != len)
        return -1;

    return len;

}


// Global pre-allocated mbuf pool for DL
static struct rte_mbuf *dl_mbufs[200];  // Pre-allocated for reuse
static bool dl_mbufs_initialized = false;

static inline int
prepare_flush_dl_burst(uint16_t sfn_no, uint16_t slot_no, int sock)
{
    struct timespec socket_start, socket_end, tx_start, tx_end, tx_start_tmp, tx_end_tmp;
    uint64_t socket_time_ns, tx_time_ns, rte_memcpy_time_ns[50] = {0};
    uint64_t offset = 0;
    uint16_t total_dl_latency;

    // ========== Receive data from socket ==========
    clock_gettime(CLOCK_MONOTONIC, &socket_start);
    int dl_data_len = recv_frame_fast(dl_data_from_socket, sock);
    clock_gettime(CLOCK_MONOTONIC, &socket_end);

    clock_gettime(CLOCK_MONOTONIC, &tx_start);

    socket_time_ns = time_diff_ns(&socket_start, &socket_end);

    // Error checking
    if (dl_data_len < 0) {
        perror("recv_frame_fast failed");
        return -1;
    }

    if (dl_data_len != LAYER_NUM * TOTAL_BYTES_PER_SLOT) {
        printf("Warning: Received DL data length %d does not match expected %d\n",
               dl_data_len, LAYER_NUM * TOTAL_BYTES_PER_SLOT);
    }

    // ========== First time: allocate all mbufs ONCE ==========
    if (!dl_mbufs_initialized) {
        if (rte_pktmbuf_alloc_bulk(mbuf_pool, dl_mbufs, 200) != 0) {
            stats.tx_errors++;
            return -ENOMEM;
        }
        dl_mbufs_initialized = true;
        printf("Pre-allocated %d mbufs for DL transmission\n", 200);
    }

    // ========== Calculate packets needed ==========
    uint32_t packets_needed = (dl_data_len + TX_DATA_LEN - 1) / TX_DATA_LEN;

    // ========== Prepare packets (reuse existing mbufs) ==========

    for (uint32_t i = 0; i < packets_needed; i++) {
        struct rte_mbuf *m = dl_mbufs[i];
        uint8_t *pkt_data = rte_pktmbuf_mtod(m, uint8_t *);

        uint32_t bytes_this_pkt = min(TX_DATA_LEN, dl_data_len - offset);

        // Copy header template (14 bytes)
        rte_memcpy(pkt_data, hdr_template, ETH_HDR_LEN);

        // Set header fields
        *(uint16_t *)(pkt_data + ETH_HDR_LEN) =
            rte_cpu_to_le_16(DATA_FRAME_INDICATOR_MASK | DL_DATA_INDICATOR_MASK);
        *(uint16_t *)(pkt_data + ETH_HDR_LEN + 4) =
            sfn_no + ((slot_no + 2) / NUM_SLOTS_PER_SFN);
        *(uint16_t *)(pkt_data + ETH_HDR_LEN + 2) =
            (slot_no + 2) % NUM_SLOTS_PER_SFN;
        *(uint32_t *)(pkt_data + ETH_HDR_LEN + 6) =
            rte_cpu_to_le_32(i);

        // clock_gettime(CLOCK_MONOTONIC, &tx_start_tmp);

        // Copy payload data
        rte_memcpy(pkt_data + NUM_BYTES_HDR_RESERVED,
                   dl_data_from_socket + offset,
                   bytes_this_pkt);

        // clock_gettime(CLOCK_MONOTONIC, &tx_end_tmp);

        // rte_memcpy_time_ns[i] = time_diff_ns(&tx_start_tmp, &tx_end_tmp);

        // Set mbuf metadata (CRITICAL: must update each time!)
        m->data_len = bytes_this_pkt + NUM_BYTES_HDR_RESERVED;
        m->pkt_len = bytes_this_pkt + NUM_BYTES_HDR_RESERVED;
        m->l2_len = ETH_HDR_LEN;
        m->nb_segs = 1;
        m->next = NULL;
        m->ol_flags = 0;

        offset += bytes_this_pkt;
    }

    // ========== Transmit all packets ==========
    uint32_t total_sent = 0;
    while (total_sent < packets_needed) {
        uint16_t to_send = min(MAX_BURST_SIZE, packets_needed - total_sent);
        uint16_t n = rte_eth_tx_burst(PORT_ID, 0, &dl_mbufs[total_sent], to_send);

        total_sent += n;

        // Update statistics
        stats.tx_packets += n;
        stats.tx_bytes += n * (TX_DATA_LEN + NUM_BYTES_HDR_RESERVED);

        // If can't send all, pause briefly and retry
        if (unlikely(n < to_send)) {
            rte_pause();
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &tx_end);
    tx_time_ns = time_diff_ns(&tx_start, &tx_end);

    // ========== Print performance statistics ==========
    // printf("DL: Producer->DPDK: %lu us\n", socket_time_ns / 1000);
    // printf("DL: DPDK->FPGA: %u bytes in %u packets - %lu us\n",
    // dl_data_len, packets_needed, tx_time_ns / 1000);

    printf("DL: Producer->DPDK: %lu \n", (socket_time_ns+tx_time_ns) / 1000);
    total_dl_latency= (socket_time_ns+tx_time_ns) / 1000;
    log_push(total_dl_latency);

    // for (uint32_t i = 0; i < packets_needed; i++) {
    //     printf(" %lu ns ",rte_memcpy_time_ns[i]);
    // }
    // printf("\n");

    // DON'T free mbufs - we reuse them next slot!
    // The NIC will automatically release them after transmission
    // They become available again for the next call to this function

    return 0;
}

static uint32_t ul_control_tick_counter = 0;
static const uint32_t UL_CONTROL_PERIOD = 10000;  // Send every 1000 ticks (skip 999, send 1)



/* Function to parse slot_tick */
static inline int
parse_slot_tick(uint8_t *rx_pkt_data, uint16_t data_len, int sock) //sock_dl
{

    struct rte_mbuf *m;
    uint8_t *rte_pkt_data ;
    uint32_t tick_id = rte_le_to_cpu_32(*(const uint32_t *)(rx_pkt_data + 2));

    uint16_t sfn_no = (tick_id & 0xFFFF0000) >> 16;
    uint16_t slot_no = tick_id & 0x0000FFFF;

    uint16_t fpga_counter = rte_le_to_cpu_16(*(const uint16_t *)(rx_pkt_data + 6));

    static uint64_t dl_slot_count = 1;
    static uint64_t dl_slot_dropped_count = 1;


    uint64_t elapsed_ns;

    if((sfn_no == 0) && (slot_no == 0) && (init_state == 1))
    {
        printf("********** Slot 0, SFN 0 reached; UL and DL initialized ********** \n");
        init_state = 2;
    }

    if(init_state < 2)
    {
        // printf("%d, Sfn: %u, Slot: %u\n", init_state, sfn_no, slot_no);
        return 0;
    }
    ul_control_tick_counter++;
    // printf("Received slot_tick: SFN %u, Slot %u with l = %u, pkt %d\n", sfn_no, slot_no,data_len, fpga_counter);
    if(sch_slot_array[(slot_no + 2)% NUM_SLOTS_PER_SFN] & UL_DATA_INDICATOR_MASK & (ul_control_tick_counter >= UL_CONTROL_PERIOD))
    // if(sch_slot_array[(slot_no + 2)% NUM_SLOTS_PER_SFN] & UL_DATA_INDICATOR_MASK)
    {
        ul_control_tick_counter=0;
        /* Check ierrors before transmitting UL control */
        struct rte_eth_stats current_stats;
        rte_eth_stats_get(PORT_ID, &current_stats);
        uint64_t ierrors_delta = current_stats.ierrors - prev_ierrors;

        if (ierrors_delta > 0) {
            printf("⚠️  ERRORS DETECTED! +%lu errors (total: %lu)\n",
                   ierrors_delta, current_stats.ierrors);
        }

        // printf("Transmitting UL control info for Slot %u in SFN %u, Slot %u\n", (slot_no + 2), sfn_no, slot_no);
        printf("Transmitting UL control info for Slot %u in SFN %u, Slot %u [ierrors: %lu, Δ%lu]\n",
               (slot_no + 2)% NUM_SLOTS_PER_SFN, sfn_no, slot_no, current_stats.ierrors, ierrors_delta);

        prev_ierrors = current_stats.ierrors;
        ul_recv_started = 1;

        /* Bulk allocation - single mempool operation for entire burst */
        if (unlikely(rte_pktmbuf_alloc_bulk(mbuf_pool, tx_pkts, 1) != 0)) {     //Mem alloc for burst size 1 (control packet)
            stats.tx_errors++;
            return -ENOMEM;
        }

        m = tx_pkts[0];
        rte_pkt_data = rte_pktmbuf_mtod(m, uint8_t *);
        rte_memcpy(rte_pkt_data, hdr_template, ETH_HDR_LEN);
        *(uint16_t *)(rte_pkt_data + ETH_HDR_LEN) = rte_cpu_to_le_16(CONTROL_FRAME_INDICATOR_MASK | UL_DATA_INDICATOR_MASK); // UL_DATA_INDICATOR_MASK
        *(uint16_t *)(rte_pkt_data + ETH_HDR_LEN + 4) = sfn_no + ((slot_no+2)/NUM_SLOTS_PER_SFN); // SFN number
        *(uint16_t *)(rte_pkt_data + ETH_HDR_LEN + 2) = (slot_no + 2)% NUM_SLOTS_PER_SFN; // slot number
        *(uint8_t *)(rte_pkt_data + ETH_HDR_LEN + 6) = MIMO_NUM; // MIMO number

        uint64_t freq_word = nco_freq_list[freq_idx] & 0xFFFFFFFFFFFF;

        // Write to packet
        // *(uint64_t *)(rte_pkt_data + ETH_HDR_LEN + 7) = 0x0A6AAAAAA040 & 0xFFFFFFFFFFFF; // 200MHz

        //*(uint64_t *)(rte_pkt_data + ETH_HDR_LEN + 7) = 0xF59555555FC0 & 0xFFFFFFFFFFFF; // -200MHz
        // *(uint64_t *)(rte_pkt_data + ETH_HDR_LEN + 7) = 0x341555554850 & 0xFFFFFFFFFFFF; // 1GHz
        // *(uint64_t *)(rte_pkt_data + ETH_HDR_LEN + 7) = 0xCBEAAAAAB7B0 & 0xFFFFFFFFFFFF; // -1GHz
        *(uint64_t *)(rte_pkt_data + ETH_HDR_LEN + 7) = ul_nco_word; // AARON config: ul nco (set via /dev/shm/dpdk_nco_ul_mhz.conf; was 0x2FAAAAA9F460 = -4GHz)
        // *(uint64_t *)(rte_pkt_data + ETH_HDR_LEN + 7) =  0x2D0FFFFFC180 & 0xFFFFFFFFFFFF; // -4.05GHz

        //*(uint64_t *)(rte_pkt_data + ETH_HDR_LEN + 7) = 0x2FAAAAA9F460 & 0xFFFFFFFFFFFF; // 4GHz


        // *(uint64_t *)(rte_pkt_data + ETH_HDR_LEN + 7) = freq_word;
        pkt_cnt++;
        // Move to next frequency
        if(pkt_cnt==1){
            freq_idx = (freq_idx + 1) % NUM_FREQS;
            pkt_cnt=0;
        }

        //DDR Controller header - UL

        if(sch_slot_array[(slot_no + 2)% NUM_SLOTS_PER_SFN] & DL_DATA_INDICATOR_MASK)       //since both UL and DL are scheduled, the DDR config is sent along with DL control
        {
            *(uint32_t *)(rte_pkt_data + 32) = 0; // UL and DL data indicator
            *(uint32_t *)(rte_pkt_data + 32 + 4) = 0; // DL - No. of samples per slot for a bus width of 512 bits (i.e., 16 samples per 512-bit word)
            *(uint32_t *)(rte_pkt_data + 32 + 8) = 0; // UL - No. of samples per slot for a bus width of 512 bits (i.e., 16 samples per 512-bit word)

        }
        else
        {
            *(uint32_t *)(rte_pkt_data + 32) = UL_DATA_INDICATOR_MASK; // UL data indicator
            *(uint32_t *)(rte_pkt_data + 32 + 4) = 0; // DL - No. of samples per slot for a bus width of 512 bits (i.e., 16 samples per 512-bit word)
            *(uint32_t *)(rte_pkt_data + 32 + 8) = NUM_SAMPLES_PER_SLOT/16; // UL - No. of samples per slot for a bus width of 512 bits (i.e., 16 samples per 512-bit word)
        }

        // Zero pad the unallocated part of the packet of the 64 bytes header
        // memset(rte_pkt_data + ETH_HDR_LEN + 7, 0, NUM_BYTES_PER_SAMPLE/2 - (ETH_HDR_LEN + 7));
        // memset(rte_pkt_data + 32, 0, NUM_BYTES_PER_SAMPLE/2 - (12));

        m->data_len = NUM_BYTES_PER_SAMPLE;
        m->pkt_len = NUM_BYTES_PER_SAMPLE;
        m->l2_len = ETH_HDR_LEN;
        m->ol_flags = 0;

        // Print the packet contents before sending
        // printf("TX UL control packet (len=%u):\n", m->data_len);
        // for (uint32_t i = 0; i < m->data_len; i++) {
        //     printf("%02x ", rte_pkt_data[i]);
        //     if ((i + 1) % 16 == 0) printf("\n");
        // }
        // printf("\n");

        uint16_t sent = rte_eth_tx_burst(PORT_ID, 0, tx_pkts, 1);   // Transmit the control packet
        stats.tx_packets += sent;
        stats.tx_bytes += sent * (ETH_HDR_LEN + 2 + 4);

        /* Free any unsent packets immediately - don't retry */
        if (unlikely(sent < 1)) {
            printf("⚠️  WARNING: UL control packet TX FAILED! (slot %u)\n", slot_no);
            stats.tx_dropped += (1 - sent);
            rte_pktmbuf_free_bulk(&tx_pkts[sent], 1 - sent);
        }
    }

    if(sch_slot_array[(slot_no + 2)% NUM_SLOTS_PER_SFN] & DL_DATA_INDICATOR_MASK)
    {
        // printf("Transmitting DL control info for Slot %u in SFN %u, Slot %u\n", (slot_no + 2)% NUM_SLOTS_PER_SFN, sfn_no, slot_no);

        /* Bulk allocation - single mempool operation for entire burst */
        if (unlikely(rte_pktmbuf_alloc_bulk(mbuf_pool, tx_pkts, 1) != 0)) {     //Mem alloc for burst size 1 (control packet)
            stats.tx_errors++;
            return -ENOMEM;
        }

        m = tx_pkts[0];
        rte_pkt_data = rte_pktmbuf_mtod(m, uint8_t *);
        rte_memcpy(rte_pkt_data, hdr_template, ETH_HDR_LEN);
        *(uint16_t *)(rte_pkt_data + ETH_HDR_LEN) = rte_cpu_to_le_16(CONTROL_FRAME_INDICATOR_MASK | DL_DATA_INDICATOR_MASK); // UL_DATA_INDICATOR_MASK
        // *(uint16_t *)(rte_pkt_data + ETH_HDR_LEN + 4) = sfn_no + ((slot_no+2)/NUM_SLOTS_PER_SFN); // SFN number
        // printf("stupidity is:%u \n",sfn_no + ((slot_no+2)/NUM_SLOTS_PER_SFN));
        *(uint16_t *)(rte_pkt_data + ETH_HDR_LEN + 4) = 0xffff; // SFN number

        *(uint16_t *)(rte_pkt_data + ETH_HDR_LEN + 2) = (slot_no + 2)% NUM_SLOTS_PER_SFN; // slot number
        *(uint8_t *)(rte_pkt_data + ETH_HDR_LEN + 6) = MIMO_NUM; // MIMO number
        *(uint64_t *)(rte_pkt_data + ETH_HDR_LEN + 7) = dl_nco_word; // AARON config: dl nco (set via /dev/shm/dpdk_nco_dl_mhz.conf; was 0x1A0AAAAAB7B0 = 500MHz)
        // *(uint64_t *)(rte_pkt_data + ETH_HDR_LEN + 7) = 0xA6AAAAAA040 & 0xFFFFFFFFFFFF; // NCO frequency (48 bits) - 200MHz
        //*(uint64_t *)(rte_pkt_data + ETH_HDR_LEN + 7) = 0xD055554E6A80 & 0xFFFFFFFFFFFF; // NCO frequency (48 bits) - 200MHz
        // *(uint64_t *)(rte_pkt_data + ETH_HDR_LEN + 7) = 0x2FAAAAA9F460 & 0xFFFFFFFFFFFF; // 4GHz

        uint64_t freq_word = nco_freq_list[freq_idx] & 0xFFFFFFFFFFFF;

        // Write to packet
        // *(uint64_t *)(rte_pkt_data + ETH_HDR_LEN + 7) = freq_word;
        pkt_cnt++;
        // Move to next frequency
        if(pkt_cnt==10){
            freq_idx = (freq_idx + 1) % NUM_FREQS;
            pkt_cnt=0;
        }

        //DDR Controller header - DL

        if(sch_slot_array[(slot_no + 2)% NUM_SLOTS_PER_SFN] & UL_DATA_INDICATOR_MASK)
        {
            *(uint32_t *)(rte_pkt_data + 32) = UL_DL_DATA_INDICATOR_MASK; // UL and DL data indicator
            *(uint32_t *)(rte_pkt_data + 32 + 8) = NUM_SAMPLES_PER_SLOT/16; // UL - No. of samples per slot for a bus width of 512 bits (i.e., 16 samples per 512-bit word)
        }
        else
        {
            *(uint32_t *)(rte_pkt_data + 32) = DL_DATA_INDICATOR_MASK; // UL data indicator
            *(uint32_t *)(rte_pkt_data + 32 + 8) = 0; // UL - No. of samples per slot for a bus width of 512 bits (i.e., 16 samples per 512-bit word)
        }

        *(uint32_t *)(rte_pkt_data + 32 + 4) = NUM_SAMPLES_PER_SLOT/16; // DL - No. of samples per slot for a bus width of 512 bits (i.e., 16 samples per 512-bit word)

        // *(uint16_t *)(rte_pkt_data + ETH_HDR_LEN + 13) = 0x000000; // NCO phase
        // *(uint64_t *)(rte_pkt_data + ETH_HDR_LEN + 7) = 0x0A6AAAAAA040 & 0xFFFFFFFFFFFF; // NCO frequency (48 bits) - 200MHz


        m->data_len = NUM_BYTES_PER_SAMPLE;
        m->pkt_len = NUM_BYTES_PER_SAMPLE;
        m->l2_len = ETH_HDR_LEN;
        m->ol_flags = 0;

        // Print the packet contents before sending
        // printf("TX UL control packet (len=%u):\n", m->data_len);
        // for (uint32_t i = 0; i < m->data_len; i++) {
        //     printf("%02x ", rte_pkt_data[i]);
        //     if ((i + 1) % 16 == 0) printf("\n");
        // }
        // printf("\n");

        uint16_t sent = rte_eth_tx_burst(PORT_ID, 0, tx_pkts, 1);   // Transmit the control packet
        stats.tx_packets += sent;
        stats.tx_bytes += sent * (ETH_HDR_LEN + 2 + 4);

        /* Free any unsent packets immediately - don't retry */
        if (unlikely(sent < 1)) {
            printf("⚠️  WARNING: DL control packet TX FAILED! (slot %u)\n", slot_no);
            stats.tx_dropped += (1 - sent);
            rte_pktmbuf_free_bulk(&tx_pkts[sent], 1 - sent);
        }

        // clock_gettime(CLOCK_MONOTONIC, &end_time);
        // elapsed_ns = time_diff_ns(&start_time, &end_time);
        // if(elapsed_ns > 10000)
        // {
        //     printf("Dropped %lu of %lu DL slots; delay_ctrl = %lu us\n", dl_slot_dropped_count++, dl_slot_count++, elapsed_ns/1000);
        // }
        // else{
        clock_gettime(CLOCK_MONOTONIC, &start_time);
        prepare_flush_dl_burst(sfn_no, slot_no, sock); //sock_dl
        dl_slot_count++;
        clock_gettime(CLOCK_MONOTONIC, &end_time);
        elapsed_ns = time_diff_ns(&start_time, &end_time);

        // if(elapsed_ns > 50000)
        // {
        //     printf("DL slot %lu transmitted in %lu us\n", dl_slot_count-1, elapsed_ns/1000);
        // }
        printf("DL slot %lu transmitted in %lu us\n", dl_slot_count-1, elapsed_ns/1000);
    }

    return 1;
}


/**
 * Main transmission loop
 * Implements the optimized transmission pattern from Pktgen-DPDK
 */

static void
slot_transmission_reception_loop(int sock_dl, int sock_ul)
{
    uint16_t nb_rx = 0;
    static uint8_t prev_init_state = 0;

    // ========== UL State Management (persistent across calls) ==========
    static int ul_data_pdu_count = 0;
    static int ul_data_to_socket_offset = 0;
    static struct timespec ul_slot_start, ul_recv_end, ul_socket_start, ul_socket_end;
    static bool ul_slot_active = false;
    static uint32_t ul_slot_bytes_received = 0;
    static uint32_t ul_slot_pdus_received = 0;

    // Accumulated timing for UL (measured during UL reception, printed at slot_tick)
    static uint64_t ul_recv_time_ns = 0;
    static uint64_t ul_socket_time_ns = 0;
    static bool ul_timing_ready = false;  // Flag: timing ready to print

    /* ----------------- Receive burst of packets ----------------*/
    while(nb_rx == 0){
        nb_rx = rte_eth_rx_burst(PORT_ID, 0, rx_pkts, MAX_BURST_SIZE);

        // // ========== PRINT UL TIMING OF THE PREVIOUS SLOT ==========
        // if (ul_timing_ready) {
        //     uint64_t total_ul_time = ul_recv_time_ns + ul_socket_time_ns;

        //     // Only print if total time > 5 µs (to avoid clutter)
        //     if (total_ul_time > 5000) {
        //         printf("UL: FPGA->DPDK: %lu µs, DPDK->Consumer: %lu µs, Total: %lu µs (%u B, %u pkts)\n",
        //                 ul_recv_time_ns / 1000,
        //                 ul_socket_time_ns / 1000,
        //                 total_ul_time / 1000,
        //                 ul_slot_bytes_received,
        //                 ul_slot_pdus_received);
        //     }

        //     // Reset timing flag
        //     ul_timing_ready = false;
        // }
        // else {
        //     rte_pause();
        // }
    }

    // Clear all the residue data in the RING buffer after initialization to avoid processing stale packets
    if((init_state == 0) && (nb_rx == MAX_BURST_SIZE))
        return;

    // First-time initialization
    if(prev_init_state == 0) {
        init_state = 1;         // To avoid the above clearing buffers after initialization
        prev_init_state = init_state;
        printf("Setup Initialized. Cleared RING buffers\n");

        rte_eth_stats_get(PORT_ID, &istats);
        printf("\n=== Initial RX Statistics (after buffer clear) ===\n");
        printf("ipackets: %lu\n", istats.ipackets);
        printf("ibytes: %lu\n", istats.ibytes);
        printf("imissed: %lu\n", istats.imissed);
        printf("ierrors: %lu  (baseline - likely from init)\n", istats.ierrors);
        printf("rx_nombuf: %lu\n", istats.rx_nombuf);

        /* Save initial error count */
        initial_ierrors = istats.ierrors;
        prev_ierrors = istats.ierrors;  /* Initialize for delta tracking */
    }

    /* ----------------- Process all packets in burst ----------------*/
    for (uint16_t pkt_idx = 0; pkt_idx < nb_rx; pkt_idx++) {
        struct rte_mbuf *m = rx_pkts[pkt_idx];
        uint8_t *rx_pkt_data = rte_pktmbuf_mtod(m, uint8_t *);

        // ========== SLOT TICK PACKET ==========
        if (rx_pkt_data[ETH_HDR_LEN] & SLOT_TICK_INDICATOR_MASK) {

            parse_slot_tick(rx_pkt_data + ETH_HDR_LEN, m->data_len , sock_dl);

            // printf("Received slot_tick: SFN %u, Slot %u\n",
            //        rte_le_to_cpu_32(*(const uint32_t *)(rx_pkt_data + ETH_HDR_LEN + 4)),
            //        rte_le_to_cpu_32(*(const uint32_t *)(rx_pkt_data + ETH_HDR_LEN + 2)));

            // // If UL slot was interrupted (incomplete), finalize it
            // if (ul_slot_active) {
            //     clock_gettime(CLOCK_MONOTONIC, &ul_recv_end);
            //     ul_recv_time_ns = time_diff_ns(&ul_slot_start, &ul_recv_end);

            //     // Send accumulated UL data
            //     clock_gettime(CLOCK_MONOTONIC, &ul_socket_start);

            //     if (send_frame(sock_ul, ul_data_to_socket, ul_data_to_socket_offset) < 0) {
            //         printf("ERROR: UL socket send failed (interrupted by tick)\n");
            //     } else {
            //         clock_gettime(CLOCK_MONOTONIC, &ul_socket_end);
            //         ul_socket_time_ns = time_diff_ns(&ul_socket_start, &ul_socket_end);

            //         // Mark timing as ready (will be printed on NEXT slot_tick)
            //         ul_timing_ready = true;
            //         printf("Delayed: ");
            //     }

            //     // Reset UL state
            //     ul_data_pdu_count = 0;
            //     ul_data_to_socket_offset = 0;
            //     ul_slot_bytes_received = 0;
            //     ul_slot_pdus_received = 0;
            //     ul_slot_active = false;
            // }

            // // Process the slot_tick for DL (NO TIMING)
            // if(ul_recv_started == 0) {
            //     parse_slot_tick(rx_pkt_data + ETH_HDR_LEN, sock_dl);
            // }

        }
        // ========== UL DATA PACKET ==========
        else if (rx_pkt_data[ETH_HDR_LEN] & UL_DATA_INDICATOR_MASK) {
            // printf("nb_rx= %d\n",nb_rx);
            // Start timing on first UL packet of slot
            if (!ul_slot_active) {
                clock_gettime(CLOCK_MONOTONIC, &ul_slot_start);
                ul_slot_active = true;
                ul_slot_pdus_received = 0;
                ul_slot_bytes_received = 0;
            }

            // Extract packet counter from FPGA header (at offset ETH_HDR_LEN + 6, 4 bytes)
            uint32_t fpga_pkt_counter = rte_le_to_cpu_16(*(const uint16_t *)(rx_pkt_data + ETH_HDR_LEN + 6));

            printf("Received UL data packet %d (pkt: %d) with l=%u, FPGA_counter=%u\n",
                   ul_data_pdu_count, pkt_idx, m->data_len, fpga_pkt_counter);

            // Calculate payload size (excluding 64-byte header)
            uint32_t payload_size = m->data_len - NUM_BYTES_HDR_RESERVED;

            // Copy payload to accumulation buffer
            // With -O3, this memcpy is optimized with AVX-512
            memcpy(ul_data_to_socket + ul_data_to_socket_offset,
                   rx_pkt_data + NUM_BYTES_HDR_RESERVED,
                   payload_size);

            ul_data_to_socket_offset += payload_size;
            ul_slot_bytes_received += payload_size;
            ul_slot_pdus_received++;
            ul_data_pdu_count++;

            // Check if slot is complete

            bool slot_complete = (ul_data_to_socket_offset >= LAYER_NUM * TOTAL_BYTES_PER_SLOT);
            // bool slot_complete = (ul_data_pdu_count >= TOTAL_MTUs_PER_SLOT) ||
            //                      (m->data_len < RX_DATA_LEN);

            if (slot_complete) {
                clock_gettime(CLOCK_MONOTONIC, &ul_recv_end);
                ul_recv_time_ns = time_diff_ns(&ul_slot_start, &ul_recv_end);

                // Send accumulated data to socket
                clock_gettime(CLOCK_MONOTONIC, &ul_socket_start);

                // AARON config: send_frame is REQUIRED for gnuradio dpdk_uplink_source
                // to receive UL data. team's version had this disabled with `if (false)`
                // (the commented `if (send_frame...)` above is the original team line);
                // restored here so the gr block actually gets data.
                if (send_frame(sock_ul, ul_data_to_socket, ul_data_to_socket_offset) < 0) {
                    printf("ERROR: UL socket send failed\n");
                    ul_timing_ready = false;
                } else {
                    clock_gettime(CLOCK_MONOTONIC, &ul_socket_end);
                    ul_socket_time_ns = time_diff_ns(&ul_socket_start, &ul_socket_end);

                    // Mark timing as ready to print on next slot_tick
                    ul_timing_ready = true;
                }

                // Reset UL state for next slot
                ul_data_pdu_count = 0;
                ul_data_to_socket_offset = 0;
                // Keep ul_slot_bytes_received and ul_slot_pdus_received for printing
                ul_slot_active = false;
            }


                    // ========== PRINT UL TIMING OF THE PREVIOUS SLOT ==========
            if (ul_timing_ready) {
                uint64_t total_ul_time = ul_recv_time_ns + ul_socket_time_ns;

                // Only print if total time > 5 µs (to avoid clutter)
                if (total_ul_time > 5000) {
                    printf("UL: FPGA->DPDK: %lu µs, DPDK->Consumer: %lu µs, Total: %lu µs (%u B, %u pkts)\n",
                            ul_recv_time_ns / 1000,
                            ul_socket_time_ns / 1000,
                            total_ul_time / 1000,
                            ul_slot_bytes_received,
                            ul_slot_pdus_received);
                }

                // Reset timing flag
                ul_timing_ready = false;
            }

        }
        // ========== UNKNOWN PACKET ==========
        else {
            printf("Received Unknown packet type\n");
        }

    } // End of packet processing loop

    // Free all received mbufs
    rte_pktmbuf_free_bulk(rx_pkts, nb_rx);
}

/**
 * Main function
 */
int
main(int argc, char **argv)
{
    int ret;
    static uint32_t run_itr = 0;

    log_init();

    /* Initialize DPDK EAL */
    ret = rte_eal_init(argc, argv);
    if (ret < 0) {
        rte_exit(EXIT_FAILURE, "Invalid EAL arguments\n");
    }

    // AARON config: load nco + 20-slot schedule from /dev/shm (written by the
    // gnuradio custom_dpdk blocks). overrides the compiled-in schedule and sets
    // ul_nco_word / dl_nco_word; falls back to compiled defaults if absent.
    load_runtime_config();

    dl_data_from_socket = malloc(TOTAL_BYTES_PER_SLOT*8*4 + 512); // Pre-allocate buffer for DL data per slot * 8 layers * 4 times BW
    if (!dl_data_from_socket) {
        printf("Malloc failed\n");
        exit(1);
    }

    ul_data_to_socket = malloc(TOTAL_BYTES_PER_SLOT*8*4 + 512); // Pre-allocate buffer for UL data per slot * 8 layers * 4 times BW
    if (!ul_data_to_socket) {
        printf("Malloc failed\n");
        exit(1);
    }

    printf("\nmain: First byte of dl_data_from_socket: %02x\n", dl_data_from_socket[0]);

    /*Initializing socket*/
    //Create UL socket

    sock_ul = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock_ul < 0) {
        perror("socket");
        exit(1);
    }
    setup_socket_buffers(sock_ul);

    // Connect Producer and consumer sockets
    struct sockaddr_un addr_dl, addr_ul;
    memset(&addr_ul, 0, sizeof(addr_ul));
    addr_ul.sun_family = AF_UNIX;
    strncpy(addr_ul.sun_path, SOCKET_PATH_UL, sizeof(addr_ul.sun_path) - 1);

    printf("Producer: Connecting to %s...\n", SOCKET_PATH_UL);
    while (connect(sock_ul, (struct sockaddr*)&addr_ul, sizeof(addr_ul)) < 0) {
        if (errno == ENOENT || errno == ECONNREFUSED) {
            usleep(100000); // Wait 100ms and retry
            continue;
        }
        perror("connect");
        exit(1);
    }
    printf("Producer: Connected!\n");

    // Remove old socket file
    unlink(SOCKET_PATH_DL);

    // Create DL socket
    int sock_dl_tmp = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock_dl_tmp < 0) {
        perror("socket");
        exit(1);
    }
    setup_socket_buffers(sock_dl_tmp);

    memset(&addr_dl, 0, sizeof(addr_dl));
    addr_dl.sun_family = AF_UNIX;
    strncpy(addr_dl.sun_path, SOCKET_PATH_DL, sizeof(addr_dl.sun_path) - 1);


    if (bind(sock_dl_tmp, (struct sockaddr*)&addr_dl, sizeof(addr_dl)) < 0) {
        perror("bind");
        exit(1);
    }

    if (listen(sock_dl_tmp, 1) < 0) {
        perror("listen");
        exit(1);
    }

    printf("Consumer: Listening on %s\n", SOCKET_PATH_DL);

    // Accept connection
    int sock_dl = accept(sock_dl_tmp, NULL, NULL);
    if (sock_dl < 0) {
        perror("accept");
        exit(1);
    }

    setup_socket_buffers(sock_dl);
    printf("Consumer: Producer connected!\n");

    /* Create mbuf pool with optimized parameters */
    mbuf_pool = rte_pktmbuf_pool_create("MBUF_POOL",
                                        NUM_MBUFS,
                                        MBUF_CACHE_SIZE,
                                        0,
                                        RTE_MBUF_DEFAULT_BUF_SIZE + 8192,
                                        rte_socket_id());
    if (mbuf_pool == NULL) {
        rte_exit(EXIT_FAILURE, "Cannot create mbuf pool\n");
    }

    printf("Created mbuf pool with %d mbufs, cache size %d\n",
           NUM_MBUFS, MBUF_CACHE_SIZE);

    /* Initialize the port */
    if (port_init() != 0) {
        rte_exit(EXIT_FAILURE, "Cannot initialize port %d\n", PORT_ID);
    }

    /*
     * NOTE: Enable RS-FEC using ethtool BEFORE running this application:
     *   sudo ethtool --set-fec enp179s0f1 encoding rs
     *
     * DPDK FEC APIs are not supported in this driver version (returns -EOPNOTSUPP)
     */
    // printf("\n⚠️  IMPORTANT: Ensure FEC is enabled via ethtool before running:\n");
    // printf("    sudo ethtool --set-fec enp179s0f1 encoding rs\n\n");

    /******************** start: Dummy transfer for testing UL sockets ********************/

    // // Allocate dummy data to test socket transmission
    // uint8_t *dummy_data = malloc(8*4*FRAME_SIZE);
    // for (int i = 0; i < 8*4*FRAME_SIZE/4; i++) {
    //     ((uint32_t*)dummy_data)[i] = i;
    // }

    // while(run_itr < 20)
    // {
    //     clock_gettime(CLOCK_MONOTONIC, &start_time);
    //     if(send_frame(sock_ul, dummy_data, 8*4*FRAME_SIZE) <0)
    //     {
    //         printf("Socket send error\n");
    //     }

    //     clock_gettime(CLOCK_MONOTONIC, &end_time);
    //     uint64_t elapsed_ns = time_diff_ns(&start_time, &end_time);
    //     printf("Producer: Sent frame of %d bytes in %lu us\n", 8*4*FRAME_SIZE, elapsed_ns/1000);
    //     // Sleep for 100ms before sending the next frame
    //     rte_delay_us_sleep(1000000); // Sleep 100ms between frames
    //     run_itr++;
    // }

    /******************** end: Dummy transfer for testing UL sockets ********************/

    sleep(5); // Sleep for 5 seconds before starting the main loop - To warmup the descriptor rings



    /* Run the transmission loop */
    while(running)  // Run for a fixed number of iterations for testing
    {
        // if(run_itr==5000)
        //     {
        //         sch_slot_array[9] = 0;
        //         sch_slot_array[12] = 0;


        //         rte_eth_stats_get(PORT_ID, &istats);

        //         printf("\nipackets: %lu\n", istats.ipackets);
        //         printf("ibytes: %lu\n", istats.ibytes);
        //         printf("imissed: %lu\n", istats.imissed);
        //         printf("rx_nombuf: %lu\n", istats.rx_nombuf);
        //         break;
        //     }
        slot_transmission_reception_loop(sock_dl, sock_ul);
        if(init_state == 2)
        {    run_itr++;
            // printf("Run iteration: %u\n", run_itr);
        }

     }
     log_dump("total_dl_latency.bin");
     log_free();


    free(dl_data_from_socket);

    rte_eth_stats_get(PORT_ID, &istats);

    printf("\n=== Final RX Statistics ===\n");
    printf("ipackets: %lu\n", istats.ipackets);
    printf("ibytes: %lu\n", istats.ibytes);
    printf("imissed: %lu  (NIC dropped - no descriptors)\n", istats.imissed);
    printf("ierrors: %lu  (total NIC errors - CRC, length, etc.)\n", istats.ierrors);
    printf("  └─ Errors during transfer: %lu  ← THIS IS THE PROBLEM!\n",
           istats.ierrors - initial_ierrors);
    printf("rx_nombuf: %lu  (mbuf allocation failures)\n", istats.rx_nombuf);
    printf("opackets: %lu\n", istats.opackets);
    printf("obytes: %lu\n", istats.obytes);
    printf("oerrors: %lu\n", istats.oerrors);

    /* Cleanup */
    printf("Cleaning up...\n");
    rte_eth_dev_stop(PORT_ID);
    rte_eth_dev_close(PORT_ID);
    rte_eal_cleanup();

    return 0;
}
