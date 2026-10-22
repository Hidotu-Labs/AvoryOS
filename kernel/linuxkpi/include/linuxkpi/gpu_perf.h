#ifndef AVORY_GPU_PERF_H
#define AVORY_GPU_PERF_H
void gpu_perf_init(const char *cmdline);
__UINT64_TYPE__ gpu_perf_ioctl_start(unsigned int request);
void gpu_perf_ioctl_finish(unsigned int request, __UINT64_TYPE__ start, long result);
void gpu_perf_dump(void);
void gpu_perf_syncobj_ready(void);
#endif
