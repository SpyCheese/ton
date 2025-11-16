set(OVERLAY_SOURCE
  dumb-overlay.cpp
  overlay-manager.cpp
  overlay.cpp
  overlay-fec.cpp
  overlay-fec-broadcast.cpp
  overlay-broadcast.cpp
  overlay-peers.cpp

  dumb-overlay.hpp
  overlay-fec.hpp
  overlay-broadcast.hpp
  overlay-fec-broadcast.hpp
  overlay-manager.h
  overlay.h
  overlay.hpp
  overlays.h
)

add_library(overlay STATIC ${OVERLAY_SOURCE})

target_include_directories(overlay PUBLIC 
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}> 
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>/.. 
  ${OPENSSL_INCLUDE_DIR}
)
target_link_libraries(overlay PRIVATE tdutils tdactor adnl tl_api dht fec)

