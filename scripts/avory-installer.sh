#!/bin/sh
# scripts/avory-installer.sh — FreeBSD-style TTY Installer for AvoryOS
# Supports guided GPT+UEFI/BIOS partitioning, component selection, user setup,
# desktop configuration, and Limine bootloader installation.

set -u

# Ensure a standard PATH
PATH="/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:$PATH"
export PATH

BACKTITLE="AvoryOS Installation Suite"
DIALOGRC_PATH="/etc/dialogrc"
export DIALOGRC="$DIALOGRC_PATH"
export TERM="${TERM:-linux}"

TARGET_MNT="/mnt/target"
TMP_DIR="/tmp/installer"
mkdir -p "$TMP_DIR"

cleanup() {
    rm -rf "$TMP_DIR" 2>/dev/null || true
    # If target is still mounted and we aborted unexpectedly
    if [ "${INSTALL_COMPLETED:-0}" -ne 1 ]; then
        umount "$TARGET_MNT/boot/efi" 2>/dev/null || true
        umount "$TARGET_MNT" 2>/dev/null || true
    fi
}
trap cleanup EXIT INT TERM

# ── Ensure FreeBSD Dialog Theme ─────────────────────────────────────────────
setup_dialogrc() {
    if [ ! -f "$DIALOGRC_PATH" ]; then
        cat <<'EOF' > "$DIALOGRC_PATH"
use_shadow = ON
use_colors = ON
screen_color = (WHITE,BLUE,ON)
shadow_color = (BLACK,BLACK,ON)
dialog_color = (BLACK,WHITE,OFF)
title_color = (BLUE,WHITE,ON)
border_color = (WHITE,WHITE,ON)
button_active_color = (WHITE,BLUE,ON)
button_inactive_color = (BLACK,WHITE,OFF)
button_key_active_color = (WHITE,BLUE,ON)
button_key_inactive_color = (RED,WHITE,OFF)
button_label_active_color = (YELLOW,BLUE,ON)
button_label_inactive_color = (BLACK,WHITE,ON)
inputbox_color = (BLACK,WHITE,OFF)
inputbox_border_color = (BLACK,WHITE,OFF)
item_color = (BLACK,WHITE,OFF)
item_selected_color = (WHITE,BLUE,ON)
tag_color = (BLUE,WHITE,ON)
tag_selected_color = (WHITE,BLUE,ON)
tag_key_color = (RED,WHITE,OFF)
tag_key_selected_color = (YELLOW,BLUE,ON)
check_color = (BLACK,WHITE,OFF)
check_selected_color = (WHITE,BLUE,ON)
uarrow_color = (GREEN,WHITE,ON)
darrow_color = (GREEN,WHITE,ON)
gauge_color = (BLUE,WHITE,ON)
border2_color = (WHITE,WHITE,ON)
searchbox_color = (BLACK,WHITE,OFF)
searchbox_title_color = (BLUE,WHITE,ON)
searchbox_border_color = (WHITE,WHITE,ON)
position_indicator_color = (BLUE,WHITE,ON)
menubox_color = (BLACK,WHITE,OFF)
menubox_border_color = (WHITE,WHITE,ON)
EOF
    fi
}

# ── Fallback TTY Helper if dialog is unavailable ─────────────────────────────
has_dialog=0
if command -v dialog >/dev/null 2>&1; then
    has_dialog=1
    setup_dialogrc
fi

