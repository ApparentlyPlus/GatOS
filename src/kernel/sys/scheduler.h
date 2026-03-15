/*
 * scheduler.h - Round-Robin Scheduler interface
 *
 * This file defines the interface for the GatOS task scheduler.
 * It manages the execution of threads across the system.
 *
 * Author: ApparentlyPlus
 */

#pragma once

#include <kernel/sys/process.h>
#include <arch/x86_64/cpu/interrupts.h>

void sched_init(void);
void sched_add(thread_t* thread);
cpu_context_t* sched_schedule(cpu_context_t* current_context);
void sched_yield(void);
thread_t* sched_current(void);
void sched_sleep(uint64_t ms);
void sched_exit(void);
bool sched_active(void);
void sched_remove_process(struct process* proc);
