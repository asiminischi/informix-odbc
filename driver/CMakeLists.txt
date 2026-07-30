add_library(ifxodbc SHARED
  handles.cpp
  handle_api.cpp
  connstr.cpp
  connect.cpp
  diag.cpp
  info.cpp
  error_map.cpp
  convert.cpp
  statement.cpp
  stmt_attr.cpp
)

target_include_directories(ifxodbc PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(ifxodbc PRIVATE ifx_sqli)
target_compile_features(ifxodbc PUBLIC cxx_std_26)

# ODBC headers (sql.h/sqlext.h/sqltypes.h): part of the Windows SDK on
# Windows (already on the include path via the platform toolchain), and
# provided by unixodbc-dev on Linux (also on the default include path once
# installed - no extra include dir needed on either platform).