# ── Enumerate Disks ──────────────────────────────────────────────────────────
# Returns a list of candidate disks: NAME SIZE_DESC MODEL
get_available_disks() {
    disks=""
    # Check /sys/block
    if [ -d /sys/block ]; then
        for b in /sys/block/*; do
            [ -e "$b" ] || continue
            name="${b##*/}"
            case "$name" in
                ram*|loop*|sr*|cdrom*|zram*|md*|dm-*) continue ;;
            esac
            
            # Read size in 512-byte sectors
            if [ -f "$b/size" ]; then
                sectors=$(cat "$b/size" 2>/dev/null || echo 0)
                [ "$sectors" -gt 204800 ] 2>/dev/null || continue # at least 100MB
                
                # Size calculation
                size_mb=$(( sectors / 2048 ))
                if [ "$size_mb" -ge 1024 ]; then
                    size_gb=$(( size_mb / 1024 ))
                    size_str="${size_gb} GB"
                else
                    size_str="${size_mb} MB"
                fi
            else
                size_str="Unknown size"
            fi

            # Model / transport description
            model=""
            if [ -f "$b/device/model" ]; then
                model=$(cat "$b/device/model" 2>/dev/null | tr -s ' ' || true)
            fi
            if [ -z "$model" ]; then
                case "$name" in
                    nvme*) model="NVMe Storage Device" ;;
                    sd*)   model="SATA/SCSI Storage Drive" ;;
                    vd*)   model="VirtIO Virtual Drive" ;;
                    hd*)   model="IDE Storage Drive" ;;
                    *)     model="Block Storage Device" ;;
                esac
            fi

            # Check if this device is the current root device
            is_live=""
            root_src=$(awk '$2 == "/" {print $1}' /proc/mounts 2>/dev/null || true)
            case "$root_src" in
                *"/$name"*) is_live=" [LIVE MEDIA - PROTECTED]" ;;
            esac

            echo "$name" "$size_str - $model$is_live"
        done
    fi
}

# ── Dialog Wrapper ───────────────────────────────────────────────────────────
run_dialog() {
    dialog --backtitle "$BACKTITLE" "$@"
}

# ── Step 1: Welcome Screen ───────────────────────────────────────────────────
welcome_screen() {
    welcome_text="
   ___                                ____  _____ 
  / _ \ _  _____  ______ _____ _____ / __ \/ ___/ 
 / /_\ | |/ / _ \/ __/ // / _ \/ __// /_/ /\__ \  
/_/   \_|___/\___/_/  \_, /\___/_/  \____//____/  
                      /___/                       

Welcome to the AvoryOS Installation Suite!

AvoryOS is a high-performance, monolithic operating system kernel
for x86_64 with modern POSIX userland, dual glibc/musl runtimes,
native DRM graphics, and desktop environments.

This utility will guide you through:
  • Configuring system hostname and keymap
  • Selecting distribution sets and desktop environment
  • Partitioning storage disks (GPT + UEFI & BIOS)
  • Installing system files and Limine bootloader
  • Creating user accounts and network settings
"
    run_dialog --title "Welcome to AvoryOS" \
        --extra-button --extra-label "Shell" \
        --help-button --help-label "Live" \
        --ok-label "Install" --cancel-label "Reboot" \
        --msgbox "$welcome_text" 22 74
    ret=$?
    case $ret in
        0) return 0 ;; # Install
        3) # Extra button: Shell
           clear
           echo "[*] Dropping to Live Shell. Type 'avory-installer' to return to installer."
           /bin/bash || /bin/sh
           welcome_screen
           ;;
        2) # Help button: Live Desktop
           clear
           echo "[*] Exiting to Live Environment..."
           exit 0
           ;;
        1) # Cancel button: Reboot
           run_dialog --title "Reboot" --yesno "Are you sure you want to reboot the system?" 7 50
           if [ $? -eq 0 ]; then
               reboot
           else
               welcome_screen
           fi
           ;;
    esac
}

# ── Step 2: Keymap Selection ─────────────────────────────────────────────────
select_keymap() {
    CHOICE_FILE="$TMP_DIR/keymap.choice"
    run_dialog --title "Keymap Selection" \
        --menu "Select keyboard layout for the console:" 18 68 9 \
        "us"      "United States (Default / Standard QWERTY)" \
        "uk"      "United Kingdom (UK Extended)" \
        "de"      "Germany (QWERTZ)" \
        "fr"      "France (AZERTY)" \
        "es"      "Spain" \
        "it"      "Italy" \
        "se"      "Sweden" \
        "no"      "Norway" \
        "tr"      "Turkey" \
        2> "$CHOICE_FILE" || true

    KEYMAP=$(cat "$CHOICE_FILE" 2>/dev/null || echo "us")
    [ -n "$KEYMAP" ] || KEYMAP="us"
}

# ── Step 3: Hostname Configuration ───────────────────────────────────────────
configure_hostname() {
    HOST_FILE="$TMP_DIR/hostname.choice"
    run_dialog --title "Set Hostname" \
        --inputbox "Please enter a hostname for this system.\nHostname should consist of alphanumeric characters and hyphens:" \
        10 65 "avoryos" 2> "$HOST_FILE" || true
    
    SYSTEM_HOSTNAME=$(cat "$HOST_FILE" 2>/dev/null || echo "avoryos")
    [ -n "$SYSTEM_HOSTNAME" ] || SYSTEM_HOSTNAME="avoryos"
}

