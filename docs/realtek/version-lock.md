# Realtek LinuxKPI version and target lock

Audit date: 2026-11-14. Repository state: AvoryOS
`db9e6f0cc9c320887882518f2dcc44afc905502a`.

## Software

| Component | Locked value | Basis |
| --- | --- | --- |
| Upstream Linux | `v6.18.42` | `scripts/linux/baseline.txt` |
| Upstream commit | `856a9b51680cbbed1ace1207b29e424a616cff48` | `scripts/linux/baseline.txt`; local `build/linux-src` cache matches the baseline according to local inspection |
| AvoryOS source baseline | `db9e6f0cc9c320887882518f2dcc44afc905502a` | Checkout HEAD at audit |

Do not move the Linux pin as part of driver bring-up. If the pin changes,
re-audit object selection, Kconfig and firmware requests and update this lock
before continuing.

## Hardware coverage

| Role | Target state |
| --- | --- |
| First `r8169` | Locked: onboard RTL8168, `0000:07:00.0`, `10ec:8168`, rev `0x15`, subsystem `1849:8168`; host driver `r8169`; IOMMU group 17; MSI-X IRQ 93; bus reset method. No binding or probe attempted. |
| RTL8125 | Unavailable; exact device/subsystem/revision not locked. |
| First `rtw89` PCIe | Not present; RTL8852BE is only a candidate. Exact PCI/subsystem/revision not locked. |
| First `rtw88` PCIe | Not present; RTL8822CE is only a candidate. Exact PCI/subsystem/revision not locked. |
| AP / wired peer | Not inventoried or locked. |

This is a partial lock: only the software baseline and first wired target are
frozen. A hardware revision must be observed before advertising support.
