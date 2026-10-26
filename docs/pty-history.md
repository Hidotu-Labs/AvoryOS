# Bash history recall through a PTY

If Up moves the cursor into earlier output, the command is invisible, and
Enter still executes it, inspect the PTY's line discipline before changing
Readline prompts or terminal escape handling.

The staged glibc uses `TCGETS2` and `TCSETSW2` for `tcgetattr` and
`tcsetattr`. Previously, PTYs implemented only the legacy `TCGETS` and
`TCSETS*` requests. The extended requests fell through to the generic
console handler in `sys_ioctl`, which returned success while accessing
`console_termios` instead of the pair's settings. The PTY remained in
canonical mode with echo enabled. It echoed the literal Up sequence
(`ESC [ A`) to the emulator, moving its cursor, and held the sequence until
Enter. Bash then recalled and executed history after the newline arrived.

Both PTY endpoints now handle the legacy and extended requests using the
same pair-local attributes, including the extended speed fields. The wire
payloads remain 36 bytes for legacy termios and 44 bytes for termios2.

Run the native regression with:

```sh
python scripts/tests/test_pty_history_boot.py
```

It boots the current kernel in a private QEMU guest using software emulation
(`AVORY_TEST_ACCEL=kvm` optionally selects KVM). It requires the existing
boot ISO, staged Arch rootfs, and musl compiler. It does not open `disk.img`.
The test covers both endpoints, all six setters, ABI output sizes, pair
isolation, raw key delivery without echo, and actual glibc Bash history
recall, Down, resize, and execution with plain and colored prompts.