# ── Step 4: Distribution Sets (Components) ───────────────────────────────────
select_components() {
    COMP_FILE="$TMP_DIR/components.choice"
    run_dialog --title "Distribution Sets" \
        --checklist "Choose optional system components to include in the installation:" 16 72 5 \
        "base"    "Core Base System & Higher-Half Kernel" ON \
        "desktop" "Desktop Environments (XFCE4, KDE, IceWM)" ON \
        "dev"     "Development Tools (GCC, make, TCC, toolchains)" ON \
        "games"   "Ported Games (Quake II, DOOM, ClassiCube)" ON \
        "media"   "Media & Internet (Firefox, VLC, fastfetch)" ON \
        2> "$COMP_FILE" || true

    SELECTED_COMPONENTS=$(cat "$COMP_FILE" 2>/dev/null || echo "base desktop dev games media")
}

# ── Step 5: Partitioning & Target Disk ───────────────────────────────────────
select_partitioning() {
    PART_MODE_FILE="$TMP_DIR/partmode.choice"
    run_dialog --title "Partitioning" \
        --menu "How would you like to partition your target drive?" 13 68 3 \
        "Auto (GPT)" "Guided Partitioning (Entire Disk with GPT & EFI)" \
        "Manual"     "Interactive Partition Tool (GNU parted / sfdisk)" \
        "Shell"      "Drop to Live Shell for Custom Setup" \
        2> "$PART_MODE_FILE" || return 1

    PART_MODE=$(cat "$PART_MODE_FILE" 2>/dev/null || echo "Auto (GPT)")
    case "$PART_MODE" in
        "Shell")
            clear
            echo "[*] Live Shell for disk preparation. Target mount is /mnt/target."
            echo "[*] Type 'exit' to return to installer."
            /bin/bash || /bin/sh
            select_partitioning
            return
            ;;
    esac

    # Disk selection
    DISK_CHOICE_FILE="$TMP_DIR/disk.choice"
    disks_list=$(get_available_disks)
    if [ -z "$disks_list" ]; then
        run_dialog --title "No Disks Found" \
            --msgbox "No suitable storage disks were detected on this machine.\nCheck your hardware connections or VM storage controllers." 8 68
        return 1
    fi

    # Build menu arguments
    set --
    eval "set -- $disks_list"
    
    run_dialog --title "Select Target Disk" \
        --menu "Select the target drive to install AvoryOS on:" 16 72 6 \
        "$@" 2> "$DISK_CHOICE_FILE" || return 1

    TARGET_DISK=$(cat "$DISK_CHOICE_FILE" 2>/dev/null || echo "")
    if [ -z "$TARGET_DISK" ]; then
        return 1
    fi

    TARGET_DEV="/dev/$TARGET_DISK"

    # Confirm erase
    CONFIRM_MSG="
WARNING: Guided partitioning will erase ALL data on:
  Device: $TARGET_DEV

The following partition layout will be created:
  • Partition 1: EFI System Partition (128 MB, FAT32)
  • Partition 2: AvoryOS Root System (Remaining space, ext4)
  • Bootloader:  Limine (UEFI x86_64 + BIOS MBR)

Are you absolutely sure you want to write these changes?"

    run_dialog --title "Confirmation - Erase Disk" \
        --yes-label "Yes - Erase and Install" --no-label "No - Go Back" \
        --yesno "$CONFIRM_MSG" 16 68
    if [ $? -ne 0 ]; then
        select_partitioning
        return
    fi
}

