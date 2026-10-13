# Non-executable cache-file probes

The reported KWin call is `access("/.cache/ksvg-elements", X_OK)`, because
its mode argument is 1. It asks for execute permission, not read or write
permission. A regular cache file without any execute bits should produce
EACCES, including for root. Root's HOME=/ and XDG_CACHE_HOME=/.cache are
intentional in the current session configuration.

Qt's Unix file-metadata implementation separately asks access() for R_OK,
W_OK and X_OK. It treats EACCES as a negative permission answer rather than
an error preventing metadata collection:
https://github.com/qt/qtbase/blob/dev/src/corelib/io/qfilesystemengine_unix.cpp

This matches the supplied call; the log alone does not establish the cache
file's type/mode or identify its exact upstream call site. The rebuilt kernel
classifies only confirmed cases using the inode it already resolved:

- Access mode must be exactly X_OK, the resolved node must be a regular file,
  and no owner/group/other execute bit may be set.
- The access handler still returns EACCES and releases the same references.
- A per-thread result marker lets SYSDIAG omit the immediate severe warning,
  while retaining the call and its count in the error table. The diagnostic
  recorder consumes the marker and also checks syscall number/mode/error.
- Directory search denials, read/write or combined permission checks, files
  with execute bits whose caller is denied, and actual open/exec failures
  continue to warn. A real denial sharing a previous probe's aggregate key
  promotes that entry to severe; a later probe cannot downgrade it.

No filesystem permissions, ownership, cache paths, or access return values
are changed. This repairs diagnostic classification, not a confirmed cache
read/write failure. If the supplied path is actually a directory or has
execute bits, its warning intentionally remains for investigation.

Verification uses actual-source ASan/UBSan harnesses:

```sh
python3 scripts/tests/test_access_probe.py
cc -std=gnu11 -O1 -g -fsanitize=address,undefined \
  -ffunction-sections -fdata-sections \
  -I kernel/src -I kernel/src/include -I kernel/linuxkpi/include \
  -I kernel/limine-protocol/include scripts/tests/test_xattr_probe.c \
  -Wl,--gc-sections -o /tmp/avory-access-diag
ASAN_OPTIONS=detect_leaks=0 /tmp/avory-access-diag
python3 scripts/tests/test_join_diagnostics.py
```

The kernel and production ISO rebuild successfully. KWin runtime behavior
has not been retested in the guest. If a warning persists, capture
`ls -ld /.cache /.cache/ksvg-elements` and `id` in the same desktop session
alongside any failing open/write calls.

## SDDM Breeze theme writability query

The SDDM trace requests access(W_OK), mode 2, for
/usr/share/icons/breeze/index.theme. The staged package file is mode 0644;
the greeter should be able to read it but cannot write it. Qt's metadata
routine above performs W_OK queries and treats EACCES as a negative answer.
The trace is consistent with that metadata query, although its exact call
site has not been confirmed in the guest.

SYSDIAG now quietly counts only access(21) returning EACCES with mode exactly
W_OK, comm exactly sddm-greeter-qt, and that exact theme pathname. The path is
copied using bounded fault-safe strncpy_from_user before any diagnostic lock.
Unreadable or truncated names cannot qualify. Other paths, callers, permission
modes, and actual open/write failures retain their warning behavior, including
severity promotion when an aggregate previously contained this quiet probe.
The kernel still returns EACCES. File permissions and ownership are unchanged.

The actual-recorder sanitizer test verifies quiet counting, severity promotion,
wrong paths/modes/callers, faulty pointers and continued actual-open warnings.
The access-handler sanitizer test also verifies unchanged permission semantics.
Boot the rebuilt ISO using the current disk; no disk recreation is necessary.
SDDM runtime startup has not been retested in the guest.
