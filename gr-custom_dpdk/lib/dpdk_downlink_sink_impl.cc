#include "dpdk_downlink_sink_impl.h"
#include <gnuradio/io_signature.h>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cstring>
#include <arpa/inet.h>
#include <iostream>
#include <algorithm>
#include <pthread.h>
#include <sched.h>

namespace gr {
    namespace custom_dpdk {

        dpdk_downlink_sink::sptr
        dpdk_downlink_sink::make(std::string socket_path, int bytes_per_slot, int cpu_core, double nco_freq_mhz, std::vector<int> slot_schedule)
        {
            return gnuradio::get_initial_sptr(
                new dpdk_downlink_sink_impl(socket_path, bytes_per_slot, cpu_core, nco_freq_mhz, slot_schedule));
        }

        dpdk_downlink_sink_impl::dpdk_downlink_sink_impl(std::string socket_path, int bytes_per_slot, int cpu_core, double nco_freq_mhz, std::vector<int> slot_schedule)
            : gr::sync_block("dpdk_downlink_sink",
                gr::io_signature::make(1, 1, sizeof(short)),
                gr::io_signature::make(0, 0, 0)),
            d_socket_path(socket_path),
            d_bytes_per_slot(bytes_per_slot),
            d_cpu_core(cpu_core),
            d_nco_freq_mhz(nco_freq_mhz),
            d_slot_schedule(slot_schedule),
            d_sock_fd(-1),
            d_connected(false),
            d_fill(0)
        {

            // validate the 20-slot schedule (0=idle, 1=ul, 2=dl, 3=both)
            if (d_slot_schedule.size() != 20) {
                throw std::invalid_argument(
                    "dpdk_downlink_sink: slot_schedule must have exactly 20 entries (got "
                    + std::to_string(d_slot_schedule.size()) + ")");
            }
            for (size_t i = 0; i < d_slot_schedule.size(); i++) {
                int v = d_slot_schedule[i];
                if (v < 0 || v > 3) {
                    throw std::invalid_argument(
                        "dpdk_downlink_sink: slot_schedule[" + std::to_string(i) + "]="
                        + std::to_string(v) + " out of range (must be 0,1,2,3)");
                }
            }

            // write dl nco freq (mhz) for the dpdk app to read at startup
            FILE *nco_cfg = fopen("/dev/shm/dpdk_nco_dl_mhz.conf", "w");
            if (nco_cfg) {
                fprintf(nco_cfg, "%.10g\n", d_nco_freq_mhz);
                fclose(nco_cfg);
                std::cout << "DPDK Sink: wrote dl nco=" << d_nco_freq_mhz
                        << " MHz to /dev/shm/dpdk_nco_dl_mhz.conf" << std::endl;
            } else {
                std::cerr << "DPDK Sink: failed to write dl nco config file" << std::endl;
            }

            // write the 20-slot ul/dl schedule for the dpdk app
            FILE *sch_cfg = fopen("/dev/shm/dpdk_slot_schedule.conf", "w");
            if (sch_cfg) {
                for (size_t i = 0; i < d_slot_schedule.size(); i++)
                    fprintf(sch_cfg, "%d%c", d_slot_schedule[i],
                            (i + 1 < d_slot_schedule.size()) ? ' ' : '\n');
                fclose(sch_cfg);
                std::cout << "DPDK Sink: wrote 20-slot schedule to /dev/shm/dpdk_slot_schedule.conf" << std::endl;
            } else {
                std::cerr << "DPDK Sink: failed to write slot schedule config file" << std::endl;
            }
            
            
            // pre-allocate ring slotss
            for (int i = 0; i < RING_SIZE; i++) {
                d_ring[i].resize(4 + d_bytes_per_slot);
                // pre-write header
                uint32_t net_len = htonl((uint32_t)d_bytes_per_slot);
                memcpy(d_ring[i].data(), &net_len, 4);
            }

            std::cout << "DPDK Sink: init, bytes_per_slot=" << d_bytes_per_slot
                    << ", ring=" << RING_SIZE << " slots" << std::endl;
        }

        dpdk_downlink_sink_impl::~dpdk_downlink_sink_impl()
        {
            if (d_sock_fd >= 0)
                close(d_sock_fd);
        }

        void dpdk_downlink_sink_impl::connect_socket()
        {
            if (d_sock_fd >= 0) {
                close(d_sock_fd);
                d_sock_fd = -1;
            }

            d_sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
            if (d_sock_fd < 0) return;

            int bufsize = 8 * 1024 * 1024;
            setsockopt(d_sock_fd, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));
            setsockopt(d_sock_fd, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize));

            struct sockaddr_un addr;
            memset(&addr, 0, sizeof(addr));
            addr.sun_family = AF_UNIX;
            strncpy(addr.sun_path, d_socket_path.c_str(), sizeof(addr.sun_path) - 1);

