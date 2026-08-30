{
  "targets": [
    {
      "target_name": "audio_engine",
      "sources": [
        "src/buffer.cpp",
        "src/wav.cpp",
        "src/dsp.cpp",
        "src/analysis.cpp",
        "src/mixer.cpp",
        "src/generator.cpp",
        "src/napi_bindings.cpp"
      ],
      "include_dirs": ["<!(node -p \"require('node-addon-api').include_dir\")"],
      "defines": ["NAPI_VERSION=8"],
      "cflags!": ["-fno-exceptions"],
      "cflags_cc!": ["-fno-exceptions"],
      "conditions": [
        ["OS=='linux'", { "cflags_cc": ["-std=c++17", "-O2"] }],
        ["OS=='mac'", { "xcode_settings": { "CLANG_CXX_LANGUAGE_STANDARD": "c++17" } }],
        [
          "OS=='win'",
          {
            "msvs_settings": {
              "VCCLCompilerTool": {
                "ExceptionHandling": 1,
                "AdditionalOptions": ["/std:c++17", "/Zc:__cplusplus", "/O2"]
              }
            }
          }
        ]
      ]
    }
  ]
}
