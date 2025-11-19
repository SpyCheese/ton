set(ATCP_SOURCE
  atcp.hpp
  atcp.cpp
  atcp-connection.hpp
  atcp-connection.cpp
)

add_library(atcp STATIC ${ATCP_SOURCE})

target_include_directories(atcp PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}> 
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>/.. 
  ${OPENSSL_INCLUDE_DIR}
)
target_link_libraries(atcp PUBLIC tdutils tdactor fec adnl tl_api)

