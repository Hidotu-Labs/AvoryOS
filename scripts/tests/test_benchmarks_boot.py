#!/usr/bin/env python3
"""Unified AvoryOS benchmark suite runner and verifier.

Consolidates all benchmark workloads into a single test in scripts/tests:
- CPU: UnixBench Dhrystone 2 integer workload & CPU accounting
- Iterations: UnixBench Hanoi recursive workload
- Process: UnixBench fork/exit/wait throughput
- Startup: fork_exec_to_main process startup latency
- Memory: RAM mmap, page touch latency, streaming bandwidth & RSS verification
- VFS: directory metadata cycles & fstime cached read/write/copy throughput
- Desktop: scheduling wakeup latency, storage fsync/writeback, IPC, and app readiness

Supports running all workloads end-to-end or selecting individual workloads via --workload.
"""

import argparse
import math
import os
from pathlib import Path
import re
import shutil
import statistics
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts/tests'))
from boot_probe import run_probe

INIT_TEMPLATE = r'''
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/reboot.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static atomic_int done, release_worker;
static void require(int ok, const char *what) {
    if (ok) return;
    printf("UNIFIED_BENCHMARK_BOOT: FAIL %s errno=%d\n", what, errno);
    fflush(stdout);
    reboot(RB_POWER_OFF);
    _exit(1);
}

static uint64_t ns(clockid_t clock) {
    struct timespec t;
    require(clock_gettime(clock, &t) == 0, "clock_gettime");
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}

static void nap(void) {
    struct timespec t = {0, 10000000};
    nanosleep(&t, 0);
}

static uint64_t cpu_ns(const struct rusage *r) {
    return (uint64_t)(r->ru_utime.tv_sec + r->ru_stime.tv_sec) * 1000000000 +
           (r->ru_utime.tv_usec + r->ru_stime.tv_usec) * 1000;
}

static void *worker_thread(void *arg) {
    (void)arg;
    uint64_t end = ns(CLOCK_MONOTONIC) + 250000000;
    volatile uint64_t sum = 1;
    while (ns(CLOCK_MONOTONIC) < end) {
        for (unsigned i = 0; i < 10000; i++) sum = sum * 33 + i;
    }
    atomic_store(&done, 1);
    while (!atomic_load(&release_worker)) nap();
    return 0;
}

static void verify_cpu_accounting(void) {
    uint64_t before = ns(CLOCK_THREAD_CPUTIME_ID);
    struct timespec t = {0, 200000000};
    nanosleep(&t, 0);
    require(ns(CLOCK_THREAD_CPUTIME_ID) - before < 100000000, "sleep counted as CPU");

    pthread_t thread;
    require(!pthread_create(&thread, 0, worker_thread, 0), "pthread_create");
    while (!atomic_load(&done)) nap();

    uint64_t group_before = ns(CLOCK_PROCESS_CPUTIME_ID);
    struct rusage process_usage, thread_usage;
    require(!getrusage(RUSAGE_SELF, &process_usage), "getrusage SELF");
    require(!getrusage(RUSAGE_THREAD, &thread_usage), "getrusage THREAD");
    require(cpu_ns(&process_usage) > cpu_ns(&thread_usage) + 50000000, "SELF must include worker");
    require(cpu_ns(&process_usage) + 20000000 >= group_before &&
            cpu_ns(&process_usage) <= group_before + 20000000, "getrusage vs process clock");

    atomic_store(&release_worker, 1);
    require(!pthread_join(thread, 0), "pthread_join");
    for (int i = 0; i < 10; i++) nap();
    require(ns(CLOCK_PROCESS_CPUTIME_ID) >= group_before, "process CPU lost after join/reap");
    puts("UNIFIED_BENCHMARK_BOOT: CPU accounting passed");
}

static void dump_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (f) {
        char b[4096];
        size_t n;
        while ((n = fread(b, 1, sizeof(b), f))) fwrite(b, 1, n, stdout);
        fclose(f);
    }
}

static void run_cmd(const char *prog, char *const argv[]) {
    fflush(stdout);
    pid_t p = fork();
    require(p >= 0, "fork");
    if (!p) {
        execv(prog, argv);
        perror(prog);
        _exit(127);
    }
    int status = 0;
    require(waitpid(p, &status, 0) == p, "waitpid");
    require(WIFEXITED(status) && WEXITSTATUS(status) == 0, prog);
}

int main(void) {
    setvbuf(stdout, 0, _IOLBF, 0);
    puts("UNIFIED_BENCHMARK_BOOT: START");

    /* WORKLOAD_BODY */

    puts("UNIFIED_BENCHMARK_BOOT: PASS");
    fflush(stdout);
    sync();
    reboot(RB_POWER_OFF);
    for (;;) pause();
}
'''


