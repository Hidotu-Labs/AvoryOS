/* PSP publication diagnostics for abrupt host loss during initialization. */
#ifdef AVORYOS_AMDGPU_MEMORY_POLICY
#include "amdgpu.h"
#include <linuxkpi/log.h>

void linuxkpi_psp_ring_set_wptr(struct psp_context *psp, uint32_t value)
{
  /* The caller holds psp->mutex and has copied the command and flushed HDP.
   * Inspect software-owned memory only. No extra MMIO, waits, retries or
   * address translation. Bound logging to the first 128 init commands.
   */
  bool trace = !psp->adev->accel_working &&
               (unsigned)atomic_read(&psp->fence_value) <= 128;
  uint32_t id = 0;
  if (trace) {
    struct psp_gfx_cmd_resp *cmd = psp->cmd_buf_mem;
    id = cmd->cmd_id;
    klogf("[INFO] LinuxKPI: PSP publish enter cmd=%x cmd_gpu=%llx "
          "fence_gpu=%llx fence=%u wptr=%u\n", id,
          (unsigned long long)psp->cmd_buf_mc_addr,
          (unsigned long long)psp->fence_buf_mc_addr,
          (unsigned)atomic_read(&psp->fence_value), value);
    if (id == GFX_CMD_ID_SETUP_TMR || id == GFX_CMD_ID_SETUP_VMR) {
      struct psp_gfx_cmd_setup_tmr *tmr = &cmd->cmd.cmd_setup_tmr;
      klogf("[INFO] LinuxKPI: PSP TMR payload gpu=%llx system=%llx "
            "size=%x flags=%x vram_start=%llx vram_end=%llx base_offset=%llx\n",
            ((unsigned long long)tmr->buf_phy_addr_hi << 32) | tmr->buf_phy_addr_lo,
            ((unsigned long long)tmr->system_phy_addr_hi << 32) | tmr->system_phy_addr_lo,
            tmr->buf_size, tmr->tmr_flags,
            (unsigned long long)psp->adev->gmc.vram_start,
            (unsigned long long)psp->adev->gmc.vram_end,
            (unsigned long long)psp->adev->vm_manager.vram_base_offset);
    } else if (id == GFX_CMD_ID_LOAD_IP_FW) {
      struct psp_gfx_cmd_load_ip_fw *fw = &cmd->cmd.cmd_load_ip_fw;
      klogf("[INFO] LinuxKPI: PSP firmware payload type=%x gpu=%llx size=%x\n",
            (unsigned)fw->fw_type,
            ((unsigned long long)fw->fw_phy_addr_hi << 32) | fw->fw_phy_addr_lo,
            fw->fw_size);
    }
  }
  psp->funcs->ring_set_wptr(psp, value);
  /* This means only that the write-pointer callback returned, not that the
   * PSP completed or accepted the command. Do not sample the response here.
   */
  if (trace)
    klogf("[INFO] LinuxKPI: PSP publish returned cmd=%x wptr=%u\n", id, value);
}
#endif