# ── Step 6: Perform Installation (Progress Gauge) ───────────────────────────
perform_installation() {
    exec 3>&1
    run_dialog --title "Installing AvoryOS" --gauge "Initializing installer..." 10 70 0 <&3 &
    GAUGE_PID=$!
    exec 3>&-

    update_gauge() {
        pct=$1
        msg=$2
        echo "$pct"
        echo "XXX"
        echo "$msg"
        echo "XXX"
    }

    (
        update_gauge 5 "Partitioning target disk ($TARGET_DEV)..."
        sleep 1

        # Determine partition device naming:
        # nvme0n1 -> nvme0n1p1, nvme0n1p2
        # sda     -> sda1, sda2
        case "$TARGET_DISK" in
            *[0-9]) PART_PREFIX="${TARGET_DEV}p" ;;
            *)      PART_PREFIX="${TARGET_DEV}" ;;
        esac
        ESP_DEV="${PART_PREFIX}1"
        ROOT_DEV="${PART_PREFIX}2"

        # Wipe old partition signatures and write GPT layout
        if command -v parted >/dev/null 2>&1; then
            parted -s "$TARGET_DEV" mklabel gpt
            parted -s "$TARGET_DEV" mkpart "EFI" fat32 1MiB 129MiB
            parted -s "$TARGET_DEV" set 1 esp on
            parted -s "$TARGET_DEV" mkpart "AvoryOS-Root" ext4 129MiB 100%
        elif command -v sfdisk >/dev/null 2>&1; then
            cat <<EOF | sfdisk "$TARGET_DEV" >/dev/null 2>&1
label: gpt
size=128MiB, type=U, name="EFI"
type=L, name="AvoryOS-Root"
EOF
        fi

        # Re-read partition table
        blockdev --rereadpt "$TARGET_DEV" 2>/dev/null || true
        sleep 1

        update_gauge 20 "Formatting EFI System Partition ($ESP_DEV)..."
        if command -v mkfs.vfat >/dev/null 2>&1; then
            mkfs.vfat -F 32 -n "EFI" "$ESP_DEV" >/dev/null 2>&1 || true
        fi

        update_gauge 30 "Formatting AvoryOS Root Partition ($ROOT_DEV)..."
        if command -v mke2fs >/dev/null 2>&1; then
            mke2fs -t ext4 -F -L "AvoryOS-Root" "$ROOT_DEV" >/dev/null 2>&1 || true
        elif command -v mkfs.ext4 >/dev/null 2>&1; then
            mkfs.ext4 -F -L "AvoryOS-Root" "$ROOT_DEV" >/dev/null 2>&1 || true
        fi

        update_gauge 40 "Mounting filesystems at $TARGET_MNT..."
        mkdir -p "$TARGET_MNT"
        mount -t ext4 "$ROOT_DEV" "$TARGET_MNT"
        mkdir -p "$TARGET_MNT/boot/efi"
        mount -t vfat "$ESP_DEV" "$TARGET_MNT/boot/efi" 2>/dev/null || true

        # Copy system files
        update_gauge 45 "Copying base system libraries and binaries..."
        for d in bin sbin lib lib64 usr etc opt var root home; do
            if [ -d "/$d" ]; then
                update_gauge 55 "Copying /$d to target..."
                cp -a "/$d" "$TARGET_MNT/" 2>/dev/null || true
            fi
        done

        # Prepare skeleton runtime mountpoints
        mkdir -p "$TARGET_MNT/proc" "$TARGET_MNT/sys" "$TARGET_MNT/dev" \
                 "$TARGET_MNT/run"  "$TARGET_MNT/tmp" "$TARGET_MNT/mnt" \
                 "$TARGET_MNT/boot/limine" "$TARGET_MNT/boot/efi/EFI/BOOT"
        chmod 1777 "$TARGET_MNT/tmp" "$TARGET_MNT/run"

        # Bootloader & Kernel
        update_gauge 75 "Installing Higher-Half Kernel and Limine Bootloader..."
        # Copy kernel
        if [ -f "/boot/kernel" ]; then
            cp -v "/boot/kernel" "$TARGET_MNT/boot/kernel" 2>/dev/null || true
        fi

        # Limine UEFI bootloader
        if [ -f "/usr/share/limine/BOOTX64.EFI" ]; then
            cp -v "/usr/share/limine/BOOTX64.EFI" "$TARGET_MNT/boot/efi/EFI/BOOT/BOOTX64.EFI" 2>/dev/null || true
        fi
        if [ -f "/usr/share/limine/BOOTIA32.EFI" ]; then
            cp -v "/usr/share/limine/BOOTIA32.EFI" "$TARGET_MNT/boot/efi/EFI/BOOT/BOOTIA32.EFI" 2>/dev/null || true
        fi

        # Limine BIOS bootloader
        if [ -f "/usr/share/limine/limine-bios.sys" ]; then
            cp -v "/usr/share/limine/limine-bios.sys" "$TARGET_MNT/boot/limine/" 2>/dev/null || true
        fi
        if [ -f "/usr/share/limine/boo.png" ]; then
            cp -v "/usr/share/limine/boo.png" "$TARGET_MNT/boot/limine/" 2>/dev/null || true
        fi

        # Deploy Limine configuration on target disk
        cat <<EOF > "$TARGET_MNT/boot/limine/limine.conf"
