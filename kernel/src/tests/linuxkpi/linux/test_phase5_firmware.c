/* Phase 5 C3 — firmware loader self-test.
 *
 * Reads /lib/firmware/test_fw.bin, staged into the disk image by the
 * top-level build (a 4 KB file where byte i is (i * 7 + 3) & 0xff), and
 * checks the full size + content, the -ENOENT path and path-traversal
 * rejection.
 *
 * Phase 6 C4 adds the amdgpu manifest check: the staging pass
 * (scripts/linux-firmware-install.sh) writes kpi_fw_manifest.h with the
 * name/size/zlib-CRC of every blob it installed, and this suite re-reads
 * each one through request_firmware() and compares.  Builds without
 * firmware staged simply skip that half. */

#include <linux/crc32.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/firmware.h>
#include <linux/moduleparam.h>
#include <linux/string.h>
#include <linux/vmalloc.h>

#include <linuxkpi/log.h>

#if defined(__has_include)
#  if __has_include(<kpi_fw_manifest.h>)
#    include <kpi_fw_manifest.h>
#    define P5F_HAVE_MANIFEST 1
#  endif
#  if __has_include(<kpi_nouveau_fw_manifest.h>)
#    include <kpi_nouveau_fw_manifest.h>
#    define P5F_HAVE_NOUVEAU_MANIFEST 1
#  endif
#endif

#define P5F_SIZE 4096u

#ifdef P5F_HAVE_NOUVEAU_MANIFEST
/* C4's private fixture carries these blobs. Ordinary AMD images need not. */
static bool kpi_nouveau_fw_test;
module_param(kpi_nouveau_fw_test, bool, 0);
#endif

static int p5f_failures;

static void p5f_ok(const char *what) {
  klogf("[  OK  ] LinuxKPI: fw %s\n", what);
}

static void p5f_fail(const char *what, long v) {
  p5f_failures++;
  klogf("[FAIL] LinuxKPI: fw %s (%ld)\n", what, v);
}

static unsigned char p5f_expected(unsigned int i) {
  return (unsigned char)((i * 7u + 3u) & 0xFFu);
}

#ifdef P5F_HAVE_NOUVEAU_MANIFEST
/* Exercise the actual guest VM/page allocator, not a host mock: Nouveau copies
 * firmware to vmalloc memory and gives GSP a page table of SG DMA addresses.
 * Check contents through those physical aliases, including the padded tail.
 * This only applies to Avory's identity DMA backend. No device is accessed. */
static void p5f_dma_image(const struct firmware *fw) {
  struct device dev = {
    .dma_mask = DMA_BIT_MASK(64),
    .coherent_dma_mask = DMA_BIT_MASK(64),
  };
  struct sg_table table = {0};
  struct scatterlist *sg;
  struct sg_dma_page_iter iter;
  size_t size = PAGE_ALIGN(fw->size);
  unsigned int pages = size >> PAGE_SHIFT, i, seen = 0;
  unsigned char *image = vzalloc(size);
  int ret = -ENOMEM;

  if (!image)
    goto done;
  memcpy(image, fw->data, fw->size);
  ret = sg_alloc_table(&table, pages, GFP_KERNEL);
  if (ret)
    goto free_image;
  for_each_sg(table.sgl, sg, pages, i)
    sg_set_page(sg, vmalloc_to_page(image + (size_t)i * PAGE_SIZE),
                PAGE_SIZE, 0);
  ret = dma_map_sgtable(&dev, &table, DMA_TO_DEVICE, 0);
  if (ret)
    goto free_table;
  dma_sync_sgtable_for_device(&dev, &table, DMA_TO_DEVICE);
  for_each_sgtable_dma_page(&table, &iter, 0) {
    dma_addr_t addr = sg_page_iter_dma_address(&iter);
    size_t offset = (size_t)seen * PAGE_SIZE;
    if (seen >= pages || (addr & (PAGE_SIZE - 1)) ||
        addr != page_to_phys(vmalloc_to_page(image + offset)) ||
        memcmp(page_address(phys_to_page(addr)), image + offset, PAGE_SIZE)) {
      ret = -EIO;
      break;
    }
    seen++;
  }
  if (!ret && seen != pages)
    ret = -EIO;
  dma_unmap_sgtable(&dev, &table, DMA_TO_DEVICE, 0);
free_table:
  sg_free_table(&table);
free_image:
  vfree(image);
done:
  if (ret)
    p5f_fail("DMA firmware page contents/address walk", ret);
  else
    klogf("[  OK  ] LinuxKPI: fw DMA image %zu bytes / %u pages match physical aliases\n",
          fw->size, pages);
}

