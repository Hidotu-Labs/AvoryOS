/* ACPI VFCT and legacy VGA shadow VBIOS loader wrapper for amdgpu.
 *
 * LinuxKPI keeps CONFIG_ACPI=n globally and omits arch/x86 IORESOURCE_ROM_SHADOW
 * setup, which disables both amdgpu_acpi_vfct_bios() and legacy 0xC0000 shadow
 * ROM mapping for APUs.  Try:
 *   1. ACPI VFCT (including unchecked checksum / RSDT fallback, logging tables)
 *   2. Upstream amdgpu_get_bios() (VRAM BAR / Expansion ROM BAR)
 *   3. Legacy VGA shadow ROM at physical 0xC0000 (128 KiB, AtomBIOS verified)
 */
#ifdef AVORYOS_AMDGPU_MEMORY_POLICY
#include "amdgpu.h"
#include "atom.h"

#include <linux/acpi.h>
#include <linux/io.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linuxkpi/log.h>
#include <linuxkpi/native_acpi.h>

extern bool linuxkpi_original_amdgpu_get_bios(struct amdgpu_device *adev);

static bool linuxkpi_check_atom_bios(struct amdgpu_device *adev, size_t size) {
  uint16_t bios_header_start;
  uint8_t *bios = adev->bios;

  if (!bios || size < 0x4a)
    return false;

  if (bios[0] != 0x55 || bios[1] != 0xAA) {
    dev_dbg(adev->dev, "VBIOS signature incorrect %x %x\n", bios[0], bios[1]);
    return false;
  }

  bios_header_start = (uint16_t)(bios[0x48] | (bios[0x49] << 8));
  if (!bios_header_start || size < (size_t)bios_header_start + 8) {
    dev_dbg(adev->dev, "VBIOS header is missing or truncated\n");
    return false;
  }

  if (!memcmp(bios + bios_header_start + 4, "ATOM", 4) ||
      !memcmp(bios + bios_header_start + 4, "MOTA", 4)) {
    dev_dbg(adev->dev, "ATOMBIOS detected\n");
    return true;
  }

  return false;
}

static int linuxkpi_vfct_match(struct amdgpu_device *adev,
                               const VFCT_IMAGE_HEADER *vhdr) {
  if (vhdr->VendorID != adev->pdev->vendor ||
      vhdr->DeviceID != adev->pdev->device)
    return -ENODEV;

  if (vhdr->PCIDevice != PCI_SLOT(adev->pdev->devfn) ||
      vhdr->PCIFunction != PCI_FUNC(adev->pdev->devfn))
    return -ENODEV;

  if (adev->pdev->bus && vhdr->PCIBus != adev->pdev->bus->number) {
    dev_notice(adev->dev,
               "VFCT bus number mismatch: table %u != runtime %u, matching by device identity (vendor 0x%04x device 0x%04x)\n",
               vhdr->PCIBus, adev->pdev->bus->number,
               adev->pdev->vendor, adev->pdev->device);
  }

  return 0;
}

static bool linuxkpi_amdgpu_acpi_vfct_bios(struct amdgpu_device *adev) {
  struct acpi_table_header *hdr = NULL;
  bool used_put = false;
  bool csum_ok = true;
  u32 tbl_size;
  UEFI_ACPI_VFCT *vfct;
  unsigned int offset;
  bool found = false;

  if (ACPI_SUCCESS(acpi_get_table(ACPI_SIG_VFCT, 1, &hdr)) && hdr) {
    used_put = true;
  } else {
    hdr = linuxkpi_acpi_find_table_unchecked(ACPI_SIG_VFCT, &csum_ok);
    if (!hdr) {
      char tables[192];
      linuxkpi_acpi_format_table_signatures(tables, sizeof(tables));
      klogf("[INFO] LinuxKPI: amdgpu VFCT ACPI table not present for %04x:%04x (ACPI: %s)\n",
            adev->pdev->vendor, adev->pdev->device, tables);
      return false;
    }
    if (!csum_ok)
      klogf("[WARN] LinuxKPI: ACPI VFCT table has invalid checksum, trying anyway\n");
  }

  tbl_size = hdr->length;
  if (tbl_size < sizeof(UEFI_ACPI_VFCT)) {
    dev_info(adev->dev,
             "ACPI VFCT table present but broken (too short #1), skipping\n");
    goto out;
  }

  vfct = (UEFI_ACPI_VFCT *)hdr;
  offset = vfct->VBIOSImageOffset;

  while (offset < tbl_size) {
    GOP_VBIOS_CONTENT *vbios = (GOP_VBIOS_CONTENT *)((char *)hdr + offset);
    VFCT_IMAGE_HEADER *vhdr = &vbios->VbiosHeader;

    if (tbl_size - offset < sizeof(VFCT_IMAGE_HEADER)) {
      dev_info(adev->dev, "ACPI VFCT image header truncated, skipping\n");
      goto out;
    }
    offset += sizeof(VFCT_IMAGE_HEADER);

    if (tbl_size - offset < vhdr->ImageLength) {
      dev_info(adev->dev, "ACPI VFCT image truncated, skipping\n");
      goto out;
    }
    offset += vhdr->ImageLength;

    if (vhdr->ImageLength && !linuxkpi_vfct_match(adev, vhdr)) {
      adev->bios = kmemdup(&vbios->VbiosContent, vhdr->ImageLength, GFP_KERNEL);
      if (!adev->bios)
        goto out;

      if (!linuxkpi_check_atom_bios(adev, vhdr->ImageLength)) {
        amdgpu_bios_release(adev);
        goto out;
      }

      adev->bios_size = vhdr->ImageLength;
      found = true;
      goto out;
    }
  }

  dev_info(adev->dev,
           "ACPI VFCT table present but no matching image found, skipping\n");

out:
  if (used_put)
    acpi_put_table(hdr);
  return found;
}

