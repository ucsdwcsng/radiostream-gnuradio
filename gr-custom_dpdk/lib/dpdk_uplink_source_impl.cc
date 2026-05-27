#include "dpdk_uplink_source_impl.h"
#include <gnuradio/io_signature.h>
#include <cstdio>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <iostream>
#include <cstring>
#include <algorithm>
#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>

namespace gr {
    namespace custom_dpdk {

        dpdk_uplink_source::sptr
        dpdk_uplink_source::make(std::string socket_path, int bytes_per_slot, int cpu_core, double nco_freq_mhz)
        {
            return gnuradio::get_initial_sptr(
                new dpdk_uplink_source_impl(socket_path, bytes_per_slot, cpu_core, nco_freq_mhz));
        }

        dpdk_uplink_source_impl::dpdk_uplink_source_impl(std::string socket_path, int bytes_per_slot, int cpu_core, double nco_freq_mhz)
            : gr::sync_block("dpdk_uplink_source",
                gr::io_signature::make(0, 0, 0),
                gr::io_signature::make(1, 1, sizeof(short))),
            d_socket_path(socket_path),
            d_bytes_per_slot(bytes_per_slot),
            d_cpu_core(cpu_core),
            d_nco_freq_mhz(nco_freq_mhz),
            d_server_sock(-1),
            d_client_sock(-1),
            d_connected(false),
            d_drain_offset(0)
        {
            // pre-allocate ring slots
            // each slot must hold a full slot worth of payload (no 4 byte header
            // as the dl uses, since the length is stored separately in d_ring_lens)
            for (int i = 0; i < RING_SIZE; i++) {
                d_ring[i].resize(d_bytes_per_slot);
                d_ring_lens[i].store(0);
            }

            std::cout << "DPDK Source: init, bytes_per_slot=" << d_bytes_per_slot
                    << ", ring=" << RING_SIZE << " slots" << std::endl;

            // write ul nco freq (mhz) for the dpdk app to read at startup.
            // must precede setup_server()'s blocking accept(): the dpdk app reads
            // this config during its own startup, before it connects to us.
            FILE *nco_cfg = fopen("/dev/shm/dpdk_nco_ul_mhz.conf", "w");
            if (nco_cfg) {
                fprintf(nco_cfg, "%.10g\n", d_nco_freq_mhz);
                fclose(nco_cfg);
                std::cout << "DPDK Source: wrote ul nco=" << d_nco_freq_mhz
                        << " MHz to /dev/shm/dpdk_nco_ul_mhz.conf" << std::endl;
            } else {
                std::cerr << "DPDK Source: failed to write ul nco config file" << std::endl;
            }

            // setup server in constructor (matches original timing)
            setup_server();
        }

        dpdk_uplink_source_impl::~dpdk_uplink_source_impl()
        {
            if (d_client_sock >= 0) close(d_client_sock);
            if (d_server_sock >= 0) close(d_server_sock);
            unlink(d_socket_path.c_str());
        }

        void dpdk_uplink_source_impl::setup_server()
        {
            unlink(d_socket_path.c_str());

            d_server_sock = socket(AF_UNIX, SOCK_STREAM, 0);
            if (d_server_sock < 0) {
                std::cerr << "DPDK Source: Socket create failed: " << strerror(errno) << std::endl;
                return;
            }

            int bufsize = 8 * 1024 * 1024;
            setsockopt(d_server_sock, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));
            setsockopt(d_server_sock, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize));

            struct sockaddr_un addr;
            memset(&addr, 0, sizeof(addr));
            addr.sun_family = AF_UNIX;
            strncpy(addr.sun_path, d_socket_path.c_str(), sizeof(addr.sun_path) - 1);

