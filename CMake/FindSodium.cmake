# - Try to find SECP256K1
# Once done this will define
#
#  SODIUM_FOUND - system has sodium
#  SODIUM_INCLUDE_DIR - the sodium include directory
#  SODIUM_LIBRARY - Link these to use sodium

find_path(
    SODIUM_INCLUDE_DIR
    NAMES sodium.h
    DOC "sodium.h include dir"
)

find_library(
    SODIUM_LIBRARY
    NAMES libsodium.lib
    DOC "sodium library"
)

set(SODIUM_INCLUDE_DIRS ${SODIUM_INCLUDE_DIR})
set(SODIUM_LIBRARIES ${SODIUM_LIBRARY})

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(SODIUM DEFAULT_MSG SODIUM_INCLUDE_DIR SODIUM_LIBRARY)
mark_as_advanced(SODIUM_INCLUDE_DIR SODIUM_LIBRARY)