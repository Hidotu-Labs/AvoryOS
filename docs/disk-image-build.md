# Disk image creation

`make disk-rebuild` creates a pristine guest disk. After a successful full
build, it keeps a separate read-only image at `build/arch/disk-pristine.img`.
On later rebuilds, matching inputs allow the builder to clone that image
and recreate the incremental partition cache without populating every file.
Guest writes and incremental executable updates never modify the pristine
build cache.

The cache key covers the rootfs, staged firmware, Quake II files, configuration
files, declared image prerequisites, direct `debugfs write` payloads, app
executables, image size, Xorg profile, and image exclusion settings. It records
inode/device identity, modes, sizes, nanosecond modification/change times, and
symlink targets. A same-size edit with a restored modification time still
invalidates the key through its change time. Input changes during a restore
prevent installing that cached disk.

Cold population creates directories and copies files below 1 MiB with a
128-block libext2fs I/O buffer, then streams larger files with the original
8-block buffer. It restores directory timestamps after both phases. Every
cold build still formats a new filesystem and copies the complete selected
file tree. The full 178,296-file/link rootfs measured 62.2 seconds with this
split versus 95.6 seconds with a uniform 8-block buffer in paired fresh-image
tests. Both images passed `e2fsck -f -n`. These timings cover population only;
other image build stages take additional time. Older debugfs versions which do not support this I/O
option automatically use their original behavior. For measurements,
`AVORY_IMAGE_IO_CACHE_BLOCKS=8` selects the original buffer size.

The first build, a changed input, or a missing/corrupt cache uses the full build
path. To explicitly bypass both cache restoration and cache saving:

```sh
make disk-rebuild DISK_CACHE=0
```

`make clean-disk` removes the guest image, incremental cache/manifest, and
pristine cache/metadata. The filesystem remains ext4 with 1 KiB blocks and
the existing kernel-compatible feature flags.

On the development Btrfs volume, a test with a 12 GiB disk and approximately
257,000 input entries restored the pristine image and incremental partition
in about five seconds. This measures reuse of unchanged inputs; cold rootfs
population remains a separate step. Other filesystems use the image-copy
fallback and can take longer.

Regression checks:

```sh
python3 scripts/tests/test_disk_build_cache.py
python3 scripts/tests/test_image_copy.py
python3 scripts/tests/test_image_population.py
python3 scripts/tests/test_image_runtime.py
```

Firmware injection (`scripts/image-add-firmware.py`) also uses range reflinks
for its private partition extraction and finalization, replacing the full
partition `dd` and block-comparison scans. It batches the writes and verifies
every installed payload against the bundle before atomically replacing the
guest image. Its installed-bundle stamp includes disk modification/change
times so guest writes cannot incorrectly hit an old success stamp. On a
private copy of the 12 GiB desktop disk, installation of the real 63.7 MB
Nouveau 570.144 bundle measured 15.9 seconds; an unchanged repeat measured
0.07 seconds. Other filesystems retain the range-copy fallback.