def build_init_source(workload, seconds, ram_mib, startup_count, desktop_samples, workers):
    lines = []

    if workload in ('all', 'core', 'cpu'):
        lines.append('    verify_cpu_accounting();')

    if workload == 'all':
        lines.append(f'    run_cmd("/bin/avory_bench", (char *[]){{"avory_bench", "all", "{seconds}", "{ram_mib}", "/tmp", (char*)0}});')
        lines.append(f'    run_cmd("/bin/bench_desktop", (char *[]){{"bench_desktop", "storage", "{desktop_samples}", "/", "{workers}", (char*)0}});')
        lines.append('    dump_file("/proc/storage_perf");')
        lines.append(f'    run_cmd("/bin/bench_desktop", (char *[]){{"bench_desktop", "scheduling", "{desktop_samples}", "/tmp", "{workers}", (char*)0}});')
    elif workload == 'core':
        lines.append(f'    run_cmd("/bin/avory_bench", (char *[]){{"avory_bench", "core", "{seconds}", "{ram_mib}", "/tmp", (char*)0}});')
    elif workload == 'cpu':
        lines.append(f'    run_cmd("/bin/avory_bench", (char *[]){{"avory_bench", "cpu", "{seconds}", (char*)0}});')
    elif workload == 'iterations':
        lines.append(f'    run_cmd("/bin/avory_bench", (char *[]){{"avory_bench", "iterations", "{seconds}", (char*)0}});')
    elif workload == 'fork':
        lines.append(f'    run_cmd("/bin/avory_bench", (char *[]){{"avory_bench", "fork", "{seconds}", (char*)0}});')
    elif workload == 'startup':
        lines.append(f'    run_cmd("/bin/avory_bench", (char *[]){{"avory_bench", "startup", "{startup_count}", (char*)0}});')
    elif workload in ('ram', 'memory'):
        lines.append(f'    run_cmd("/bin/avory_bench", (char *[]){{"avory_bench", "memory", "{ram_mib}", (char*)0}});')
    elif workload == 'vfs':
        lines.append(f'    run_cmd("/bin/avory_bench", (char *[]){{"avory_bench", "vfs", "{seconds}", "/tmp", (char*)0}});')
    elif workload == 'storage':
        lines.append(f'    run_cmd("/bin/bench_desktop", (char *[]){{"bench_desktop", "storage", "{desktop_samples}", "/", "{workers}", (char*)0}});')
        lines.append('    dump_file("/proc/storage_perf");')
    elif workload in ('wakeup', 'scheduling'):
        lines.append(f'    run_cmd("/bin/bench_desktop", (char *[]){{"bench_desktop", "scheduling", "{desktop_samples}", "/tmp", "{workers}", (char*)0}});')
    elif workload == 'desktop':
        lines.append(f'    run_cmd("/bin/bench_desktop", (char *[]){{"bench_desktop", "all", "{desktop_samples}", "/", "{workers}", (char*)0}});')
    else:
        raise ValueError(f"Unknown workload: {workload}")

    body = '\n'.join(lines)
    return INIT_TEMPLATE.replace('    /* WORKLOAD_BODY */', body)


