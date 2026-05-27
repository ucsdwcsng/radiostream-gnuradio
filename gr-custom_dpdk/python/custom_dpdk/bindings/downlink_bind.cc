#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <gnuradio/custom_dpdk/dpdk_downlink_sink.h>

namespace py = pybind11;

namespace gr {
namespace custom_dpdk {

void bind_dpdk_downlink_sink(py::module& m)
{
    using dpdk_downlink_sink = gr::custom_dpdk::dpdk_downlink_sink;

    py::class_<dpdk_downlink_sink,
               gr::sync_block,
               gr::block,
               gr::basic_block,
               std::shared_ptr<dpdk_downlink_sink>>(m, "dpdk_downlink_sink")
        .def(py::init(&dpdk_downlink_sink::make),
	     py::arg("socket_path"),
	     py::arg("bytes_per_slot"),
	     py::arg("cpu_core"),
	     py::arg("nco_freq_mhz"),
	     py::arg("slot_schedule"));
}

} // namespace custom_dpdk
} // namespace gr
