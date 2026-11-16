# Shader compiler regression test

This standalone test needs no game assets. It compiles the SDK's RSQ/LOG
zero-input branches as vertex and pixel shaders through `D3DCompile`, using the
same compatibility helper and optimization level as the renderer. `precise`
is retained; positive and negative infinity remain exact IEEE values.

On Linux, after setting up xwin as described in the main README:

```sh
cmake -S tests/shader-compat -B out/build/shader-compat \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/toolchains/xwin.cmake" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build out/build/shader-compat
```

On Windows, configure the same directory from a Visual Studio developer prompt
with `-G Ninja -DCMAKE_CXX_COMPILER=clang++` instead of the xwin toolchain.

Run `shader_compat_test.exe` with Wine's built-in `d3dcompiler_47`, for example:

```sh
WINEDLLOVERRIDES="d3dcompiler_47=b" wine out/build/shader-compat/shader_compat_test.exe
```

For Proton, use its `runinprefix` verb with `STEAM_COMPAT_DATA_PATH` pointing to
an initialized test prefix and `STEAM_COMPAT_CLIENT_INSTALL_PATH` pointing to Steam.
Use a separate prefix from any running game. The executable prints a PASS for
each stage and returns nonzero on failure.

With Microsoft's x64 `d3dcompiler_47.dll` beside the test executable, run
`shader_compat_test.exe --compare-legacy` (under Wine/Proton, select it with
`WINEDLLOVERRIDES="d3dcompiler_47=n"`). This also requires the normalized shaders
to produce byte-identical DXBC to the legacy literals on that compiler.

Background: [BLACK issue #35](https://github.com/TGP482/BLACK-XBERecomp/issues/35).