            if (bind(d_server_sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
                std::cerr << "DPDK Source: Bind failed: " << strerror(errno) << std::endl;
                return;
            }

            if (listen(d_server_sock, 1) < 0) {
                std::cerr << "DPDK Source: Listen failed: " << strerror(errno) << std::endl;
                return;
            }

            std::cout << "DPDK Source: Listening on " << d_socket_path
                    << ", waiting for DPDK app to connect..." << std::endl;

            d_client_sock = accept(d_server_sock, NULL, NULL);
            if (d_client_sock < 0) {
                std::cerr << "DPDK Source: Accept failed: " << strerror(errno) << std::endl;
                return;
            }

            // no recv timeout: reader thread blocks indefinitely waiting for data
            // (decoupled from gnu radio scheduler so blocking is fine here)
            setsockopt(d_client_sock, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));
            setsockopt(d_client_sock, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize));

            d_connected = true;
            std::cout << "DPDK Source: DPDK app connected!" << std::endl;
        }

        // single recv (length header + payload) into the provided buffer
        int dpdk_uplink_source_impl::recv_frame(uint8_t *buf, size_t buf_capacity)
        {
            if (d_client_sock < 0) return -1;

            // receive 4 byte length header
            uint32_t net_len;
            ssize_t rc = recv(d_client_sock, &net_len, sizeof(net_len), MSG_WAITALL);
            if (rc != sizeof(net_len)) {
                d_connected = false;
                return -1;
            }

            uint32_t len = ntohl(net_len);
            if (len > buf_capacity) {
                std::cerr << "DPDK Source: Frame too large: " << len << " bytes (cap "
                          << buf_capacity << ")" << std::endl;
                d_connected = false;
                return -1;
            }

            // single syscall for entire payload
            rc = recv(d_client_sock, buf, len, MSG_WAITALL);
            if (rc != (ssize_t)len) {
                d_connected = false;
                return -1;
            }

            return (int)len;
        }

        // dedicated reader thread
        void dpdk_uplink_source_impl::reader_loop()
        {
            // pin thread to specified core
            if (d_cpu_core >= 0) {
                cpu_set_t cpuset;
                CPU_ZERO(&cpuset);
                CPU_SET(d_cpu_core, &cpuset);
                pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
                std::cout << "DPDK Source: reader thread pinned to core " << d_cpu_core << std::endl;
            }

            while (d_running.load(std::memory_order_relaxed)) {
                // wait for ring space to be available
                {
                    std::unique_lock<std::mutex> lock(d_mutex);
                    d_cv.wait(lock, [this] {
                        return d_ring_count.load() < RING_SIZE || !d_running.load();
                    });
                }

                if (!d_running.load()) break;

                // not connected: bail (setup_server would have set d_connected on accept)
                if (!d_connected) {
                    usleep(100000);
                    continue;
                }

                // recv directly into the ring slot at write_idx
                int wi = d_write_idx.load();
                int frame_len = recv_frame(d_ring[wi].data(), d_ring[wi].size());
                if (frame_len < 0) {
                    std::cerr << "DPDK Source: recv error, will retry" << std::endl;
                    usleep(100000);
                    continue;
                }

                // commit slot
                d_ring_lens[wi].store((size_t)frame_len);
                d_write_idx.store((wi + 1) % RING_SIZE);
                d_ring_count.fetch_add(1);
                d_cv.notify_one();
            }
        }

        bool dpdk_uplink_source_impl::start()
        {
            std::cerr << "DPDK Source: start()" << std::endl;

            d_drain_offset = 0;
            d_write_idx.store(0);
            d_read_idx.store(0);
            d_ring_count.store(0);
            d_running.store(true);

            // launch reader thread (decoupled from gnu radio scheduler)
            d_reader_thread = std::thread(&dpdk_uplink_source_impl::reader_loop, this);

            return true;
        }

        bool dpdk_uplink_source_impl::stop()
        {
            std::cerr << "DPDK Source: stop()" << std::endl;
            d_running.store(false);
            d_cv.notify_all();

            if (d_reader_thread.joinable())
                d_reader_thread.join();

            if (d_client_sock >= 0) {
                close(d_client_sock);
                d_client_sock = -1;
            }
            if (d_server_sock >= 0) {
                close(d_server_sock);
                d_server_sock = -1;
            }
            d_connected = false;
            return true;
        }

        int dpdk_uplink_source_impl::work(int noutput_items,
                                        gr_vector_const_void_star &input_items,
                                        gr_vector_void_star &output_items)
        {
            (void)input_items;

            short *out = (short *)output_items[0];
            size_t total_bytes_needed = (size_t)noutput_items * sizeof(short);
            size_t produced_bytes = 0;

            while (produced_bytes < total_bytes_needed) {
                // wait for a slot to be available (non-blocking peek)
                if (d_ring_count.load() == 0) {
                    break; // nothing in ring right now, return what we have
                }

                int ri = d_read_idx.load();
                size_t slot_len = d_ring_lens[ri].load();
                size_t bytes_remaining_in_slot = slot_len - d_drain_offset;
                size_t bytes_to_copy = std::min(bytes_remaining_in_slot,
                                                total_bytes_needed - produced_bytes);

                // align copy size to short boundary
                bytes_to_copy = (bytes_to_copy / sizeof(short)) * sizeof(short);
                if (bytes_to_copy == 0) break;

                memcpy((uint8_t *)out + produced_bytes,
                       d_ring[ri].data() + d_drain_offset,
                       bytes_to_copy);

                d_drain_offset += bytes_to_copy;
                produced_bytes += bytes_to_copy;

                // slot fully consumed: free it and advance read pointer
                if (d_drain_offset >= slot_len) {
                    d_read_idx.store((ri + 1) % RING_SIZE);
                    d_ring_count.fetch_sub(1);
                    d_cv.notify_one();
                    d_drain_offset = 0;
                }
            }

            int produced = (int)(produced_bytes / sizeof(short));

            // if no data was available, output zeros and pause briefly
            // (matches original behavior: prevents tight-loop cpu spin when starved)
            if (produced == 0) {
                int n = std::min(noutput_items, 4096);
                memset(out, 0, n * sizeof(short));
                produced = n;
                usleep(50000); // 50ms pause otherwise cpu throttles gnu radio scheduler
            }

            return produced;
        }

    } /* namespace custom_dpdk */
} /* namespace gr */

