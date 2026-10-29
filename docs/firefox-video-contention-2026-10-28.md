# Firefox 1080p video and graphics contention

The user reports slowdown when opening a 1080p YouTube video. The tests below
exercise graphical Firefox with VP9/Opus, audio, streaming buffers and KWin
Wayland. They do not establish reproduction of the original YouTube page's
lasting slowdown.

## Repair

VirGL serializes context/resource ioctls through `ioctl_owner`. A holder can
sleep for host graphics work, but contenders previously remained runnable,
repeatedly exchanging the ownership word and calling `sched_yield()`. Browser
uploads and compositor activity therefore consumed CPU and scheduler work
while waiting for the same gate.

Contenders now register on a wait queue, publish their blocked state and
deadline with interrupts masked, and recheck ownership before parking. Release
wakes a waiter. The post-registration check handles an unlock that happened
before registration. Cleanup removes the stack-resident entry and restores
the caller's interrupt state; signals can interrupt acquisition before any
ioctl mutation. A one-second safety deadline bounds a missed notification.

The existing BO-WAIT path still retains its BO and releases the metadata gate
before waiting for its fence. Context/resource serialization and fence ordering
are unchanged. No codec, power-management, renderer or sandbox setting in the
installed desktop is changed by this repair.

## Tests

The actual-source gate regression covers uncontended acquisition, release
before registration, spurious wake/re-registration, pending signals, and eight
concurrent clients waiting behind an owner. It checks exclusive ownership,
balanced queue cleanup and interrupt restoration. ASan/UBSan pass with leak
detection disabled. The saved prior implementation fails because its yield
occurs while the client is still runnable instead of blocked.

Existing tests for BO-WAIT concurrency, exec/fence ownership and queued render
dependencies pass. Kernel compilation/link succeeds with the existing RWX
LOAD-segment warning.

Private KVM playback uses four vCPUs, virtual HDA with the null host audio
backend, and virtual graphics. The VirGL cases use SDL offscreen EGL and the
repository's host renderer library. Host hardware decoding and audible output
are not verified. The media is a generated 8-second VP9/Opus test clip, looped
or repeatedly appended through MediaSource. Streaming evicts older buffers.
Frame counts, dropped frames, media time, screenshot captures, CPU accounting,
TLB/DRM statistics and a separate 10 ms sleep monitor are recorded.

Before the gate repair:

- 720p30 with Xorg/IceWM, VirGL and audio passed; steady-state monitor maxima
  were 11–15 ms.
- 1080p60 with the same stack passed; after startup the monitor maxima were
  12–14 ms.
- 1080p60 MediaSource streaming with 4 GiB RAM passed; steady-state monitor
  maxima were 13–15 ms.
- The streaming case with 1536 MiB RAM also passed. Swap was exercised and
  frame drops increased; steady-state wake maxima were 13–24 ms.
- KWin Wayland streaming passed. After startup the monitor maxima were
  13–17 ms. At approximately 60 seconds the browser reported 3486 frames,
  349 dropped frames and 58.1 seconds of media progression.

After the gate repair, KWin Wayland streaming also passed: about 60 seconds
into playback it reported 3508 frames, 292 dropped frames and 58.5 seconds of
media progression. The full run had 13–19 ms monitor maxima after startup and
a 17 ms post-exit maximum. This single comparison is not a stable FPS benchmark
or proof of the original site's symptom. The existing VirGL KVM test passed
texture round-trip, fences, PRIME, large command packets and 6000 live resources
on the repaired kernel.

These baselines demonstrate that the local video workload did not reproduce
a persistent whole-system slowdown. The gate regression independently proves
the unnecessary runnable retry behavior; playback measurements are validation,
not proof that every reported YouTube slowdown shares that cause.

Evidence, generated media and comparison kernel/source are under
`build/firefox-video/`. Each guest uses a separate raw filesystem copy. The
first test used the preceding offline browser fixture; after that fixture was
removed, subsequent tests used an offline copy of the cached desktop partition.
The running user's disk and VM were not modified or stopped.

The verified kernel is installed in `avoryos-x86_64.iso`. Production boot
configuration was preserved and the extracted kernel matched the intended
stripped ELF before atomic replacement. Rollback ISO:
`build/firefox-video/iso-before-video.iso`; installed hash:
`build/firefox-video/installed-iso.sha256`. Restart the VM to load the change.

## Reproduce

```sh
python3 scripts/tests/test_virgl_gate_sleep.py
python3 scripts/tests/test_virgl_wait_concurrency.py
python3 scripts/tests/test_virgl_exec_fence.py
python3 scripts/tests/test_virgl_render_queue.py

ffmpeg -f lavfi -i testsrc2=size=1920x1080:rate=60 \
  -f lavfi -i sine=frequency=440:sample_rate=48000 -t 8 \
  -c:v libvpx-vp9 -deadline realtime -cpu-used 8 -row-mt 1 -threads 2 \
  -b:v 4000k -c:a libopus build/firefox-video/1080p60.webm

AVORY_TEST_DESKTOP_FS=build/firefox-video/base.img \
  python3 scripts/tests/test_firefox_video_boot.py --virgl --audio --mse \
  --wayland --clip build/firefox-video/1080p60.webm \
  --name firefox-video-kwin-fixed
```

The optional seed variable names an **offline raw filesystem**, not a GPT disk.
Without it the fixture clones the production disk's first partition, and
refuses to do so if QEMU has that disk open.
