{
  "variables": {
      "os_linux_compiler%": "gcc",
      "build_v8_with_gn": "false"
  },
  "targets": [
    {
      "target_name": "msgpackr-extract",
      "win_delay_load_hook": "false",
      "sources": [
        "src/extract.cpp",
      ],
      "include_dirs": [
        "<!(node -e \"require('nan')\")",
      ],
      "conditions": [
        ["OS=='linux'", {
          "variables": {
            "gcc_version" : "<!(<(os_linux_compiler) -dumpversion | cut -d '.' -f 1)",
          },
          "conditions": [
            ["gcc_version>=7", {
              "cflags": [
                "-Wimplicit-fallthrough=2",
              ],
            }],
            ["node_module_version >= 93", {
              "cflags_cc": [
                "-fPIC",
                "-fvisibility=hidden",
                "-fvisibility-inlines-hidden",
                "-std=c++14"
              ]
            }, {
             "cflags_cc": [
              "-fPIC",
              "-fvisibility=hidden",
              "-fvisibility-inlines-hidden",
              "-std=c++11"
              ],
            }],
          ],
          "ldflags": [
            "-fPIC",
            "-fvisibility=hidden"
          ],
          "cflags": [
            "-fPIC",
            "-fvisibility=hidden",
            "-O3"
          ],
        }],
        ["OS=='mac'", {
          "xcode_settings": {
            "OTHER_CPLUSPLUSFLAGS" : ["-std=c++14"],
            "MACOSX_DEPLOYMENT_TARGET": "10.7",
            "OTHER_LDFLAGS": ["-std=c++11"],
            "CLANG_CXX_LIBRARY": "libc++"
          }
        }],
      ],
    }
  ]
}
