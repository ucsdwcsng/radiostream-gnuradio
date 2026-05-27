find_package(PkgConfig)

PKG_CHECK_MODULES(PC_GR_CUSTOM_DPDK gnuradio-custom_dpdk)

FIND_PATH(
    GR_CUSTOM_DPDK_INCLUDE_DIRS
    NAMES gnuradio/custom_dpdk/api.h
    HINTS $ENV{CUSTOM_DPDK_DIR}/include
        ${PC_CUSTOM_DPDK_INCLUDEDIR}
    PATHS ${CMAKE_INSTALL_PREFIX}/include
          /usr/local/include
          /usr/include
)

FIND_LIBRARY(
    GR_CUSTOM_DPDK_LIBRARIES
    NAMES gnuradio-custom_dpdk
    HINTS $ENV{CUSTOM_DPDK_DIR}/lib
        ${PC_CUSTOM_DPDK_LIBDIR}
    PATHS ${CMAKE_INSTALL_PREFIX}/lib
          ${CMAKE_INSTALL_PREFIX}/lib64
          /usr/local/lib
          /usr/local/lib64
          /usr/lib
          /usr/lib64
          )

include("${CMAKE_CURRENT_LIST_DIR}/gnuradio-custom_dpdkTarget.cmake")

INCLUDE(FindPackageHandleStandardArgs)
FIND_PACKAGE_HANDLE_STANDARD_ARGS(GR_CUSTOM_DPDK DEFAULT_MSG GR_CUSTOM_DPDK_LIBRARIES GR_CUSTOM_DPDK_INCLUDE_DIRS)
MARK_AS_ADVANCED(GR_CUSTOM_DPDK_LIBRARIES GR_CUSTOM_DPDK_INCLUDE_DIRS)
