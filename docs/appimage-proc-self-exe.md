# AppImage /proc/self/exe lookup repair

The screenshot reports a successful executable launch followed by:

```
Cannot open /proc/self/exe: No such file or directory
Failed to get fs offset for /proc/self/exe
```

The launch command is relative: `..//Downloads/Forkit-1.3.0.AppImage` from `/`.
The downloaded release bytes and executable format are now valid; this failure
occurs when the AppImage runtime reopens itself to locate its bundled SquashFS.

## Cause and repair

`sys_execve` saved its caller-supplied pathname directly into `thread.exe_path`.
Both the readlink syscall shortcut and procfs's exe link return that field.
VFS therefore followed a relative link from `/proc/<pid>/`, rather than from
the directory where exec occurred. Changing cwd after exec also made the saved
relative name unusable.

Exec now prepares a bounded, absolute saved pathname from the exec cwd before
creating the replacement address space. It publishes that name only after
successful loading. This leaves argv and AT_EXECFN unchanged. Dot and dot-dot
components are retained for VFS traversal, because collapsing `link/..`
lexically would change the meaning when `link` is a symlink. Names exceeding
the existing 256-byte saved-path field are rejected before replacing the
address space rather than silently publishing a truncated link.

A second issue affected relative launches from directories other than `/`:
exec's permission/shebang check used cwd, while the ELF loader used global root.
The loader now resolves relative executable and PT_INTERP names against the
current thread's cwd node, with root as the fallback when no cwd is available.
Absolute names retain the normal root/mount-namespace resolution path.

This repair retains the current pathname-based procfs implementation; it does
not add persistent executable-inode handles for rename/unlink cases.

## Verification

- `python3 scripts/tests/test_proc_self_exe.py` runs the actual path snapshot
  helper under UBSan: screenshot spelling, cwd anchoring, absolute names,
  preserved symlink/dot-dot traversal, exact capacity bounds and pre-commit
  placement.
- `python3 scripts/tests/test_elf_loader_placement.py` passes, including ELF32,
  Wine placement, header-table fallback, and the new cwd/root selection checks.
  Its mocks were updated for the loader's path-at API and an obsolete unused
  prefetch stub was removed.
- `python3 scripts/tests/test_proc_self_exe_boot.py` passes on a private QEMU
  disk: relative exec from `/bin`, absolute readlink output, reopening the same
  executable inode after chdir to `/`, unchanged link after a failed exec,
  and successful exec through `/proc/self/exe` without recursion.
- `AVORY_TEST_APPIMAGE=/tmp/avory-Forkit-1.3.0.AppImage python3
  scripts/tests/test_appimage_exec_boot.py` passes using the checksum-verified
  Forkit release. It retains the full 117,723,467-byte append/write/readback
  regression, then runs `--appimage-offset` via an absolute path, the exact
  screenshot relative path, and `./Forkit-1.3.0.AppImage` from `/Downloads`.
  Every run exits successfully and reports offset `188392`.
- Before the repair, the same guest test prints the screenshot's exact
  `/proc/self/exe` errors for the root-relative launch, and the launch from
  `/Downloads` fails during ELF loading. Logs are retained under
  `build/appimage-exec-boot/serial-before-exe-path.log` and
  `build/appimage-exec-boot/serial-after-exe-path.log`.

Kernel and regular boot ISO builds pass. Boot the rebuilt ISO to use the fix;
the correctly downloaded AppImage can be reused. These checks validate self
lookup and the runtime's filesystem-offset operation, not a full Electron GUI
launch or FUSE mounting.
