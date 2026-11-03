# Third-party notices

The release zip contains compiled code from these projects. Their license texts are in
[`licenses/`](licenses/).

| Component | Used for | License | Text |
|---|---|---|---|
| [dynarmic](https://github.com/dfdx047/dynarmic) (fork of merryhime/dynarmic) | ARM32 JIT inside `prebuilt/arm64-v8a/libunity.so` | 0BSD | [licenses/dynarmic.txt](licenses/dynarmic.txt) |
| [mcl](https://github.com/azahar-emu/mcl) | dynarmic support library | MIT | [licenses/mcl.txt](licenses/mcl.txt) |
| [oaknut](https://github.com/merryhime/oaknut) | dynarmic's ARM64 code emitter | MIT | [licenses/oaknut.txt](licenses/oaknut.txt) |
| [{fmt}](https://github.com/fmtlib/fmt) | dynarmic formatting | MIT | [licenses/fmt.txt](licenses/fmt.txt) |
| [robin-map](https://github.com/Tessil/robin-map) | dynarmic hash maps | MIT | [licenses/robin-map.txt](licenses/robin-map.txt) |
| [Boost](https://www.boost.org/) (headers) | dynarmic | BSL-1.0 | [licenses/boost.txt](licenses/boost.txt) |
| [Mono.Cecil](https://github.com/jbevain/cecil) | `prebuilt/PatchAssemblies` | MIT | [licenses/mono-cecil.txt](licenses/mono-cecil.txt) |
| [.NET runtime](https://github.com/dotnet/runtime) | `prebuilt/PatchAssemblies` (self-contained) | MIT | [licenses/dotnet-runtime.txt](licenses/dotnet-runtime.txt) |

Used by the patcher but not included (fetched by `uv` or installed with `pip`):
[UnityPy](https://github.com/K0lb3/UnityPy) (MIT) and [fontTools](https://github.com/fonttools/fonttools) (MIT).

The banner and logo in `assets/` are original artwork for this project. Their text is set in
[Cinzel](https://github.com/NDISCOVER/Cinzel), SIL Open Font License 1.1
([licenses/cinzel-ofl.txt](licenses/cinzel-ofl.txt)).

Ravensword: Shadowlands is © Crescent Moon Games. No part of the game is included here or in the
releases.
