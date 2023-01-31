# - Try to find SODIUM
# Once done this will define
#
#  SODIUM_FOUND - system has SODIUM
#  SODIUM_INCLUDE_DIRS - the SODIUM include directory
#  SODIUM_LIBRARY - Link these to use SODIUM


find_path(
    SODIUM_INCLUDE_DIR
    NAMES sodium.h
)

find_library(
    SODIUM_LIBRARY
    NAMES libsodium.a libsodium.lib
)

if (SODIUM_LIBRARY)
  message(STATUS "Found Sodium: ${SODIUM_LIBRARY}")
endif()

#set(SODIUM_INCLUDE_DIRS ${SODIUM_INCLUDE_DIR})
#set(SODIUM_LIBRARIES ${SODIUM_LIBRARY})

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(SodiumAndroid DEFAULT_MSG SODIUM_INCLUDE_DIR SODIUM_LIBRARY)
mark_as_advanced(SODIUM_INCLUDE_DIR SODIUM_LIBRARY)