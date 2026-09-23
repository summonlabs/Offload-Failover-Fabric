# Validation

## Toolchain

| Component | Version used |
| --- | --- |
| Compiler | Microsoft C/C++ 19.44.35209 for x64 (Visual Studio 2022 17.14) |
| Build system | CMake 4.3.2 with Ninja 1.13.2 |
| Standard library | MSVC STL, `/std:c++20` |

First-party warnings are `/W4 /permissive- /WX` in every configuration.
Warning flags are applied `PRIVATE` and wrapped in `$<BUILD_INTERFACE:...>`,
so nothing leaks into the exported target.

## Configurations exercised

```
cmake -S . -B build/debug   -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/debug   && ctest --test-dir build/debug   --output-on-failure
cmake --build build/release && ctest --test-dir build/release --output-on-failure
```

Both configurations compile with zero warnings and pass the complete suite.

## Test inventory

| Kind | Where | Notes |
| --- | --- | --- |
| Unit | `tests/test_strong.cpp`, `test_canonical.cpp`, `test_model.cpp`, `test_store.cpp` | Types, codec, digests, persistence format |
| Integration | `tests/test_qualify.cpp`, `test_fence.cpp`, `test_planner.cpp`, `test_stateful.cpp`, `test_effects.cpp`, `test_failback.cpp` | Full decision pipeline |
| End-to-end | `tests/test_restart.cpp` | Real store directories, real reopen, real crash shapes |
| Property / invariant | `tests/test_property.cpp` | Seeded xorshift schedules; invariants checked after every step |
| Adversarial | `tests/test_adversarial.cpp` | Malformed, truncated, oversized, impossible, future-generation input |
| Concurrency | `tests/test_concurrency.cpp` | Serial vs concurrent equivalence, duplicate delivery, cancellation |
| Independent process | `tests/test_process.cpp` | Spawns `offd` and `off` as child processes over real loopback sockets |

There are no timeouts anywhere. Every test synchronises on completed work: a
protocol response frame, a joined thread, EOF, or a condition variable.

Run the suite with a filter to isolate a case:

```
off_tests qualify.
off_process_tests path/to/offd path/to/off process.
```

## Sanitizers

**AddressSanitizer coverage is not available in this environment and is not
claimed.** MSVC's ASan requires the `clang_rt.asan_*` runtime libraries; the
installed toolset provides only `asan_compat.lib`, so linking fails with
`LNK1104: cannot open file 'clang_rt.asan_static_runtime_thunk-x86_64.lib'`.

The build system states this honestly rather than pretending otherwise:

```
cmake -S . -B build/asan -DOFF_ENABLE_ASAN=ON
-- OFF_ENABLE_ASAN was requested but this toolchain has no AddressSanitizer
   runtime; the build continues WITHOUT sanitizer coverage.
```

When the option is requested and the toolchain *does* provide a runtime, a
compile-and-link probe enables it and defines `OFF_SANITIZED=1` for consumers.

In place of sanitizer coverage this repository relies on:

* Debug builds with the MSVC STL iterator and container debugging active.
* Explicit bounds checking in every decoder, validated against ceilings before
  any allocation.
* Deterministic invariant checking in the property suite.
* Adversarial decoding of thousands of random byte strings, asserting that
  anything that decodes re-encodes to exactly the input bytes.

## Benchmarks

`off_bench` measures completed work only: each figure counts operations that
returned an accepted decision and whose effect is observable in the exported
state afterwards. It never measures enqueue latency.

```
off_bench [rounds]
```

## Packaging validation

```
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=<prefix>
cmake --build build/release
cmake --install build/release
cmake -S examples/downstream -B build/downstream \
      -DCMAKE_PREFIX_PATH=<prefix>
cmake --build build/downstream
build/downstream/off_downstream
```

The downstream project uses only `find_package(OffloadFailoverFabric 1.0
REQUIRED CONFIG)`, installed headers and the exported target. It performs a
complete governed recovery and asserts the fence, continuity and export
invariants from outside the build tree.
