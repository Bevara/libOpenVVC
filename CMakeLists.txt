cmake_minimum_required(VERSION 3.7)
project(Filters)

include(filters.cmake)

add_definitions(-fpic)

SET(LIBOPENVVC_SRC
        ${CMAKE_CURRENT_SOURCE_DIR}/dec_vvc.c
)

SET(LIBOPENVVC_LIB
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/libvvdec.a
)

SET(LIBOPENVVC_INC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# VVdeC is C++, but as in libheif no C++ runtime is linked into the side
# module: libc++/libc++abi come from the main module at load time.
add_filter(libOpenVVC
        "${LIBOPENVVC_SRC}"
        "${LIBOPENVVC_LIB}"
        []
        ""
        "${LIBOPENVVC_INC}"
        ""
        "1")
