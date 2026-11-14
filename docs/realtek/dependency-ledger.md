# Realtek LinuxKPI dependency ledger — source audit

Audited against Linux `v6.18.42` (`856a9b51680cbbed1ace1207b29e424a616cff48`)
from `build/linux-src`. These are Kbuild/Kconfig observations, not a successful
enabled-family AvoryOS build. R1 added the candidate sources to
`scripts/linux/subset.txt`.

| Scope | Upstream build closure / selected feature | AvoryOS ownership and open questions |
| --- | --- | --- |
| `r8169` | `r8169_main.o`, `r8169_firmware.o`, `r8169_phy_config.o`; optional `r8169_leds.o`. Kconfig selects `FW_LOADER`, `CRC32`, `PHYLIB`, `REALTEK_PHY`; depends on PCI. | R1 import and opt-in family gate. R2 netdev/skb/NAPI/queue bridge and teardown. R3 PCI resource/IRQ/DMA, firmware request, MDIO/PHYLIB/Realtek PHY, ethtool/PM reachable paths. Audit exact active firmware requests and optional callbacks. |
| `rtw89` PCI RTL8852BE candidate | `RTW89_8852BE` selects `RTW89_CORE`, `RTW89_PCI`, `RTW89_8852B`, `RTW89_8852B_COMMON`; common core object list in `drivers/net/wireless/realtek/rtw89/Makefile`; PCI objects `pci.o`, `pci_be.o`. Driver family depends on `MAC80211`; core selects `WANT_DEV_COREDUMP`. | R1 source/object selection. R5 PCI/DMA/IRQ, firmware, scan, regulatory and recovery. Verify exact PCI/subsystem/revision and firmware requests on physical target. Devcoredump optionality is unresolved. |
| `rtw88` PCI RTL8822CE candidate | `RTW88_8822CE` selects `RTW88_CORE`, `RTW88_PCI`, `RTW88_8822C`; core object list in `drivers/net/wireless/realtek/rtw88/Makefile`; PCI object `pci.o`. Depends on `MAC80211`; core selects `WANT_DEV_COREDUMP`. | R1 source/object selection. R7 repeat R5/R6 gates and record differences. Verify exact PCI/subsystem/revision and firmware requests on physical target. Devcoredump optionality unresolved. |
| Ethernet PHY | PHYLIB and REALTEK_PHY are selected by R8169; active MDIO bus, PHY discovery/config and reachable PHY drivers must be established from the exact target's probe path. | R3 own PHYLIB/MDIO and Realtek PHY imports; distinguish required driver objects from unrelated PHY drivers. Audit IRQ/polling, link state, firmware and cleanup. |
| Wireless core | `net/wireless/cfg80211.o` comprises core/sysfs/radiotap/util/reg/scan/nl80211/mlme/ibss/sme/chan/ethtool/mesh/ap/trace/pmsr objects (plus conditionals). `net/mac80211/mac80211.o` includes main/status/driver-ops/station/scan/crypto/MLME/TX/RX/channel/rate and other core objects; optional features expand closure. Kconfig: mac80211 depends on cfg80211 and selects CRYPTO, ARC4, AES, CCM, GCM, CMAC and CRC32. cfg80211 selects FW_LOADER and CRC32; signed-regdb crypto/signature services are conditional. | R5 owns minimal feature profile and crypto/RCU/regulatory/workqueue dependencies; R6 owns nl80211 control/event path and supplicant transport. Need explicit regulatory policy and db source. Audit crypto algorithm APIs against AvoryOS services. Disable unneeded debugfs/mesh/WEXT/certs only after proving selected configuration. |
| Cross-cutting LinuxKPI | PCI, IRQ, coherent/streaming DMA, firmware, workqueues, locks/atomics, RCU, skb/network core, timers, completions, crypto, netlink and lifetime/teardown. | Existing PCI/IRQ/DMA/firmware/workqueue services exist, but compatibility and semantics for these callers are not demonstrated. Assign findings to R2/R3/R5/R6 and track symbol-level evidence after isolated link spike. |

## Isolated compile spike

Ran `gcc -std=gnu11 -O2 -fsyntax-only` on the cached pinned Linux sources, with
the include order and forced headers from `kernel/GNUmakefile`'s Linux source
rule: LinuxKPI overlay first, then generated x86/Linux headers, then freestanding
headers; `__KERNEL__`, imported-source define, `autoconf.h`, `compiler_types.h`
and `kconfig.h` enabled. No repository files were added to the import and no
object was emitted. Compiler stderr was retained under `/tmp/r0-compile/`.

| Family/core source | Result | First concrete blocker |
| --- | --- | --- |
| `drivers/net/ethernet/realtek/r8169_main.c` | Initial R0 syntax compile failed; after R2 split-type overlays, enabled build proceeds past the duplicate definitions | Remaining failures include missing `seqcount_spinlock_t` and netmem/page-pool layout assertions against LinuxKPI `struct page`; full enabled-build diagnostics are in `/tmp/r2-r8169-compile.log`. |
| `drivers/net/wireless/realtek/rtw89/core.c`, `pci.c`, `rtw8852be.c` | Failed syntax compile | Linux `net/netmem.h` compile-time layout assertions require `struct page` fields `pp_magic`, `pp`, `_pp_mapping_pad`, `dma_addr`, `pp_ref_count`; the LinuxKPI `struct page` overlay lacks these fields. First error is at `include/linux/stddef.h:16` expanding `NETMEM_DESC_ASSERT_OFFSET` in `include/net/netmem.h`. |
| `drivers/net/wireless/realtek/rtw88/main.c`, `pci.c`, `rtw8822ce.c` | Failed syntax compile | Same `net/netmem.h`/LinuxKPI `struct page` layout mismatch. |
| `net/mac80211/main.c` | Failed syntax compile | Same `net/netmem.h`/LinuxKPI `struct page` layout mismatch. |
| `net/wireless/core.c` | Failed syntax compile | First reported error: `include/linux/socket.h:403`, incomplete `struct timespec64` in `struct __kernel_sock_timeval` path; it also encounters the `netmem.h` page layout mismatch. |

The initial R0 diagnostics above record historical first failures. R2 resolved
the lockdep/timer/workqueue type collisions, and the default-off kernel build
passes. Enabled family compilation remains negative: it exposes further
header, page-layout and API gaps, and does not establish link closure or runtime
behavior. Continue one semantic compatibility layer at a time; do not add
placeholder page fields to satisfy compile-time assertions.