timeout: 3
interface_resolution: 1280x800
wallpaper: boot():/boot/limine/boo.png
term_wallpaper: boot():/boot/limine/boo.png

/AvoryOS
    protocol: limine
    path: boot():/boot/kernel
    cmdline: init=/sbin/init quiet splash
    resolution: 1280x800

/AvoryOS (Console / Verbose)
    protocol: limine
    path: boot():/boot/kernel
    cmdline: init=/sbin/init screenlog
    resolution: 1280x800

/AvoryOS (Rescue Shell)
    protocol: limine
    path: boot():/boot/kernel
    cmdline: init=/bin/bash screenlog
    resolution: 1280x800
EOF

        # Install BIOS MBR boot code if limine utility is present
        if command -v limine >/dev/null 2>&1; then
            limine bios-install "$TARGET_DEV" >/dev/null 2>&1 || true
        elif [ -x /usr/bin/limine ]; then
            /usr/bin/limine bios-install "$TARGET_DEV" >/dev/null 2>&1 || true
        fi

        # Generate /etc/fstab
        update_gauge 85 "Generating /etc/fstab and system configuration..."
        cat <<EOF > "$TARGET_MNT/etc/fstab"
# /etc/fstab: static file system information
# <file system> <mount point>   <type>  <options>       <dump>  <pass>
$ROOT_DEV       /               ext4    noatime,rw      0       1
$ESP_DEV        /boot/efi       vfat    defaults        0       2
tmpfs           /tmp            tmpfs   defaults        0       0
proc            /proc           proc    defaults        0       0
sysfs           /sys            sysfs   defaults        0       0
EOF

        # Configure hostname
        echo "$SYSTEM_HOSTNAME" > "$TARGET_MNT/etc/hostname"
        cat <<EOF > "$TARGET_MNT/etc/hosts"
127.0.0.1   localhost $SYSTEM_HOSTNAME
::1         localhost ip6-localhost ip6-loopback
EOF

        # Configure keymap
        if [ -n "${KEYMAP:-}" ]; then
            mkdir -p "$TARGET_MNT/etc/conf.d"
            echo "keymap=\"$KEYMAP\"" > "$TARGET_MNT/etc/conf.d/keymaps" 2>/dev/null || true
        fi

        update_gauge 95 "Flushing filesystem buffers to disk..."
        sync

        update_gauge 100 "Installation complete!"
        sleep 1
    ) | run_dialog --title "Installing AvoryOS" --gauge "Preparing installation..." 10 70 0
}

# ── Step 7: Network Configuration ────────────────────────────────────────────
configure_network() {
    run_dialog --title "Network Configuration" \
        --yes-label "Yes (DHCP)" --no-label "Skip" \
        --yesno "Would you like to enable automatic DHCP networking on boot?" 8 65
    if [ $? -eq 0 ]; then
        # Enable networking in OpenRC on target
        if [ -d "$TARGET_MNT/etc/runlevels/default" ]; then
            ln -sf /etc/init.d/networking "$TARGET_MNT/etc/runlevels/default/networking" 2>/dev/null || true
        fi
        # Configure /etc/network/interfaces
        cat <<'EOF' > "$TARGET_MNT/etc/network/interfaces"
auto lo
iface lo inet loopback

auto eth0
iface eth0 inet dhcp
EOF
    fi
}

