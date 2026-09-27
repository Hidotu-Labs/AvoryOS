#include "../console/console.h"
#include "../lib/string.h"
#include "../smp/cpu.h"
#include "sched.h"

static void test_thread_entry(void) {
  struct thread *curr = sched_get_current();
  console_puts("Thread ");

  // Print TID manually
  char buf[10];
  int i = 0;
  uint32_t tid = curr->tid;
  if (tid == 0)
    buf[i++] = '0';
  else {
    while (tid > 0) {
      buf[i++] = '0' + (tid % 10);
      tid /= 10;
    }
  }
  for (int j = i - 1; j >= 0; j--)
    console_putchar(buf[j]);

  console_puts(" (Prio ");
  i = 0;
  uint32_t prio = curr->priority;
  if (prio == 0)
    buf[i++] = '0';
  else {
    while (prio > 0) {
      buf[i++] = '0' + (prio % 10);
      prio /= 10;
    }
  }
  for (int j = i - 1; j >= 0; j--)
    console_putchar(buf[j]);
  console_puts(") is running on CPU ");
  console_putchar('0' + (cpu_get_current()->cpu_id % 10));
  console_puts("\n");

  // Yield to let others run
  sched_yield();

  console_puts("Thread ");
  tid = curr->tid;
  i = 0;
  if (tid == 0)
    buf[i++] = '0';
  else {
    while (tid > 0) {
      buf[i++] = '0' + (tid % 10);
      tid /= 10;
    }
  }
  for (int j = i - 1; j >= 0; j--)
    console_putchar(buf[j]);
  console_puts(" finishing\n");
}

void sched_run_phase1_test(void) {
  console_puts("--- Scheduler Phase 1 Test: O(1) & Priority ---\n");

  // Create 3 threads with different priorities
  // Prio 5 (High), 16 (Default), 25 (Low)

  struct thread *t1 =
      sched_create_kernel_thread(test_thread_entry, NULL, false);
  t1->priority = 25;
  t1->static_priority = 25;

  struct thread *t2 =
      sched_create_kernel_thread(test_thread_entry, NULL, false);
  t2->priority = 5;
  t2->static_priority = 5;

  struct thread *t3 =
      sched_create_kernel_thread(test_thread_entry, NULL, false);
  t3->priority = 16;
  t3->static_priority = 16;

  console_puts("Created threads: T1(P25), T2(P5), T3(P16)\n");
  console_puts(
      "Enqueuing T1, then T2, then T3. Execution SHOULD be T2 -> T3 -> T1\n");

  // Explicitly enqueue them on the BSP to ensure deterministic order for this
  // test
  struct cpu_info *bsp = cpu_get_bsp();
  sched_enqueue_thread(t1, bsp);
  sched_enqueue_thread(t2, bsp);
  sched_enqueue_thread(t3, bsp);

  if (sched_validate_runqueues(bsp))
    console_puts("Runqueue linkage/membership validation: PASS\n");
  else
    console_puts("Runqueue linkage/membership validation: FAIL\n");

  console_puts("All threads enqueued. Yielding BSP to start test...\n");
  sched_yield();
}
