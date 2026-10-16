# VLC launcher

`vlc` and the existing desktop shortcut use `config/vlc/vlc.sh`. The wrapper
sets `/usr/lib/vlc/plugins` as the first plugin directory, defaults `DISPLAY`
to `:0` when absent or empty, and selects Qt. Arguments are forwarded intact;
an explicit interface such as `cvlc`'s `-I dummy` overrides the default.

`scripts/setup-arch.sh` installs the wrapper during package setup, before
the staged rootfs is copied into an image. It preserves the packaged launcher as
`/usr/bin/vlc.distrib` and creates `/usr/bin/vlc.bin` with only the launcher's
root-refusal branch removed. VLC consequently runs with the calling user's
actual credentials, including root. The patch is guarded by the original
binary's SHA-256; a different VLC package requires inspecting and updating
the patch before building an image. Re-running the installer is supported.

The historical `make setup-alpine` target runs `scripts/setup-arch.sh` before
marking the rootfs ready. Changes to the setup script, package mapping,
account setup, or VLC wrapper invalidate that stamp, so subsequent image
builds pick up those changes.

Normal QEMU launch targets (including `make run-c7-prime`) reuse an existing
`disk.img` without visiting its rebuild dependencies. Run `make disk.img`
explicitly to refresh the disk; launch targets build one if it is missing.

The existing `disk.img` was patched directly at its root partition offset
(1048576 bytes), using libext2fs to install just these three files. No image
rebuild was performed. A copy-on-write pre-patch backup is saved at
`build/disk-before-vlc-wrapper.img`. Restore it only with the VM stopped.

Validation: image files match the staged files byte for byte; a user-namespace
chroot with UID 0 runs `vlc --version` and `vlc --list --no-plugins-cache`
successfully and discovers the Qt plugin. Graphical playback still needs
verification inside the running guest.

## H.264 video decoding

Arch's `vlc-plugin-ffmpeg` package is required in addition to the FFmpeg
libraries. `setup-arch.sh` installs it explicitly; without its
`codec/libavcodec_plugin.so`, MP4 audio can play while VLC reports that H.264
is unsupported. The matching `3.0.23_2-16` plugin package was installed into
the staged rootfs and its plugin cache regenerated. A UID-0 chroot test of
`assets/video.mp4` with software decoding and dummy video output successfully
selected `avcodec` and decoded H.264 frames. This checks decoding without
depending on the guest's graphics drivers.

The existing `disk.img` also has the eight plugin files, updated
`plugins.dat`, and package database record installed in place. All installed
files were read back and matched against the staging tree. Its pre-codec
backup is `build/disk-before-vlc-codecs.img`.