static bool linuxkpi_amdgpu_shadow_bios(struct amdgpu_device *adev) {
  const resource_size_t shadow_phys = 0xC0000;
  const size_t shadow_max = 0x20000;
  void __iomem *rom;
  uint8_t *buf;
  size_t rom_len;

  if ((adev->pdev->class >> 16) != PCI_BASE_CLASS_DISPLAY)
    return false;

  rom = ioremap(shadow_phys, shadow_max);
  if (!rom)
    return false;

  buf = kmalloc(shadow_max, GFP_KERNEL);
  if (!buf) {
    iounmap(rom);
    return false;
  }

  memcpy_fromio(buf, rom, shadow_max);
  iounmap(rom);

  if (buf[0] != 0x55 || buf[1] != 0xAA) {
    klogf("[INFO] LinuxKPI: legacy shadow ROM 0xC0000 sig=%02x %02x\n",
          buf[0], buf[1]);
    kfree(buf);
    return false;
  }

  rom_len = buf[2] ? ((size_t)buf[2] << 9) : shadow_max;
  if (rom_len > shadow_max)
    rom_len = shadow_max;

  if (rom_len >= 0x20) {
    uint16_t pcir_off = (uint16_t)(buf[0x18] | (buf[0x19] << 8));
    if (pcir_off <= rom_len - 8 && !memcmp(buf + pcir_off, "PCIR", 4)) {
      uint16_t ven = (uint16_t)(buf[pcir_off + 4] | (buf[pcir_off + 5] << 8));
      if (ven != adev->pdev->vendor) {
        klogf("[INFO] LinuxKPI: shadow ROM PCIR vendor %04x != %04x\n",
              ven, adev->pdev->vendor);
        kfree(buf);
        return false;
      }
    }
  }

  adev->bios = buf;
  if (!linuxkpi_check_atom_bios(adev, rom_len)) {
    klogf("[INFO] LinuxKPI: legacy shadow ROM 0xC0000 failed AtomBIOS check (len=%zu)\n",
          rom_len);
    amdgpu_bios_release(adev);
    return false;
  }

  adev->bios_size = (uint32_t)rom_len;
  dev_info(adev->dev, "Fetched VBIOS from legacy shadow ROM (%u bytes)\n",
           adev->bios_size);
  return true;
}

bool amdgpu_get_bios(struct amdgpu_device *adev) {
  if (!adev || !adev->pdev)
    return false;

  if (linuxkpi_amdgpu_acpi_vfct_bios(adev)) {
    dev_info(adev->dev, "Fetched VBIOS from VFCT (%u bytes)\n",
             adev->bios_size);
    adev->is_atom_fw = adev->asic_type >= CHIP_VEGA10;
    return true;
  }

  if (linuxkpi_original_amdgpu_get_bios(adev))
    return true;

  if (linuxkpi_amdgpu_shadow_bios(adev)) {
    adev->is_atom_fw = adev->asic_type >= CHIP_VEGA10;
    return true;
  }

  return false;
}
#endif
