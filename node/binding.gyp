{
  "targets": [
    {
      "target_name": "informix_native",
      "sources": [
        "src/addon.cpp",
        "vendor/sqli/version.cpp",
        "vendor/sqli/transport.cpp",
        "vendor/sqli/handshake.cpp",
        "vendor/sqli/connection.cpp",
        "vendor/sqli/statement.cpp",
        "vendor/sqli/types.cpp",
        "vendor/driver/convert.cpp",
        "vendor/driver/insert_table_name.cpp"
      ],
      "include_dirs": [
        "<!(node -p \"require('node-addon-api').include_dir\")",
        "vendor"
      ],
      "defines": ["NAPI_VERSION=8", "NAPI_CPP_EXCEPTIONS"],
      "cflags!": ["-fno-exceptions"],
      "cflags_cc!": ["-fno-exceptions", "-fno-rtti", "-std=gnu++17", "-std=gnu++20", "-std=c++17", "-std=c++20"],
      "cflags_cc": ["-std=c++26", "-fexceptions"],
      "xcode_settings": {
        "GCC_ENABLE_CPP_EXCEPTIONS": "YES",
        "CLANG_CXX_LANGUAGE_STANDARD": "c++2c",
        "MACOSX_DEPLOYMENT_TARGET": "13.3"
      },
      "msvs_settings": {
        "VCCLCompilerTool": {
          "ExceptionHandling": 1,
          "AdditionalOptions": ["/std:c++latest"]
        }
      },
      "conditions": [
        ["OS=='win'", { "libraries": ["ws2_32.lib"] }],
        ["OS=='linux'", { "ldflags": ["-static-libstdc++", "-static-libgcc"] }]
      ]
    }
  ]
}
