#include "ktrack.h"
#include "../sched/sched.h"

void ktrack_record(const char *subsys, const char *file, uint32_t line, const char *func, int64_t err) {
    struct thread *t = sched_get_current();
    if (!t)
        return;

    t->last_subsystem = subsys;
    t->last_kernel_file = file;
    t->last_kernel_line = line;
    t->last_kernel_func = func;
    t->last_error_code = err;
}
