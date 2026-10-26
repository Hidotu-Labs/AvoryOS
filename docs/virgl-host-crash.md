# Indexed draw crashing the host NVIDIA driver

The captured `make run` core (QEMU 11.1.0, virglrenderer 1.3.0, NVIDIA
610.57.04) crashed inside `glDrawElementsInstancedARB`, called by
`vrend_draw_vbo`. The driver attempted to copy 12 bytes from address zero.
The matching QEMU, libc and NVIDIA binaries from the package cache and matching
virglrenderer debug symbols were used to unwind the core.

The captured command stream contains `SET_VERTEX_BUFFERS` with zero bindings,
followed immediately by an indexed instanced triangle draw (498 indices, one
instance). The shader and vertex-element layout still expect vertex inputs.
The index buffer exists and its byte range fits; the vertex buffers are missing.
This sends enabled vertex attributes with no backing buffers to the host driver.
The reason the guest produced that invalid binding sequence is not established.

The local renderer patch checks the vertex bindings needed by the selected
vertex shader before binding draw objects. A missing or out-of-range binding
skips that draw. It does not reuse an old buffer or fabricate vertex data. The
context remains usable: a subsequent valid binding and indexed draw can render.
Warnings are limited to one per subcontext. Bufferless vertex-ID shaders, unused
vertex attributes, interleaved attributes and sparse valid bindings are allowed.

The guard uses the vertex shader's attribute-input mask rather than assuming
its inputs occupy the first `num_inputs` elements. Sparse inputs could make the
earlier guard reject valid geometry because an unused lower element was
unbound, or overlook a missing active higher element. VBO updates also retain
the highest retired binding until GL consumes the update: several count
reductions before a draw must not leave a deleted buffer bound on the host.
The build script upgrades the earlier guard using its checked reverse patch
without resetting the source checkout. These binding regressions pass; the
Minecraft allocation-limit verification is described below.

The later Minecraft investigation reproduced GPU allocation failures from the
native guest's 4095-live-handle ceiling, followed by the missing-buffer guard.
The enlarged guest table passed a 6000-resource ABI test and Minecraft rendered
a stress scene using over 5900 live resources. See
[minecraft-rendering-2026-10-26.md](minecraft-rendering-2026-10-26.md).
The normal host vertex-binding path is enabled by default. The earlier legacy
path is retained only as `VIRGL_LEGACY_VERTEX_BINDING=1` for comparison, including
its sparse input and constant-attribute offset corrections.

`make run` builds upstream virglrenderer 1.3.0 at pinned commit
`ca50e008863837e094747a69974dde3ae148aeaa`, applies
[scripts/virgl/missing-vertex-buffer.patch](../scripts/virgl/missing-vertex-buffer.patch),
and selects `build/virgl-host/lib` through `LD_LIBRARY_PATH` for QEMU. This is a
host fix; it does not require changing the guest disk or installing replacement
system graphics packages. The library is replaced atomically on rebuild so an
already-running VM can keep mapping the prior inode.

The first build needs Git/network access to upstream, a C compiler, Meson,
Ninja, pkg-config, Python/PyYAML, and development files for libepoxy, EGL/GLX,
GBM, libdrm and X11. Run `make virgl-host` to build it separately. Subsequent
`make run` launches reuse the library until the build script or patch changes.
`RUN_VIRGL=0` bypasses the host renderer. `RUN_VIRGL_HOST_FIX=0` explicitly uses
the system renderer for debugging and does not contain this crash guard.

## Regression evidence

The real host-GPU reproducer initializes vertex and fragment shaders, draws
valid geometry, removes the vertex bindings and submits an indexed instanced
draw, then restores bindings and verifies valid geometry by pixel readback.
On the RTX 4060 Ti host:

- The unpatched renderer terminates with SIGSEGV at the missing-buffer draw.
- The guarded renderer skips that draw, renders the following valid indexed
  draw and passes pixel readback.
- Source-backed guard tests preserve vertex-ID-only shaders, interleaved and
  sparse valid bindings and unused attributes; warnings remain bounded.
- A private full-desktop SDL VM with the guarded library passed 600 animated
  frames, sustained mouse movement/clicks, resizing/fullscreen transitions and
  live RGB pixel checks without a QEMU crash.

Run:

```sh
make virgl-host
python3 scripts/tests/test_virgl_host_vertex_validation.py
python3 scripts/tests/test_virgl_host_binding_state.py
python3 scripts/tests/test_virgl_host_vertex_guard.py
LD_LIBRARY_PATH="$PWD/build/virgl-host/lib:${LD_LIBRARY_PATH:-}" \
  python3 scripts/tests/test_virgl_gtk_boot.py --full-rootfs --pointer=usb-mouse --stress
```

The negative-control option `--expect-crash --library-dir=...` requires an
unpatched renderer and the affected host driver. It disables core dumps for the
test process. This reproduces and prevents the specific captured crash; it is
not a claim that every possible QEMU/NVIDIA crash has been eliminated.

Source: [upstream virglrenderer 1.3.0](https://gitlab.freedesktop.org/virgl/virglrenderer/-/tree/1.3.0).
