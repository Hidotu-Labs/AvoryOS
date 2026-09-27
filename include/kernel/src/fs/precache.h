#ifndef FS_PRECACHE_H
#define FS_PRECACHE_H

// precache_hot_files() - Proactively populate the VFS page cache with the
// dynamic-linker interpreter and the most commonly loaded shared libraries.
// Called once during init_thread_entry(), before the first process_exec_argv(),
// so that every subsequent execve() hits hot cache instead of cold storage.
void precache_hot_files(void);

#endif // FS_PRECACHE_H