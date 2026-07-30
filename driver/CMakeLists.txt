add_library(ifxodbc SHARED
  handles.cpp
  handle_api.cpp
  connstr.cpp
  connect.cpp
  diag.cpp
  info.cpp
  error_map.cpp
  convert.cpp
  convert_native.cpp
  statement.cpp
  stmt_attr.cpp
)

target_include_directories(ifxodbc PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(ifxodbc PRIVATE ifx_sqli)
target_compile_features(ifxodbc PUBLIC cxx_std_26)

# Only the functions explicitly marked IFXODBC_EXPORT (driver/handles.h) are
# exported; everything else defaults to hidden. See handles.h's comment on
# IFXODBC_EXPORT for why this isn't just tidiness - it prevents this
# driver's internal cross-calls from being interposed by a same-named
# symbol in whatever Driver Manager loads it.
set_target_properties(ifxodbc PROPERTIES
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
)

# ODBC headers (sql.h/sqlext.h/sqltypes.h): part of the Windows SDK on
# Windows (already on the include path via the platform toolchain), and
# provided by unixodbc-dev on Linux (also on the default include path once
# installed - no extra include dir needed on either platform).
