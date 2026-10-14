# First-initialization 3D pixel corruption

The user reports bad pixels only in 3D applications (tunnel/Minecraft), on the
first initialization, disappearing on the second or third. The successful
X-close recovery does not resolve this symptom. No pixel fix is claimed here.

## Compared evidence

The retained launches `20261014T202502Z-b6rpn6qk` (user-labelled corrupt) and
`20261014T202702Z-r_qgipj0` (user-labelled clean) share the host boot ID. Both
perform MODE2 successfully, checked PSP stop and ring creation, firmware loading,
and register AMDGPU. Filtered initialization traces match apart from the prior
mailbox value: `80030000` vs `80020000`. Both stop responses are `80030000`.
No meaning beyond the existing handshake masks is inferred. Common BIOS-object
and PM sysfs warnings do not distinguish the reported outcomes.

Bounded comparison: `build/gpu-first-boot-investigation/init-diff.txt`.
The retained launch metadata reports different ISO hashes; this is not proof
of different kernel bytes because the launcher rebuilds ISO containers. No
claim that these two ISO contents are identical is made. These logs contain
no GPU pipeline pixel-oracle output, and cannot distinguish texture/uniform
upload, shader/cache execution, or presentation corruption.

An upstream RadeonSI correction for stale instruction/scalar/vector cache
contents on gfx1036 is a relevant candidate, not an established explanation:
https://chromium.googlesource.com/external/gitlab.freedesktop.org/mesa/mesa/+/57fd11256bdf0ec1ff1908277e4963180252491f
The previously documented simple constants pass does not cover all such
workloads. Do not insert a GPU-specific cache workaround into generic CPU/DMA
barriers or modify imported AMDGPU merely from this report.

## Prepared affected-state capture

`scripts/gpu-first-boot-capture.sh` runs the existing desktop-OpenGL hardware
pipeline oracle twice without restarting the GPU. It retains texture, mip,
UBO/SSBO and CPU-source comparisons, renderer identity, existing debug environment,
and both exit statuses. Default mode preserves the environment. Optional
`disabled` changes the shader disk-cache setting only for the diagnostic child;
this is a diagnostic variable, not a fix. Running the draws may itself warm
hardware state, so check the affected application again afterwards.

Artifacts are built and served from `build/gpu-diagnostics` on loopback9007.
In the affected guest:

```sh
curl -fsS http://10.0.2.2:9007/gpu-first-boot-capture.sh -o /tmp/gpu-first-boot-capture.sh
sh /tmp/gpu-first-boot-capture.sh corrupt
```

Repeat with label `clean` on a clean boot. Reports are retained in a private
`/tmp/gpu-first-boot.*/report.log`, printed, and copied to `/dev/ttyS0` if
writable. Default mode does not delete caches, change game options, trigger
reset, or claim success from software rendering. Exit2 and zero overlap coverage
remain inconclusive. If all oracles match, an affected app capture remains
necessary; the bounded workloads do not cover every game shader.

The existing user QEMU was left running; no physical restart or hardware oracle
was executed during this investigation. Script syntax, diagnostic build, served
file equality, and diff whitespace checks pass. No CPU result is presented as
a hardware pixel fix. Xerj refresh still fails with the retained document family
mismatch, so bounded exact source reads were used.

## Switchable submission visibility candidate

At the user's request to implement a correction rather than stop at capture,
`kernel/linuxkpi/src/amdgpu_ib_visibility.c` wraps `amdgpu_ib_schedule`. The
imported implementation is renamed at build time and remains unchanged. For
1002:164e APU PF passthrough with GC10.3.6, hardware access enabled, a GPUVM job,
and a graphics/compute ring supporting the callback, the wrapper sets the
existing `AMDGPU_IB_FLAG_EMIT_MEM_SYNC` request on the first IB during delegation.
All original flags are restored on success and error. Internal ring tests,
non-GPUVM jobs, SDMA, VFs and other devices are excluded. There is no additional
firmware reset or retry.

This uses `gfx_v10_0_emit_mem_sync` (8 dwords, already included in the imported
frame budget) to write back/invalidate GL2/GLM and invalidate GL1/GLV/GLK/GLI.
It is GPU-specific compatibility policy, not a generic CPU cache/DMA change.
The boot switch `kpi_amdgpu_ib_mem_sync` defaults enabled for this candidate;
set it to 0 to restore original submission requests. Mandatory scope checks
still apply. It does not rewrite userspace command buffers.

The upstream Mesa stale-cache correction motivates this experiment but is not
the same patch: Mesa places invalidations in its IB prologue; this policy
requests the existing kernel ring synchronization before the submitted batch.
Neither logs nor CPU tests prove that their causes or effects are identical.
The added full synchronization can reduce throughput; no FPS improvement or
pixel repair is claimed without affected first-boot hardware validation.

`test_amdgpu_ib_visibility.py` compiles the actual wrapper and imported emitter
under ASan/UBSan. It checks graphics/compute packets, existing budget, scope,
explicit disable, exactly one delegation, flags/error/fence ownership, and
AMD-disabled compilation. The kernel/ISO build passed:
`build/logs/raphael-ib-visibility-build.log`. The linked ELF contains both the
owned wrapper and original renamed scheduler. The existing user VM remains
running an earlier kernel and was not terminated to claim validation.

For an ordinary next boot, use the rebuilt ISO. A first graphics/compute GPUVM
submission should log `Raphael GPU cache sync enabled`. This marker proves
policy use, not correct pixels. Preserve the successful MODE2+PSP recovery
path while evaluating this independent rendering candidate.
