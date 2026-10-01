/*
 * umain.c - Userspace application launch
 *
 * Linked into the kernel high half (.text). uapps() sets up the demo process, the thread
 * entry points live in uproc.c.
 *
 * Author: u/ApparentlyPlus
 */

#include <kernel/sys/scheduler.h>
#include <kernel/sys/process.h>
#include <kernel/drivers/tty.h>
#include <kernel/uproc.h>

/*
 * uapps - Spawns the userspace apps 
 */
void uapps(void) {
    process_t* proc = process_create("demo", NULL);
    sched_add(thread_create(proc, "thread_a", demo_threadA, NULL, true, 0));
    sched_add(thread_create(proc, "thread_b", demo_threadB, NULL, true, 0));

    process_t* proc2 = process_create("donut", NULL);
    sched_add(thread_create(proc2, "donut_thread", donut_sim, NULL, true, 0));
}
