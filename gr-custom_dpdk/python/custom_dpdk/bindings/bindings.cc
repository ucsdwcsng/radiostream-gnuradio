#include <pybind11/pybind11.h>
#include <gnuradio/custom_dpdk/api.h>

namespace py = pybind11;

namespace gr {
namespace custom_dpdk {

void bind_dpdk_downlink_sink(py::module& m);
void bind_dpdk_uplink_source(py::module& m);

PYBIND11_MODULE(custom_dpdk_python, m)
{
    m.doc() = "The custom_dpdk OOT module";

    // Import gnuradio.gr so pybind11 knows about gr::sync_block,
    // gr::block, and gr::basic_block base types.
    py::module::import("gnuradio.gr");

    bind_dpdk_downlink_sink(m);
    bind_dpdk_uplink_source(m);
}

} // namespace custom_dpdk
} // namespace gr
