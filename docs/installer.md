# AvoryOS TTY Installer

AvoryOS features a native, curses-inspired TTY installation wizard written in C (`userland/avory-installer.c`). It combines the aesthetics and resilience of **FreeBSD's `bsdinstall`** with the streamlined identity and storage workflow of **Ubuntu Server's `subiquity`**.

![AvoryOS Installer Architecture](assets/installer-preview.png)

---

## Highlights & Features

- **Native C Implementation**:
  - Compiled statically with `musl-libc` (`userland/avory-installer.elf`) with zero dynamic runtime library dependencies.
  - Custom zero-flicker double-buffered ANSI/VT100 TUI engine supporting standard 16/256-color palettes, drop shadows, and clean borders.
  - Dynamically resizes to match any terminal resolution via `TIOCGWINSZ`.

- **Visual Style & User Experience**:
  - **FreeBSD Blue & Silver Theme**: Classic deep blue screen backdrop, crisp dialog cards, bold titles, and intuitive hotkeys.
  - **Ubuntu Server Navigation**: Step-by-step progress indicator (`[Step X/Y]`), single-glance disk and interface tables, and real-time form field validation.
  - Animated installation gauge (`[######------] XX%`) with live subsystem status.

- **Storage & Partitioning Engine**:
  - **Automatic Guided Partitioning**:
    - Automatically discovers candidate NVMe, SATA/AHCI, and VirtIO disks from `/sys/block` and `/proc/partitions`.
    - Automatically identifies the active live media to prevent accidental data loss.
    - Creates a modern **GPT (GUID Partition Table)** layout:
      - **Partition 1 (128 MB)**: EFI System Partition (FAT32, type `EF00`).
      - **Partition 2 (Remaining)**: AvoryOS Root System (ext4, labeled `AvoryOS-Root`, type `8300`).
  - **Dual Bootloader Setup**:
    - Deploys **Limine UEFI** (`EFI/BOOT/BOOTX64.EFI`) onto the ESP.
    - Installs **Limine BIOS MBR** boot records for legacy hardware support.
    - Generates target `/boot/limine/limine.conf` configured for the newly installed disk.

- **System & User Configuration**:
  - Console keyboard layout selection (`us`, `uk`, `de`, `fr`, `es`, `it`, `se`, `no`, `tr`).
  - Network configuration (Automatic DHCP or manual).
  - Component sets (Base system, Desktop suite, Development toolchain, Games & Media).
  - Target user account creation (`useradd` with `wheel`, `audio`, `video`, `input` groups).
  - Root and user password configuration.
  - Default desktop session selection (XFCE 4, KDE Plasma 6, IceWM, or Console).

---

## Launching the Installer

The installer can be launched in multiple ways:

### 1. From Limine Bootloader Menu
Select the boot menu entry:
```
/Install AvoryOS (TTY Installer)
```
This boots directly into the installer wizard.

### 2. At the Console Login Prompt
At the `login:` prompt on `tty1`, type:
```
install
```
or
```
installer
```
(No password required). The system will directly spawn the installer.

### 3. From an Interactive Shell
Run the command directly from any terminal or TTY:
```bash
avory-installer
```
or
```bash
install-avoryos
```

---

## Wizard Navigation & Keybindings

| Key | Action |
| :--- | :--- |
| **`Up` / `Down`** | Move between menu items, disk rows, and options |
| **`Left` / `Right`** | Switch between bottom action buttons (`< Continue >`, `< Back >`) |
| **`Tab` / `Shift-Tab`** | Move focus between list items, text inputs, and action buttons |
| **`Space`** | Toggle checklist options and radio selections |
| **`Enter`** | Confirm selection or activate the highlighted button |
| **`Backspace`** | Edit text input fields |