def setup(root):
    for name in ('avory_bench', 'bench_cpu', 'bench_iterations', 'bench_fork', 'bench_vfs', 'bench_desktop'):
        src = ROOT / f'userland/{name}.elf'
        if src.exists():
            shutil.copy2(src, root / f'bin/{name}')

    tree = ROOT / 'build/arch/rootfs'

    def resolve(path):
        parts, pending = [], path.split('/')
        for _ in range(128):
            if not pending:
                return tree.joinpath(*parts)
            part = pending.pop(0)
            if part in ('', '.'):
                continue
            if part == '..':
                if parts:
                    parts.pop()
                continue
            source = tree.joinpath(*parts, part)
            if source.is_symlink():
                target = os.readlink(source)
                if target.startswith('/'):
                    parts = []
                pending = target.split('/') + pending
            else:
                parts.append(part)
        raise RuntimeError('guest library symlink loop')

    for guest in ('/lib64/ld-linux-x86-64.so.2', '/usr/lib/libc.so.6', '/usr/lib/libm.so.6'):
        source = resolve(guest)
        if not source.exists() and guest == '/usr/lib/libc.so.6':
            source = resolve('/lib/libc.so.6')
        destination = root / guest.lstrip('/')
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)


def prepare_disk(disk, out):
    subprocess.run(['mke2fs', '-q', '-F', '-t', 'ext4', '-O',
                    'none,filetype,sparse_super,extent,has_journal',
                    '-d', str(out / 'root'), str(disk)], check=True)


def verify_and_report(log, workload):
    print("\n" + "=" * 60)
    print(f"  AVORY BENCHMARK REPORT: workload={workload}")
    print("=" * 60)

    # Status check
    if 'UNIFIED_BENCHMARK_BOOT: PASS' not in log:
        raise RuntimeError("Unified benchmark suite failed in guest execution.")

    # CPU accounting check
    if workload in ('all', 'core', 'cpu'):
        if 'CPU accounting passed' in log:
            print("[+] CPU accounting: PASSED (process vs thread clocks verified)")
        else:
            raise RuntimeError("CPU accounting verification failed")

    # Upstream UnixBench counts
    counts = re.findall(r'COUNT\|(\d+)\|1\|lps', log)
    if counts:
        print(f"[+] UnixBench loops/sec: {', '.join(counts)}")

    # Startup benchmark
    startup_matches = re.findall(r'STARTUP count=(\d+) fork_exec_to_main_avg_us=([\d.]+) min_us=([\d.]+) max_us=([\d.]+) fork_exec_wait_avg_us=([\d.]+)', log)
    for count, avg, min_us, max_us, wait_avg in startup_matches:
        print(f"[+] Startup ({count} launches): fork_exec_to_main_avg={avg} us (min={min_us} us, max={max_us} us), wait_avg={wait_avg} us")

    # RAM benchmark
    ram_streams = re.findall(r'RAM stream_read_write_MiB_s=([\d.]+) passes=\d+ checksum=(\d+)', log)
    for rate, csum in ram_streams:
        print(f"[+] RAM streaming throughput: {rate} MiB/s (verified checksum={csum})")

    rss = {stage: int(value) for stage, value in re.findall(r'RAM stage=(\w+) rss_KiB=(-?\d+)', log)}
    if rss:
        print(f"[+] RAM RSS: before={rss.get('before', -1)} KiB, mapped={rss.get('mapped', -1)} KiB, touched={rss.get('touched', -1)} KiB, released={rss.get('released', -1)} KiB")

    # VFS benchmark
    vfs_matches = re.findall(r'VFS metadata_cycles=(\d+) create_close_stat_rename_unlink_ns_per_cycle=([\d.]+) cycles_s=([\d.]+)', log)
    for cycles, ns_per, cycles_s in vfs_matches:
        print(f"[+] VFS metadata: {cycles} cycles, {ns_per} ns/cycle, {cycles_s} cycles/sec")

    # Latency percentiles
    latencies = re.findall(r'LATENCY name=(\S+) unit=us count=(\d+) .*? p50=([\d.]+) p90=([\d.]+) p95=([\d.]+) p99=([\d.]+) max=([\d.]+)', log)
    if latencies:
        print("\n--- Latency Workloads (microseconds) ---")
        for name, count, p50, p90, p95, p99, max_us in latencies:
            print(f"  * {name:<35} (N={count}): p50={p50:>8}  p90={p90:>8}  p95={p95:>8}  p99={p99:>8}  max={max_us:>8}")

    # Desktop suite status
    if 'status=COMPLETE errors=0' in log:
        print("\n[+] Desktop Latency Suite: COMPLETE (0 errors)")

    print("=" * 60)
    print("PASS: Unified benchmark suite completed successfully.\n")