            std::cout << "DPDK Sink: connecting to " << d_socket_path << "..." << std::endl;

            for (int i = 0; i < 150; i++) {
                if (connect(d_sock_fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
                    std::cout << "DPDK Sink: connected" << std::endl;
                    d_connected = true;
                    return;
                }
                usleep(200000);
            }

            std::cerr << "DPDK Sink: connection failed after 30s" << std::endl;
            close(d_sock_fd);
            d_sock_fd = -1;
        }

        // single send + single recv
        int dpdk_downlink_sink_impl::send_frame(const uint8_t *buf, size_t len)
        {
            ssize_t sent = send(d_sock_fd, buf, len, MSG_NOSIGNAL);
            if (sent != (ssize_t)len) {
                d_connected = false;
                return -1;
            }

            uint32_t ack;
            ssize_t rc = recv(d_sock_fd, &ack, sizeof(ack), MSG_WAITALL);
            if (rc != sizeof(ack) || ntohl(ack) != (uint32_t)d_bytes_per_slot) {
                d_connected = false;
                return -1;
            }

            return 0;
        }

        // dedicated sender thread
        void dpdk_downlink_sink_impl::sender_loop()
        {
            // pin thread to specified core
            if (d_cpu_core >= 0) {
                cpu_set_t cpuset;
                CPU_ZERO(&cpuset);
                CPU_SET(d_cpu_core, &cpuset);
                pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
                std::cout << "DPDK Sink: sender thread pinned to core " << d_cpu_core << std::endl;
            }

            while (d_running.load(std::memory_order_relaxed)) {
                // wait for a frame to be available
                {
                    std::unique_lock<std::mutex> lock(d_mutex);
                    d_cv.wait(lock, [this] {
                        return d_ring_count.load() > 0 || !d_running.load();
                    });
                }

                if (!d_running.load()) break;

                // reconnect if needed
                if (!d_connected) {
                    connect_socket();
                    if (!d_connected) {
                        usleep(100000);
                        continue;
                    }
                }

                // send the frame at read_idx
                int ri = d_read_idx.load();
                size_t total = 4 + (size_t)d_bytes_per_slot;

                if (send_frame(d_ring[ri].data(), total) < 0) {
                    std::cerr << "DPDK Sink: send error, reconnecting" << std::endl;
                    continue;
                }

                // advance read pointer, free the slot
                d_read_idx.store((ri + 1) % RING_SIZE);
                d_ring_count.fetch_sub(1);
            }
        }

        bool dpdk_downlink_sink_impl::start()
        {
            std::cerr << "DPDK Sink: start()" << std::endl;
            connect_socket();

            d_fill = 0;
            d_write_idx.store(0);
            d_read_idx.store(0);
            d_ring_count.store(0);
            d_running.store(true);

            // launch sender thread (decoupled from gnu radio scheduler)
            d_sender_thread = std::thread(&dpdk_downlink_sink_impl::sender_loop, this);

            return true;
        }

        bool dpdk_downlink_sink_impl::stop()
        {
            std::cerr << "DPDK Sink: stop()" << std::endl;
            d_running.store(false);
            d_cv.notify_all();

            if (d_sender_thread.joinable())
                d_sender_thread.join();

            if (d_sock_fd >= 0) {
                close(d_sock_fd);
                d_sock_fd = -1;
            }
            d_connected = false;
            return true;
        }

        int dpdk_downlink_sink_impl::work(int noutput_items,
                                        gr_vector_const_void_star &input_items,
                                        gr_vector_void_star &output_items)
        {
            (void)output_items;

            const uint8_t *in = (const uint8_t *)input_items[0];
            size_t total_bytes = (size_t)noutput_items * sizeof(short);
            size_t offset = 0;

            while (offset < total_bytes) {
                int wi = d_write_idx.load();
                size_t space = (size_t)d_bytes_per_slot - d_fill;
                size_t chunk = std::min(space, total_bytes - offset);

                // copy into payload area of current write slot (after 4-byte header)
                memcpy(&d_ring[wi][4 + d_fill], &in[offset], chunk);
                d_fill += chunk;
                offset += chunk;

                // slot full: commit to ring and wake sender
                if (d_fill >= (size_t)d_bytes_per_slot) {
                    // wait if ring is full (backpressure)
                    while (d_ring_count.load() >= RING_SIZE) {
                        if (!d_running.load())
                            return (int)(offset / sizeof(short));
                        usleep(50);
                    }

                    d_write_idx.store((wi + 1) % RING_SIZE);
                    d_ring_count.fetch_add(1);
                    d_cv.notify_one();
                    d_fill = 0;
                }
            }

            return noutput_items;
        }

    } /* namespace custom_dpdk */
} /* namespace gr */
