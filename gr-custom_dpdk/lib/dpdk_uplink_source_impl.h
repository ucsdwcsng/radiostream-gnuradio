#ifndef INCLUDED_CUSTOM_DPDK_DPDK_UPLINK_SOURCE_IMPL_H
#define INCLUDED_CUSTOM_DPDK_DPDK_UPLINK_SOURCE_IMPL_H

#include <gnuradio/custom_dpdk/dpdk_uplink_source.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <vector>
#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>

namespace gr {
  namespace custom_dpdk {

      class dpdk_uplink_source_impl : public dpdk_uplink_source
      {
        private:
          std::string d_socket_path;
          int d_bytes_per_slot;
          int d_cpu_core;
          double d_nco_freq_mhz;
          int d_server_sock;
          int d_client_sock;
          bool d_connected;

          // ring buffer of N slot-sized frames each with a length field
          static constexpr int RING_SIZE = 128;
          std::vector<uint8_t> d_ring[RING_SIZE];
          std::atomic<size_t> d_ring_lens[RING_SIZE]; // actual valid bytes per slot
          std::atomic<int> d_write_idx{0};
          std::atomic<int> d_read_idx{0};
          std::atomic<int> d_ring_count{0}; // frames available to consume

          // partial-consume tracking: how far work() has read into the current ring slot
          size_t d_drain_offset;

          // reader thread
          std::thread d_reader_thread;
          std::mutex d_mutex;
          std::condition_variable d_cv;
          std::atomic<bool> d_running{false};

          void setup_server();
          int recv_frame(uint8_t *buf, size_t buf_capacity);
          void reader_loop();

        public:
          dpdk_uplink_source_impl(std::string socket_path, int bytes_per_slot, int cpu_core, double nco_freq_mhz);
          ~dpdk_uplink_source_impl();

          bool start() override;
          bool stop() override;

          int work(int noutput_items,
                  gr_vector_const_void_star &input_items,
                  gr_vector_void_star &output_items);
      };

  } // namespace custom_dpdk
} // namespace gr

#endif /* INCLUDED_CUSTOM_DPDK_DPDK_UPLINK_SOURCE_IMPL_H */