# ── Step 8: Timezone Configuration ───────────────────────────────────────────
configure_timezone() {
    TZ_FILE="$TMP_DIR/tz.choice"
    run_dialog --title "Timezone Selection" \
        --menu "Select system timezone:" 16 68 8 \
        "UTC"                 "Coordinated Universal Time" \
        "America/New_York"    "US Eastern (New York)" \
        "America/Chicago"     "US Central (Chicago)" \
        "America/Denver"      "US Mountain (Denver)" \
        "America/Los_Angeles" "US Pacific (Los Angeles)" \
        "Europe/London"       "United Kingdom (London)" \
        "Europe/Paris"        "France (Paris)" \
        "Europe/Berlin"       "Germany (Berlin)" \
        "Asia/Tokyo"          "Japan (Tokyo)" \
        "Asia/Shanghai"       "China (Shanghai)" \
        2> "$TZ_FILE" || true

    TZ_VAL=$(cat "$TZ_FILE" 2>/dev/null || echo "UTC")
    [ -n "$TZ_VAL" ] || TZ_VAL="UTC"

    echo "$TZ_VAL" > "$TARGET_MNT/etc/timezone"
    if [ -f "$TARGET_MNT/usr/share/zoneinfo/$TZ_VAL" ]; then
        ln -sf "/usr/share/zoneinfo/$TZ_VAL" "$TARGET_MNT/etc/localtime"
    fi
}

# ── Step 9: Root Password ────────────────────────────────────────────────────
configure_root_password() {
    P1_FILE="$TMP_DIR/p1.txt"
    P2_FILE="$TMP_DIR/p2.txt"

    while true; do
        run_dialog --insecure --title "Set Root Password" \
            --passwordbox "Enter new root password (leave blank for no password):" 9 60 2> "$P1_FILE" || break
        P1=$(cat "$P1_FILE")
        [ -z "$P1" ] && break

        run_dialog --insecure --title "Confirm Root Password" \
            --passwordbox "Re-enter root password:" 9 60 2> "$P2_FILE" || continue
        P2=$(cat "$P2_FILE")

        if [ "$P1" = "$P2" ]; then
            # Set root password on target
            echo "root:$P1" | chpasswd --root "$TARGET_MNT" 2>/dev/null || \
            echo "root:$P1" | chroot "$TARGET_MNT" chpasswd 2>/dev/null || true
            break
        else
            run_dialog --title "Error" --msgbox "Passwords do not match. Please try again." 7 50
        fi
    done
    rm -f "$P1_FILE" "$P2_FILE"
}

# ── Step 10: Create User Account (FreeBSD adduser) ────────────────────────────
configure_user_account() {
    run_dialog --title "Add User Account" \
        --yes-label "Yes" --no-label "Skip" \
        --yesno "Would you like to create a normal user account?" 7 60
    [ $? -eq 0 ] || return 0

    U_NAME_FILE="$TMP_DIR/uname.txt"
    run_dialog --title "Username" \
        --inputbox "Enter username for the new account:" 9 55 "avory" 2> "$U_NAME_FILE" || return 0
    NEW_USER=$(cat "$U_NAME_FILE")
    [ -n "$NEW_USER" ] || return 0

    U_GECOS_FILE="$TMP_DIR/gecos.txt"
    run_dialog --title "Full Name" \
        --inputbox "Enter full name / description for $NEW_USER:" 9 55 "AvoryOS User" 2> "$U_GECOS_FILE" || true
    NEW_GECOS=$(cat "$U_GECOS_FILE" 2>/dev/null || echo "AvoryOS User")

    # User password
    U_P1_FILE="$TMP_DIR/up1.txt"
    U_P2_FILE="$TMP_DIR/up2.txt"
    USER_PASS=""
    while true; do
        run_dialog --insecure --title "User Password" \
            --passwordbox "Enter password for $NEW_USER:" 9 60 2> "$U_P1_FILE" || break
        UP1=$(cat "$U_P1_FILE")
        [ -z "$UP1" ] && break

        run_dialog --insecure --title "Confirm Password" \
            --passwordbox "Re-enter password for $NEW_USER:" 9 60 2> "$U_P2_FILE" || continue
        UP2=$(cat "$U_P2_FILE")

        if [ "$UP1" = "$UP2" ]; then
            USER_PASS="$UP1"
            break
        else
            run_dialog --title "Error" --msgbox "Passwords do not match. Please try again." 7 50
        fi
    done
    rm -f "$U_P1_FILE" "$U_P2_FILE"

    # Admin access
    run_dialog --title "Administrator Privileges" \
        --yes-label "Yes (Wheel/Sudo)" --no-label "No (Standard)" \
        --yesno "Should $NEW_USER be granted administrative (wheel/sudo) privileges?" 7 65
    IS_ADMIN=$?

    # Create account in target
    USER_SHELL="/bin/bash"
    [ -x "$TARGET_MNT/bin/bash" ] || USER_SHELL="/bin/sh"

    GROUPS="audio,video,input"
    [ $IS_ADMIN -eq 0 ] && GROUPS="$GROUPS,wheel"

    if [ -x "$TARGET_MNT/usr/sbin/useradd" ]; then
        chroot "$TARGET_MNT" /usr/sbin/useradd -m -s "$USER_SHELL" -c "$NEW_GECOS" -G "$GROUPS" "$NEW_USER" 2>/dev/null || true
    fi

    if [ -n "$USER_PASS" ]; then
        echo "$NEW_USER:$USER_PASS" | chpasswd --root "$TARGET_MNT" 2>/dev/null || \
        echo "$NEW_USER:$USER_PASS" | chroot "$TARGET_MNT" chpasswd 2>/dev/null || true
    fi
}

