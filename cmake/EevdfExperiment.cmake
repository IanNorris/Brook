# Keep the default unchanged; expose only the normal-request control being
# compared with the measured eight-vCPU Linux reference. Other hints/weights
# and the selected boot policy are unaffected.
set(BROOK_EEVDF_NORMAL_REQUEST_NS "4000000" CACHE STRING
    "Experimental EEVDF normal request: 4000000 baseline or 2800000 control (ns)")
set_property(CACHE BROOK_EEVDF_NORMAL_REQUEST_NS PROPERTY STRINGS 4000000 2800000)
if(NOT BROOK_EEVDF_NORMAL_REQUEST_NS MATCHES "^(4000000|2800000)$")
    message(FATAL_ERROR "BROOK_EEVDF_NORMAL_REQUEST_NS must be 4000000 or 2800000")
endif()
