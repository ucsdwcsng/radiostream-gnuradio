#ifndef INCLUDED_CUSTOM_DPDK_DPDK_UPLINK_SOURCE_H
#define INCLUDED_CUSTOM_DPDK_DPDK_UPLINK_SOURCE_H

#include <gnuradio/custom_dpdk/api.h>
#include <gnuradio/sync_block.h>

namespace gr {
  namespace custom_dpdk {
    class CUSTOM_DPDK_API dpdk_uplink_source : virtual public gr::sync_block
    {
    public:
      typedef std::shared_ptr<dpdk_uplink_source> sptr;
      static sptr make(std::string socket_path, int bytes_per_slot, int cpu_core, double nco_freq_mhz);
    };
  } // namespace custom_dpdk
} // namespace gr
#endif /* INCLUDED_CUSTOM_DPDK_DPDK_UPLINK_SOURCE_H */