static void p5f_dma_coherent(void) {
  struct device dev = {
    .dma_mask = DMA_BIT_MASK(64),
    .coherent_dma_mask = DMA_BIT_MASK(64),
  };
  const size_t sizes[] = {4096, 65536, 0x81000};
  for (unsigned int i = 0; i < ARRAY_SIZE(sizes); i++) {
    dma_addr_t addr;
    size_t size = sizes[i], offset;
    unsigned char *cpu = dma_alloc_coherent(&dev, size, &addr, GFP_KERNEL);
    int ret = 0;
    if (!cpu) {
      p5f_fail("coherent GSP buffer allocation", -ENOMEM);
      continue;
    }
    /* Linux promises order alignment for allocations up to 64 KiB. */
    if (addr & ((size <= 65536 ? size : PAGE_SIZE) - 1))
      ret = -EINVAL;
    for (offset = 0; offset < size; offset++) {
      if (cpu[offset])
        ret = -EIO;
      cpu[offset] = p5f_expected(offset);
    }
    dma_wmb();
    for (offset = 0; offset < size; offset += PAGE_SIZE) {
      size_t bytes = size - offset < PAGE_SIZE ? size - offset : PAGE_SIZE;
      if (memcmp(page_address(phys_to_page(addr + offset)), cpu + offset, bytes))
        ret = -EIO;
    }
    dma_free_coherent(&dev, size, cpu, addr);
    if (ret)
      p5f_fail("coherent GSP buffer zero/alignment/physical contents", ret);
    else
      klogf("[  OK  ] LinuxKPI: fw coherent DMA %zu bytes zero/alignment/physical contents\n", size);
  }
}
#endif

void linuxkpi_test_phase5_firmware(void) {
  const struct firmware *fw = NULL;
  int ret;
  unsigned int i;

  p5f_failures = 0;
  klog_puts("[LINUXKPI] Phase 5 firmware self-test\n");

  ret = request_firmware(&fw, "test_fw.bin", NULL);
  if (ret == 0 && fw && fw->size == P5F_SIZE) {
    for (i = 0; i < P5F_SIZE; i++) {
      if (fw->data[i] != p5f_expected(i))
        break;
    }
    if (i == P5F_SIZE)
      p5f_ok("request_firmware size + full content match");
    else
      p5f_fail("firmware content", (long)i);
  } else {
    p5f_fail("request_firmware", ret);
  }
  if (fw)
    release_firmware(fw);
  fw = NULL;

  ret = request_firmware(&fw, "definitely_missing.bin", NULL);
  if (ret == -ENOENT && fw == NULL)
    p5f_ok("missing file returns -ENOENT");
  else
    p5f_fail("missing file", ret);

  ret = request_firmware(&fw, "../etc/passwd", NULL);
  if (ret == -EINVAL && fw == NULL)
    p5f_ok("path traversal rejected");
  else
    p5f_fail("path traversal", ret);

#ifdef P5F_HAVE_MANIFEST
  {
    int manifest_bad = 0;

    for (i = 0; i < (unsigned int)KPI_FW_MANIFEST_COUNT; i++) {
      const struct kpi_fw_manifest_entry *entry = &kpi_fw_manifest[i];

      ret = request_firmware(&fw, entry->name, NULL);
      if (ret == 0 && fw) {
        u32 crc = crc32_le(~0u, fw->data, fw->size) ^ ~0u;

        if (fw->size != entry->size || crc != entry->crc32) {
          p5f_fail(entry->name, (long)crc);
          manifest_bad++;
        }
        release_firmware(fw);
        fw = NULL;
      } else {
        p5f_fail(entry->name, ret);
        manifest_bad++;
      }
    }
    if (manifest_bad == 0)
      klogf("[  OK  ] LinuxKPI: fw %u staged amdgpu blobs match host CRC\n",
            (unsigned int)KPI_FW_MANIFEST_COUNT);
  }
#else
  klog_puts("[SKIP] LinuxKPI: fw amdgpu manifest not staged\n");
#endif

#ifdef P5F_HAVE_NOUVEAU_MANIFEST
  if (kpi_nouveau_fw_test) for (i = 0; i < KPI_NOUVEAU_FW_COUNT; i++) {
    ret = firmware_request_nowarn(&fw, kpi_nouveau_fw_manifest[i].name, NULL);
    if (ret || !fw) {
      p5f_fail(kpi_nouveau_fw_manifest[i].name, ret);
      continue;
    }
    u32 crc = crc32_le(~0u, fw->data, fw->size) ^ ~0u;
    if (fw->size == kpi_nouveau_fw_manifest[i].size &&
        crc == kpi_nouveau_fw_manifest[i].crc32) {
      p5f_ok(kpi_nouveau_fw_manifest[i].name);
      p5f_dma_image(fw);
    } else
      p5f_fail(kpi_nouveau_fw_manifest[i].name, crc);
    release_firmware(fw);
    fw = NULL;
  }
  if (kpi_nouveau_fw_test)
    p5f_dma_coherent();
#endif

  if (p5f_failures == 0)
    klog_puts("[  OK  ] LinuxKPI: fw suite complete\n");
  else
    klogf("[FAIL] LinuxKPI: fw suite had %d failure(s)\n", p5f_failures);
}
