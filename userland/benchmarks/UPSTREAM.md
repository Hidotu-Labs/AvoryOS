# UnixBench source provenance

Source: https://github.com/kdlucas/byte-unixbench

Pinned commit: `0b9d5ad4af3919065ee3be1be17c8a186b3cbe1f`.
Retrieved 2026-11-05. The files in `unixbench/` are unmodified upstream
sources, with the upstream GPL v2 license and README retained.

We build Dhrystone 2, Hanoi, spawn and fstime directly, without the upstream
Perl harness. `metrics.c` is an AvoryOS addition linked separately, adding
wall time and worker CPU usage. These are component results, not an official
UnixBench index. Do not compare them to the UnixBench composite score.

Build settings deliberately use generic x86-64, -O2 and no LTO or native CPU
flags. Record compiler/flags when comparing systems.
