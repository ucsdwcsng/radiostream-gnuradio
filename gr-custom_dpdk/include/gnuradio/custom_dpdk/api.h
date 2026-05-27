#ifndef INCLUDED_CUSTOM_DPDK_API_H
#define INCLUDED_CUSTOM_DPDK_API_H

#include <gnuradio/attributes.h>

#ifdef gnuradio_custom_dpdk_EXPORTS
#  define CUSTOM_DPDK_API __GR_ATTR_EXPORT
#else
#  define CUSTOM_DPDK_API __GR_ATTR_IMPORT
#endif

#endif /* INCLUDED_CUSTOM_DPDK_API_H */
