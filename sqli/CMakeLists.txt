add_library(ifx_sqli STATIC
  version.cpp
  transport.cpp
  handshake.cpp
  connection.cpp
  statement.cpp
  types.cpp
)

target_include_directories(ifx_sqli PUBLIC ${CMAKE_SOURCE_DIR})
target_compile_features(ifx_sqli PUBLIC cxx_std_26)

if(WIN32)
  target_link_libraries(ifx_sqli PUBLIC ws2_32)
endif()
