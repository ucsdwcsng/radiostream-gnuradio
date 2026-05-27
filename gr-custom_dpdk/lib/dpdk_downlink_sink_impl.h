#ifndef INCLUDED_CUSTOM_DPDK_DPDK_DOWNLINK_SINK_IMPL_H
#define INCLUDED_CUSTOM_DPDK_DPDK_DOWNLINK_SINK_IMPL_H

#include <gnuradio/custom_dpdk/dpdk_downlink_sink.h>
#include <vector>
#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>

namespace gr {
    namespace custom_dpdk {

        class dpdk_downlink_sink_impl : public dpdk_downlink_sink
        {
            private:
                std::string d_socket_path;
                int d_bytes_per_slot;
                int d_cpu_core;
                double d_nco_freq_mhz;
                std::vector<int> d_slot_schedule;
                int d_sock_fd;
                bool d_connected;

                // ring buffer of N slot-sized frames each with 4-byte header
                static constexpr int RING_SIZE = 8;
                std::vector<uint8_t> d_ring[RING_SIZE];
                std::atomic<int> d_write_idx{0};
                std::atomic<int> d_read_idx{0};
                std::atomic<int> d_ring_count{0}; // frames available to send

                // accumulation buffer for partial work() calls
                size_t d_fill;

                // sender thread
                std::thread d_sender_thread;
                std::mutex d_mutex;
                std::condition_variable d_cv;
                std::atomic<bool> d_running{false};

                void connect_socket();
                int send_frame(const uint8_t *buf, size_t len);
                void sender_loop();

            public:
                dpdk_downlink_sink_impl(std::string socket_path, int bytes_per_slot, int cpu_core, double nco_freq_mhz, std::vector<int> slot_schedule);
                ~dpdk_downlink_sink_impl();

                bool start() override;
                bool stop() override;

                int work(int noutput_items,
                        gr_vector_const_void_star &input_items,
                        gr_vector_void_star &output_items);
        };

    } // namespace custom_dpdk
} // namespace gr

#endif
