section .rodata
global vdso_blob_start
global vdso_blob_end

align 4096
vdso_blob_start:
incbin "src/syscalls/vdso.so"
vdso_blob_end:
