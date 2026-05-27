include(GrPybind)
link_directories(/usr/lib/x86_64-linux-gnu)
pybind11_add_module(custom_dpdk_python
    bindings.cc
    downlink_bind.cc
    uplink_bind.cc
)

# Force same pybind11 ABI as radioconda's gnuradio.
# gnuradio was compiled with older GCC that produces:
#   __pybind11_internals_v5_libstdcpp__
# Our GCC 15 produces:
#   __pybind11_internals_v5_gcc_libstdcpp_cxxabi1020__
# Override the component macros to match gnuradio.
target_compile_definitions(custom_dpdk_python PRIVATE
    "PYBIND11_COMPILER_TYPE=\"\""
    "PYBIND11_BUILD_ABI=\"\""
)

target_link_libraries(custom_dpdk_python PRIVATE
    gnuradio-custom_dpdk
    gnuradio::gnuradio-runtime
    ${DPDK_LIBRARIES}
    ${CMAKE_DL_LIBS}
)
target_link_options(custom_dpdk_python PRIVATE 
    "LINKER:/home/wcsng-901/radioconda/lib/python3.12/site-packages/gnuradio/gr/gr_python.cpython-312-x86_64-linux-gnu.so"
)
target_compile_features(custom_dpdk_python PRIVATE cxx_std_17)
install(TARGETS custom_dpdk_python
    DESTINATION ${GR_PYTHON_DIR}/custom_dpdk
    COMPONENT pythonapi
)
