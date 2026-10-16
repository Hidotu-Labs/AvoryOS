# SDDM greeter crash while loading a corrupt Qt pipeline cache

The supplied fault is in `QSGRenderThread`, tgid 179, running
`/usr/bin/sddm-greeter-qt6`. The installed libc disassembly identifies offset
`0x18f632` as a load in `__memcpy_avx512_unaligned_erms`. The caller's return
offset, `libQt6Gui.so.6.11.2+0x5dca5e`, follows the key-data copy in
`QRhiGles2::setPipelineCacheData`. The installed binary reads a 32-bit length
and copies that many bytes without checking the remaining cache extent.

This matches the [Qt 6.11.2 loader source](https://github.com/qt/qtbase/blob/v6.11.2/src/gui/rhi/qrhigles2.cpp#L1710).
Qt Quick [loads the automatic cache through QFile::readAll](https://github.com/qt/qtdeclarative/blob/v6.11.2/src/quick/scenegraph/qsgrhisupport.cpp#L902)
before passing it to this parser. The fault in memcpy is a consequence of
the supplied length; the log does not establish an AVX state defect or a
missing grow-down stack mapping.

## Matching disk evidence

The running VM's `disk.img` was opened read-only, using the existing
`build/wine-window-diagnostic/read-disk.py` ext4 reader with its input path
changed in a temporary copy. No guest files or disk blocks were modified.
The live read is not an atomic filesystem snapshot, but the extracted file
matches the fault's header and offending record length exactly.

Guest path:

```
/var/lib/sddm/.cache/sddm-greeter-qt6/qtpipelinecache-x86_64-little_endian-lp64/qqpc_opengl
```

- Inode: 59967. File length: 124969 bytes.
- SHA-256: `0d84cdc29e8f490ab5054ffcf114c090120f0918e5fddb0edf78af1f6aa5a396`.
- Header: RHI ID `0x02060b02`, pointer width 8, count 13, payload 124713.
  Header plus payload equals the file size, so the outer length check passes.
- Driver string identifies the Ryzen 5 7600 RadeonSI/Mesa 26.2.4 renderer,
  matching the string visible on the faulting stack.
- Records 0 through 7 have lengths that fit inside the payload.
- At file offset `0x13ea4`, record 8's key length is `0xbda8576f`
  (3181926255 bytes). After the length field, only 43393 bytes remain.
  The same invalid length appears in RDX and R15 in the fault.
- The crash's requested mmap size `0xbda86000` is consistent with the
  attempted multi-gigabyte QByteArray allocation. The source address then
  walks outside its mapping during the oversized copy.

The offending cache was preserved locally as
`build/sddm-pipeline-cache-fault/qqpc_opengl.corrupt` for investigation.

## Targeted recovery

Inside AvoryOS, as root, move the single corrupt automatic cache aside before
the next greeter launch:

```sh
cache=/var/lib/sddm/.cache/sddm-greeter-qt6/qtpipelinecache-x86_64-little_endian-lp64/qqpc_opengl
mv -n -- "$cache" "$cache.corrupt-20261016"
```

Qt can rebuild this disposable cache on a later successful run. Other Qt,
Mesa and application caches need not be cleared. The host must not edit this
filesystem through debugfs while QEMU is using it.

This is a recovery for an identified bad cache, not a measured guest fix.
The parser needs record bounds checks for robust handling of malformed blobs;
the [current upstream implementation](https://github.com/qt/qtbase/blob/dev/src/gui/rhi/qrhigles2.cpp)
uses a bounded reader. The initial cause of the corrupted file remains
unproven; this crash alone does not identify a journal, storage, RAM or GPU
write as its origin. No kernel mapping change is justified by this fault.
