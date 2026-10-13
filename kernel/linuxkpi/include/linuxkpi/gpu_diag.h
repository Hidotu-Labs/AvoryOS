#ifndef AVORY_GPU_DIAG_H
#define AVORY_GPU_DIAG_H
/* Native flight recorder ABI; usable without importing either DRM model. */
enum gpu_diag_kind {
  GPU_DIAG_IOCTL = 1, GPU_DIAG_CS_IN, GPU_DIAG_CS_OUT,
  GPU_DIAG_CACHE, GPU_DIAG_MAP, GPU_DIAG_FAULT,
  GPU_DIAG_IMPORT, GPU_DIAG_CPU_BEGIN, GPU_DIAG_CPU_END,
  GPU_DIAG_FENCE, GPU_DIAG_COPY_LAYOUT, GPU_DIAG_COPY_CHECK,
  GPU_DIAG_PRESENT, GPU_DIAG_CACHE_CHECK
};
enum gpu_diag_prime_stage {
  GPU_PRIME_SOURCE_ALLOC = 1, GPU_PRIME_SOURCE_EXPORT,
  GPU_PRIME_GET_FD, GPU_PRIME_SIZE, GPU_PRIME_ATTACH,
  GPU_PRIME_SG_MAP, GPU_PRIME_CPU_MAP, GPU_PRIME_IOMEM,
  GPU_PRIME_OBJECT_ALLOC, GPU_PRIME_HANDLE
};
void gpu_diag_prime_error(unsigned int stage, const void *object,
                          __UINT64_TYPE__ detail, __UINT64_TYPE__ size,
                          long result);
unsigned int gpu_diag_level(void);
void gpu_diag_record(unsigned int kind, const void *object,
                     __UINT64_TYPE__ a, __UINT64_TYPE__ b, __UINT64_TYPE__ c,
                     __UINT64_TYPE__ d, long result);
int gpu_diag_take_sample(void);
__UINT64_TYPE__ gpu_diag_hash_rect(const void *pixels, __SIZE_TYPE__ row,
                                   unsigned int height, __SIZE_TYPE__ pitch);
unsigned int gpu_diag_read(unsigned int offset, unsigned int size,
                            unsigned char *buffer);
unsigned int gpu_diag_control(unsigned int size, const unsigned char *buffer);
void gpu_diag_dump(void);
void gpu_diag_cache_check(const void *object, __UINT64_TYPE__ physical,
                           __UINT64_TYPE__ expected);
#endif
