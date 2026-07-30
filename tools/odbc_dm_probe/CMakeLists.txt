add_executable(odbc_dm_probe main.cpp)

if(WIN32)
  target_link_libraries(odbc_dm_probe PRIVATE odbc32)
else()
  find_library(ODBC_LIBRARY NAMES odbc)
  target_link_libraries(odbc_dm_probe PRIVATE ${ODBC_LIBRARY})
endif()
