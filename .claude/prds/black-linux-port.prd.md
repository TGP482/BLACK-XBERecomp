# Black XBE Recomp — Linux Port

## Problem

This project only builds and runs on 64-bit Windows. Its toolchain is bound to MSVC/Clang-for-Windows, its renderer targets Direct3D 12, its audio targets XAudio2, and its window, input, cursor and settings-overlay code target Win32. Linux players therefore cannot run the port at all, and the port cannot be offered to the upstream project, whose own Linux harness explicitly expects a game repository to supply its own non-Windows host.

The cost of leaving this unsolved is that a working recompilation of an Xbox game stays locked to one operating system, and every future feature must be written twice.

## Evidence

- Measured in this repository: the 21,000-line host layer includes the Win32 header in 11 of its source files; the CMake presets activate only when the host system is Windows; the link line binds Direct3D 12, DXGI, XAudio2, Ole32, WinMM, DWM, Winsock and dbghelp; the UI layer ships only the Win32 and Direct3D 12 backends.
- Measured in the upstream runtime: the Windows surface is confined to guest memory allocation, guest thread creation, the application shell (window, message loop, splash, settings, handover, crash dumps), the Direct3D 12 renderer and the XAudio2 output. The recompiler, the CPU dispatch layer and the console API layer contain no Windows dependency.
- Upstream ships a headless Linux harness that builds the recompiled game code and the runtime with clang++ on Linux, using a small compatibility header for the handful of Win32 names the portable sources touch. It also documents the intended pattern: the game repository supplies stand-ins for its own Windows-only sources.
- The renderer is reachable only through a narrow host interface, and the console-side Direct3D 8 shim plus the shader and state translation are platform-neutral, so the GPU implementation can be replaced without touching recompiled game code.
- The USA release assets are present in this repository, so every milestone can be verified end-to-end rather than argued about.

## Users

- **Primary**: a developer or player running the recompiled port on a 64-bit Linux desktop under X11 or Wayland, on hardware or handheld-class GPUs. Trigger: they own the game and want the port to run on their Linux machine with the same features the Windows build offers.
- **Secondary**: upstream maintainers and contributors, who need a Linux-capable game repository to exist before the upstream runtime can support Linux hosts.
- **Not for**: users without their own copy of the game (assets are not distributable), macOS or ARM64 Linux targets, and anyone wanting features the Windows build does not have.

## Hypothesis

We believe a native Linux host and renderer built on the existing recompilation runtime will make this port fully playable on Linux without a Windows compatibility layer. We will know we are right when a Linux build, built from a clean checkout against the unmodified upstream runtime, boots the recompiled game from the user's own assets, plays with mouse and keyboard and controller, and exposes every feature the project documents — with a hardware-accelerated frame rate.

## Success Metrics

| Metric | Target | How measured |
|---|---|---|
| Build from clean checkout | Succeeds with upstream runtime unmodified, no Windows toolchain, no vendored patches | CI job on a clean Linux image |
| Boot to first frame | Under 60 s on the reference machine, excluding the one-time code recompile | Wall clock against run log timestamps |
| Rendered frame rate | At least 60 FPS at 1280x720 on the reference GPU on the native renderer | In-game frame-rate counter |
| Feature parity | 100% of the documented features functional on Linux | Per-feature checklist with a screenshot or capture per feature |
| Input parity | Mouse and keyboard, controller and gyro aiming all usable; rebinding persists | Manual test matrix on both display servers |
| Upstream integrity | Zero patches to the upstream runtime; replacements live in this repository | Repository diff against upstream runtime sources |
| Regression safety | The Windows build keeps working from the same tree | Existing Windows build script still produces a running binary |

## Scope

**MVP** — a Linux build that boots the recompiled game and is playable: a window, a rendered scene, mouse and keyboard input, sound, the in-game options menu, and save handling. Rendering reaches this milestone on the CPU path (reusing the upstream harness rasterizer) so the host, input, audio and game logic are proven before any GPU work starts. The native GPU renderer and the full feature-parity list are the following milestones, not part of the MVP.

**Out of scope**

- Distributing or acquiring game assets — the user supplies their own legally acquired copy, as today.
- macOS and ARM64 targets, including Apple Silicon.
- Changes to the recompiler, to the generated game code, or to the upstream runtime — the runtime is consumed unmodified and every platform-specific piece is replaced in this repository.
- Any feature that the Windows build does not have — parity only, no new graphics options or gameplay features.
- Co-op and system-link features beyond the behaviour upstream already provides.
- Distribution packaging (Flatpak, AppImage, Deck builds) — build instructions for a native install come first; packaging follows only if asked.
- Wine or any other Windows compatibility layer as a shipping path.

