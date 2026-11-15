# Third-party notices: macOS build

What `tools/macos/package.sh` puts in `dist/`, and where each licence text sits on this machine. Paths were checked for
existence on macOS 26 / Homebrew at `/opt/homebrew`; `brew --prefix <formula>` prints the same folders on another install.

| Component | Licence | Licence text (verified present) | Reaches the app as |
| --- | --- | --- | --- |
| SDL3 | zlib | `$(brew --prefix sdl3)/LICENSE.txt` | `lib/libSDL3.0.dylib`, copied from `$(brew --prefix sdl3)/lib` |
| vulkan-loader | Apache-2.0 | `$(brew --prefix vulkan-loader)/LICENSE.txt` | `lib/libvulkan.1.dylib` |
| MoltenVK | Apache-2.0 | `$(brew --prefix molten-vk)/LICENSE` | not linked: the loader dlopens it from the ICD json in `/opt/homebrew/etc/vulkan/icd.d`, so it stays outside `dist/` |
| Dear ImGui | MIT | `vendor/crownpark-full/third_party/imgui-src/LICENSE.txt` | compiled in (`_deps/imgui-src`) |
| toml++ | MIT | `vendor/crownpark-full/third_party/tomlplusplus-src/LICENSE` | header-only, compiled in |
| Vulkan Memory Allocator | MIT | `vendor/crownpark-full/third_party/vma-src/LICENSE.txt` | header-only, compiled in |
| Zydis / Zycore | MIT | `vendor/crownpark-full/third_party/zydis-src/LICENSE`, `.../zycore/LICENSE` | build tools only (`xbe_recomp`, `xbe_symbols`), not shipped |
| glslang | BSD-3-Clause | `$(brew --prefix glslang)/LICENSE.txt` | authoring only (`tools/macos/hlsl2spv.sh` calls `glslangValidator`) |

glslang appears nowhere in `build.ninja`; the shipped executable links only SDL3, vulkan-loader and libSystem.

## CrownPark-derived files

Sources taken from `vendor/crownpark-full` carry no licence grant of their own. `THIRD_PARTY.md` says:

> No new blanket license is assigned to the combined port snapshots.

and, above the table,

> This import does not relicense individual components or claim upstream work.

Those files must not be redistributed until the licence question is settled; the notices above cover only the components
listed, not the port sources themselves.
