# PID namespaces

`CLONE_NEWPID` creates a hierarchical PID namespace. Its first process is PID 1,
with parent PID 0 and its own initial process group/session. Kernel scheduler,
parent links, PIDFDs, and signal queues retain global task identifiers. Public
IDs are translated through an immutable mapping for every ancestor namespace;
a task cannot resolve an ancestor or sibling namespace's processes by guessing
its global identifier.

`unshare(CLONE_NEWPID)` selects the namespace for future children without changing
the caller's own PID or active namespace. Fork, vfork, clone, threads, and exec
preserve the appropriate mappings. Creating a thread across PID namespaces is
rejected. Namespace references and parent references survive until all task and
future-child references are released. Nesting is limited to 31 child levels.

Public getpid/gettid/getppid, clone return/TID stores, wait results, scheduler and
priority targets, process group/session IDs, PIDFD creation, and signal targets
use IDs visible to the caller. Process-directed and group-directed signals only
match visible tasks. Signal sender identities are translated at delivery;
user-supplied queued signal payloads retain their explicit payload semantics.

Proc numeric lookup, enumeration, self/thread-self links, task subdirectories,
stat/status identities, and namespace identities use the current PID view.
Generated PID nodes retain global identifiers internally and recheck visibility
when accessed. Sensitive process files and descriptor magic links require the
same process or CAP_SYS_PTRACE. Descriptor magic links hold a vnode reference
under task/descriptor locks and remain usable across filesystem pivots.

Orphans are adopted by the live namespace init when available. When the last
thread of namespace init exits, the namespace closes permanently to new tasks
and remaining descendants receive SIGKILL. Namespace init ignores unhandled
userspace signals; visible ancestors may force SIGKILL or SIGSTOP. Existing ancestor references may
still wait for their direct children. New children in a closed namespace fail
with ENOMEM.

Validation: `python3 scripts/tests/test_pid_namespace_boot.py` boots a private
four-CPU QEMU image. It checks namespace PID 1, PPID/group/session values,
ancestor PID/proc invisibility, fork numbering and wait results, local SIGKILL,
nested namespaces, unshare-for-children, dead-namespace ENOMEM, invalid thread
combinations, and namespace-init exit killing a sleeping orphan. It does not
modify `disk.img` or the normal boot ISO.

The implementation does not yet provide setns or Linux proc namespace magic
links, pid_for_children namespace descriptors, per-namespace PID allocation
sysctls. Namespace
files currently expose identity through regular file inode numbers. Those
features are separate from the clone/unshare lifecycle used by bubblewrap.
