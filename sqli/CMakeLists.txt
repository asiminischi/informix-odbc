add_library(ifx_sqli STATIC
  version.cpp
)

target_include_directories(ifx_sqli PUBLIC ${CMAKE_SOURCE_DIR})
target_compile_features(ifx_sqli PUBLIC cxx_std_26)
