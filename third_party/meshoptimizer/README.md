# meshoptimizer (subset)

From https://github.com/zeux/meshoptimizer, tag v0.21 (MIT, see LICENSE.md):
`src/meshoptimizer.h`, `src/simplifier.cpp`, `src/allocator.cpp`, unchanged.

OpenTIE-VR (as OpenXWA-VR, where this copy comes from) uses the simplifier at
run time to make light cut-out meshes of the headset runtime's VR controller
models (src/tie_remaster/vr/vr_controllers.c; the `tie_meshopt` library builds
only the simplifier and the allocator).