## Delivery Milestones

<!-- Business outcomes, not engineering tasks. /plan turns each into a plan. -->
<!-- Status: pending | in-progress | complete -->

| # | Milestone | Outcome | Status | Plan |
|---|---|---|---|---|
| 1 | Linux toolchain green | The project configures and builds on Linux from a clean checkout against the unmodified upstream runtime, and the binary starts and reports a clean run | complete | `~/.opencode/plan/2026-10-21-linux-toolchain-green.md` |
| 2 | Booting on Linux | The recompiled game starts from the user's own assets and reaches its first frame in a window, on the CPU rendering path | complete | `~/.opencode/plan/2026-10-21-linux-window-cpu-renderer.md` |
| 3 | Playable core | Mouse and keyboard, sound, the in-game options menu and save handling all work, and the game is playable end to end | pending | — |
| 4 | Native GPU renderer | Hardware-accelerated rendering replaces the CPU path at full frame rate, drawing the same scene as the Windows build | pending | — |
| 5 | Feature parity | Every documented feature works on Linux: display modes and monitor selection, resolution and render scale, field of view, frame cap, V-Sync, HDR output, frame-rate counter, FMV scaling, MSAA, SMAA, anisotropic filtering, draw distance, ambient occlusion, reload blur, controller support, gyro aiming, rebinding, skip intro, pause on focus loss, custom cursor and the settings overlay | pending | — |
| 6 | Packaging and documentation | A one-command Linux build flow, documented system requirements and dependencies, and Linux build instructions in the project README | pending | — |

## Open Questions

- [x] Does the upstream CPU rasterizer cover the shader features this title actually uses (fixed-function pixel combiners, shadow maps, depth bias, alpha-tested foliage)? Validation: run the title on the CPU path and capture frames for comparison against the Windows build. Answered in milestone 2: the rasterizer draws the title's geometry (about 14 FPS at 640x480) but its picture is far darker than the Windows build's, so the title's textured and shaded content is not covered. Two host-side gaps had to be closed for the rasterizer to draw at all: the SDK's vertex path has to run in its converted mode (`XBE_RAW_VERTICES=0`, as the upstream harness builds it) and the console's RHW-space `oPos.w` has to carry the 1 the hardware reads it as. Milestone 4's native renderer is where the picture is closed.
- [ ] Which shader translation path carries the console's generated HLSL into SPIR-V with full coverage — an HLSL-capable offline compiler or a DXC-based one? Validation: compile the full shader corpus the title generates with each candidate and record failures.
- [ ] Can the CPU path and the native renderer share the same host, or does the CPU path need its own presentation route? Validation: prototype both behind the same host interface.
- [ ] Can HDR output be delivered on Linux at all, given it is compositor-dependent on both X11 and Wayland? Validation: test against a HDR-capable display under each compositor; if not deliverable, document SDR behaviour as the parity definition.
- [ ] Does the audio path need surround output, or is stereo sufficient on Linux? Validation: confirm what the reference Linux audio stack can present and what the game exposes.
- [ ] Should the Linux and Windows builds be produced by one CMake configuration or separate ones? Validation: build both from the same tree with the documented commands and compare.
- [ ] Should the portable host be proposed upstream as part of the runtime, which would change the "runtime consumed unmodified" decision? Validation: raise with the upstream maintainer before milestone 4.

## Risks

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| Shader translation gaps break the native renderer for parts of the title | Medium | High | Prove the translation path early against the full shader corpus; keep the CPU path as a working fallback; compare captured frames with the Windows build |
| The CPU rasterizer does not cover the title's shader features, shrinking milestone 3 | Medium | Medium | Validate coverage before committing milestone 3 scope; if coverage is partial, state the reduced-fidelity limits in the milestone outcome |
| Guest thread and calling-convention assumptions behave differently under the Linux ABI | Low | High | The upstream harness already runs recompiled code on Linux; a successful milestone 2 retires this risk |
| Upstream runtime updates break the local replacements | Medium | Medium | Pin the upstream runtime to a known revision for the port and keep the replacement surface small and explicit |
| The parity list is long enough to stall the whole port | High | Medium | Milestone 3 ships a playable build before parity work starts, so each parity feature is a separately shippable increment |
| Third-party shader tooling differs across distributions or is unavailable offline | Medium | Medium | Prefer distribution-packaged tools, pin versions in the documented build steps, and verify in CI on a clean image |
| Game assets cannot be shared with contributors, so others cannot reproduce results | Certain | Medium | State in the documentation that assets come from the user's own copy, and have CI verify the build without assets |

---

*Status: DRAFT — requirements only. Implementation planning pending via /plan.*
