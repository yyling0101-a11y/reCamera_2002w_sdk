# Repository build-directory policy

- All CMake builds in this repository must use the single top-level directory
  `build/`.
- Configure with `cmake -S . -B build ...` and build individual examples with
  `cmake --build build --target <target> -j`.
- Do not create per-example or per-feature directories such as `build-foo`,
  `build-example`, `build-debug`, or a nested `examples/*/build` directory.
- When adding an example, add it to the root CMake project and build its target
  from the existing top-level `build/` directory.
- Reconfigure the existing `build/` directory when CMake options change. Only
  remove and recreate `build/` when the user explicitly requests a clean build.
- Documentation and deployment scripts must reference artifacts under `build/`.
