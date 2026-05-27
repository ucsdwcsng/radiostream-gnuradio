#ifndef INCLUDED_CUSTOM_DPDK_DPDK_DOWNLINK_SINK_H
#define INCLUDED_CUSTOM_DPDK_DPDK_DOWNLINK_SINK_H

#include <gnuradio/custom_dpdk/api.h>
#include <gnuradio/sync_block.h>
#include <string>
#include <vector>

namespace gr {
  namespace custom_dpdk {

    class CUSTOM_DPDK_API dpdk_downlink_sink : virtual public gr::sync_block
    {
    public:
      typedef std::shared_ptr<dpdk_downlink_sink> sptr;
      static sptr make(std::string socket_path, int bytes_per_slot, int cpu_core, double nco_freq_mhz, std::vector<int> slot_schedule);
    };

  } // namespace custom_dpdk
} // namespace gr

#endif /* INCLUDED_CUSTOM_DPDK_DPDK_DOWNLINK_SINK_H */
