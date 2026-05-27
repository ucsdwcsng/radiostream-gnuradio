########################################################################
# Gnuradio Custom Dpdk C++ Library
########################################################################

# Find Dependencies
find_package(PkgConfig)

link_directories(
    ${DPDK_LIBRARY_DIRS}
    /usr/lib/x86_64-linux-gnu
)

# Source files
file(GLOB custom_dpdk_sources "*.cc")
list(FILTER custom_dpdk_sources EXCLUDE REGEX "qa_.*")

# Add Library
add_library(gnuradio-custom_dpdk SHARED ${custom_dpdk_sources})

# Link Libraries (Plain signature)
target_link_libraries(gnuradio-custom_dpdk 
    gnuradio::gnuradio-runtime 
)

# Include Directories
target_include_directories(gnuradio-custom_dpdk PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include/gnuradio>
    $<INSTALL_INTERFACE:include>
)

# Compilation Options
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(gnuradio-custom_dpdk PRIVATE -Wall -Wextra)
endif()

# Installation
install(TARGETS gnuradio-custom_dpdk
    EXPORT gnuradio-custom_dpdkTargets
    LIBRARY DESTINATION ${GR_LIBRARY_DIR}
    COMPONENT libs
)