def main():
    parser = argparse.ArgumentParser(description="Unified AvoryOS Benchmark Suite Boot Test")
    parser.add_argument('--workload', '-w', default='all',
                        choices=['all', 'core', 'cpu', 'iterations', 'fork', 'startup',
                                 'ram', 'memory', 'vfs', 'storage', 'wakeup', 'scheduling', 'desktop'],
                        help="Benchmark workload to execute (default: all)")
    parser.add_argument('--seconds', '-s', type=int, default=1,
                        help="Duration in seconds for timed workloads (default: 1 for test runs, 3 for full)")
    parser.add_argument('--ram-mib', '-m', type=int, default=32,
                        help="RAM size in MiB for memory workload (default: 32)")
    parser.add_argument('--startup-count', '-c', type=int, default=50,
                        help="Process count for startup workload (default: 50)")
    parser.add_argument('--desktop-samples', '-n', type=int, default=8,
                        help="Sample count for latency workloads (default: 8)")
    parser.add_argument('--workers', type=int, default=2,
                        help="Contention worker count for latency workloads (default: 2)")
    parser.add_argument('--disk-mib', type=int, default=256,
                        help="Virtual disk size in MiB (default: 256)")
    parser.add_argument('--timeout', type=int, default=180,
                        help="QEMU execution timeout in seconds (default: 180)")
    parser.add_argument('--accel', default=os.environ.get('AVORY_TEST_ACCEL', 'kvm' if Path('/dev/kvm').exists() else 'tcg'),
                        help="QEMU accelerator (default: auto kvm/tcg)")
    parser.add_argument('--smp', type=int, default=4,
                        help="vCPU count (default: 4)")
    parser.add_argument('--verbose', '-v', action='store_true',
                        help="Print raw serial log")

    args = parser.parse_args()

    # Ensure userland benchmark binaries exist
    subprocess.run(['make', '-C', 'userland', '-j4', 'avory_bench.elf',
                    'bench_cpu.elf', 'bench_iterations.elf', 'bench_fork.elf',
                    'bench_vfs.elf', 'bench_desktop.elf'], cwd=ROOT, check=True)

    init_code = build_init_source(
        workload=args.workload,
        seconds=args.seconds,
        ram_mib=args.ram_mib,
        startup_count=args.startup_count,
        desktop_samples=args.desktop_samples,
        workers=args.workers
    )

    probe_name = f'benchmarks-boot-{args.workload}'
    out = run_probe(
        name=probe_name,
        init=init_code,
        marker='UNIFIED_BENCHMARK_BOOT: PASS',
        setup=setup,
        prepare_disk=prepare_disk,
        filesystem='ext4',
        disk_mib=args.disk_mib,
        timeout=args.timeout,
        accel=args.accel,
        cpus=args.smp
    )

    log = (out / 'serial.log').read_text(errors='replace')
    if args.verbose:
        print(log)

    verify_and_report(log, args.workload)


if __name__ == '__main__':
    main()
