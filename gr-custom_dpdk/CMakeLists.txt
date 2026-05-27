cmake_minimum_required(VERSION 3.8)
project(gr-custom_dpdk CXX C)
set(CMAKE_CXX_STANDARD 17)

# Find GNU Radio components
find_package(Gnuradio "3.10" REQUIRED COMPONENTS runtime)
find_package(Volk REQUIRED)
find_package(pybind11 REQUIRED)

# Standard Includes
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# Add subdirectories
add_subdirectory(include/gnuradio/custom_dpdk)
add_subdirectory(lib)
add_subdirectory(python/custom_dpdk)
add_subdirectory(grc)
