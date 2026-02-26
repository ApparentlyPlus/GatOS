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

void scheduler_init(void);
void scheduler_add_thread(thread_t* thread);
cpu_context_t* scheduler_schedule(cpu_context_t* current_context);
void scheduler_yield(void);
thread_t* scheduler_get_current_thread(void);
void scheduler_thread_sleep(uint64_t ms);
void scheduler_thread_exit(void);
bool scheduler_is_active(void);
