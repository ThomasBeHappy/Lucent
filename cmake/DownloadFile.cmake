cmake_minimum_required(VERSION 3.25)

# Usage:
#   cmake -DURL="..." -DOUT="path/to/file" [-DSHA256="..."] -P cmake/DownloadFile.cmake
#
# If SHA256 is provided, the download will be verified.

if(NOT DEFINED URL OR URL STREQUAL "")
    message(FATAL_ERROR "DownloadFile.cmake: URL is required")
endif()
if(NOT DEFINED OUT OR OUT STREQUAL "")
    message(FATAL_ERROR "DownloadFile.cmake: OUT is required")
endif()

get_filename_component(_out_dir "${OUT}" DIRECTORY)
file(MAKE_DIRECTORY "${_out_dir}")

if(EXISTS "${OUT}")
    message(STATUS "DownloadFile: already exists: ${OUT}")
    return()
endif()

message(STATUS "DownloadFile: downloading ${URL} -> ${OUT}")

set(_args
    "${URL}"
    "${OUT}"
    SHOW_PROGRESS
    TLS_VERIFY ON
)

if(DEFINED SHA256 AND NOT SHA256 STREQUAL "")
    list(APPEND _args EXPECTED_HASH "SHA256=${SHA256}")
endif()

file(DOWNLOAD ${_args} STATUS _status)
list(GET _status 0 _code)
list(GET _status 1 _msg)
if(NOT _code EQUAL 0)
    message(FATAL_ERROR "DownloadFile: failed (${_code}): ${_msg}")
endif()


