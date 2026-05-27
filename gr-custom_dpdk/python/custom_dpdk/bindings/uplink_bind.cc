#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <gnuradio/custom_dpdk/dpdk_uplink_source.h>

namespace py = pybind11;

namespace gr {
namespace custom_dpdk {

void bind_dpdk_uplink_source(py::module& m)
{
    using dpdk_uplink_source = gr::custom_dpdk::dpdk_uplink_source;

    py::class_<dpdk_uplink_source,
               gr::sync_block,
               gr::block,
               gr::basic_block,
               std::shared_ptr<dpdk_uplink_source>>(m, "dpdk_uplink_source")
        .def(py::init(&dpdk_uplink_source::make),
             py::arg("socket_path"),
             py::arg("bytes_per_slot"),
             py::arg("cpu_core"),
             py::arg("nco_freq_mhz"));
}

} // namespace custom_dpdk
} // namespace gr
