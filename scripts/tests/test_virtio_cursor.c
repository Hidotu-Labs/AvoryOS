/* Compile the real cursor queue implementation against a controlled transport.
 * cc -O2 -ffunction-sections -fdata-sections -Ikernel/src \
 *   -Ikernel/linuxkpi/include -Ikernel/limine-protocol/include \
 *   scripts/tests/test_virtio_cursor.c \
 *   -Wl,--gc-sections -o /tmp/test-virtio-cursor
 */
#include "../../kernel/src/drivers/gpu/virtio_gpu/virtio_gpu.c"

extern int puts(const char *);
static struct virtio_gpu_update_cursor commands[128];
static void *cookies[128];
static unsigned submitted, completed, completion_budget;
static bool reject_submit;
static unsigned char command_storage[PAGE_SIZE];
static dma_buffer_t command_buffer;

hal_irq_state_t hal_irq_save(void) { return 0; }
void hal_irq_restore(hal_irq_state_t flags) { (void)flags; }
void hal_irq_enable(void) {}
void hal_irq_disable(void) {}
void tlb_shootdown_poll(void) {}
void lockdiag_spot_take(int id, uint64_t cycles, uint64_t ip) {
    (void)id; (void)cycles; (void)ip;
}
void lockdiag_spot_drop(int id) { (void)id; }
void lockdiag_spin_begin(const void *lock, uint64_t ip) { (void)lock; (void)ip; }
void lockdiag_spin_end(const void *lock) { (void)lock; }
uint64_t lapic_timer_get_ms(void) { return 0; }
void klog_puts(const char *s) { (void)s; }

int virtq_submit(struct virtqueue *vq, const struct virtq_iov *out,
                 size_t out_count, const struct virtq_iov *in,
                 size_t in_count, void *cookie) {
    (void)vq; (void)out_count; (void)in; (void)in_count;
    if (reject_submit || submitted == 128) return -1;
    commands[submitted] = *(struct virtio_gpu_update_cursor *)(uintptr_t)out->phys_addr;
    cookies[submitted++] = cookie;
    return 0;
}

bool virtq_poll_complete(struct virtqueue *vq, void **cookie, uint32_t *len) {
    (void)vq;
    if (!completion_budget || completed == submitted) return false;
    completion_budget--;
    *cookie = cookies[completed++];
    *len = 0;
    return true;
}

static void reset(void) {
    memset(&gpu, 0, sizeof(gpu));
    memset(cursor_slots, 0, sizeof(cursor_slots));
    spinlock_init(&gpu_cursor_lock);
    cursor_pending_move = false;
    cursor_in_flight = 0;
    cursor_active_head = 0;
    cursor_resource_id = 7;
    submitted = completed = completion_budget = 0;
    reject_submit = false;
    command_buffer.virt = command_storage;
    command_buffer.phys = (uintptr_t)command_storage;
    cursor_command_dma = &command_buffer;
    gpu.display_info.pmodes[0].r.width = 640;
    gpu.display_info.pmodes[0].r.height = 480;
    gpu.display_info.pmodes[1].r.width = 800;
    gpu.display_info.pmodes[1].r.height = 600;
}

int main(void) {
    reset();
    for (unsigned i = 0; i < GPU_CURSOR_SLOTS; i++)
        if (!cursor_submit(VIRTIO_GPU_CMD_MOVE_CURSOR, 7, i, i, 0, 0)) return 1;
    cursor_active_head = 1;
    if (!cursor_submit(VIRTIO_GPU_CMD_MOVE_CURSOR, 7, 73, 91, 2, 3) ||
        !cursor_pending_move) return 2;
    /* The completion worker can run before the producer sets visible=true,
     * or after another producer changes its head/hotspot. */
    cursor_visible = false;
    cursor_active_head = 0;
    cursor_last_hot_x = cursor_last_hot_y = 12;
    completion_budget = 1;
    gpu_drain_cursor_completions();
    struct virtio_gpu_update_cursor *last = &commands[submitted - 1];
    if (last->resource_id != 7 || last->pos.scanout_id != 1 ||
        last->pos.x != 73 || last->pos.y != 91 ||
        last->hot_x != 2 || last->hot_y != 3) {
        puts("FAIL: deferred move changed into a hide or used another cursor's state");
        return 3;
    }
    const uint32_t newer[] = {VIRTIO_GPU_CMD_MOVE_CURSOR,
                              VIRTIO_GPU_CMD_UPDATE_CURSOR,
                              VIRTIO_GPU_CMD_UPDATE_CURSOR};
    for (unsigned i = 0; i < 3; i++) {
        reset();
        reject_submit = true;
        cursor_submit(VIRTIO_GPU_CMD_MOVE_CURSOR, 7, 10, 20, 0, 0);
        reject_submit = false;
        if (!cursor_submit(newer[i], i == 1 ? 0 : 7, 30, 40, 0, 0) ||
            cursor_pending_move) {
            puts("FAIL: stale deferred move survived a newer move/hide/show");
            return 4;
        }
        completion_budget = 1;
        gpu_drain_cursor_completions();
        if (submitted != 1 || gpu.stats.cursor_failures) return 5;
    }
    puts("PASS: queue saturation preserves cursor visibility/head/hotspot; newer move/hide/show cancels stale moves");
    return 0;
}