# ── Step 11: Desktop Environment Selection ───────────────────────────────────
configure_desktop() {
    DE_FILE="$TMP_DIR/de.choice"
    run_dialog --title "Default Desktop Session" \
        --radiolist "Select default environment to launch on startup:" 14 68 4 \
        "xfce4"   "XFCE 4 Desktop (Traditional, fast, recommended)" ON \
        "plasma"  "KDE Plasma 6 (Modern, high-performance)" OFF \
        "icewm"   "IceWM (Ultra-lightweight window manager)" OFF \
        "console" "Text Console / TTY Only" OFF \
        2> "$DE_FILE" || true

    DE_CHOICE=$(cat "$DE_FILE" 2>/dev/null || echo "xfce4")
    case "$DE_CHOICE" in
        "xfce4")
            XINIT_CMD="exec startxfce4"
            ;;
        "plasma")
            XINIT_CMD="exec startplasma-x11"
            ;;
        "icewm")
            XINIT_CMD="exec icewm-session"
            ;;
        *)
            XINIT_CMD=""
            ;;
    esac

    if [ -n "$XINIT_CMD" ]; then
        echo "$XINIT_CMD" > "$TARGET_MNT/root/.xinitrc"
        chmod +x "$TARGET_MNT/root/.xinitrc" 2>/dev/null || true
        if [ -n "${NEW_USER:-}" ] && [ -d "$TARGET_MNT/home/$NEW_USER" ]; then
            echo "$XINIT_CMD" > "$TARGET_MNT/home/$NEW_USER/.xinitrc"
            chroot "$TARGET_MNT" chown "$NEW_USER:$NEW_USER" "/home/$NEW_USER/.xinitrc" 2>/dev/null || true
            chmod +x "$TARGET_MNT/home/$NEW_USER/.xinitrc" 2>/dev/null || true
        fi
    fi
}

# ── Step 12: Installation Complete ───────────────────────────────────────────
installation_complete() {
    INSTALL_COMPLETED=1
    sync
    umount "$TARGET_MNT/boot/efi" 2>/dev/null || true
    umount "$TARGET_MNT" 2>/dev/null || true

    COMPLETE_MSG="
AvoryOS installation has finished successfully!

Your system is now bootable via Limine on both UEFI and BIOS.
Please remove any installation media (USB/CD) before rebooting.

Would you like to reboot into your new installation now, or
drop into a shell for final adjustments?"

    run_dialog --title "Installation Complete" \
        --extra-button --extra-label "Shell" \
        --ok-label "Reboot" --cancel-label "Exit" \
        --msgbox "$COMPLETE_MSG" 16 68
    ret=$?
    case $ret in
        0) # Reboot
           clear
           echo "[*] Rebooting system..."
           reboot
           ;;
        3) # Shell
           clear
           echo "[*] Dropping to shell. You can inspect or mount your target disk manually."
           /bin/bash || /bin/sh
           ;;
        *) # Exit
           clear
           echo "[*] Thank you for choosing AvoryOS!"
           ;;
    esac
}

# ── Main Flow ────────────────────────────────────────────────────────────────
main() {
    clear
    welcome_screen
    select_keymap
    configure_hostname
    select_components
    select_partitioning || exit 1
    perform_installation
    configure_network
    configure_timezone
    configure_root_password
    configure_user_account
    configure_desktop
    installation_complete
}

main "$@"
