/*
 * umain.c - Userspace application launch
 *
 * Linked into kernel high-half (.text). uapps() is called by kernel_main and
 * sets up the demo process. The thread entry points live in uproc.c
 * and are routed to .user_text by the linker; their symbols resolve to
 * userspace VMAs, which is exactly what thread_create expects.
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

    process_t* proc2 = process_create("demo2", NULL);
    sched_add(thread_create(proc2, "thread_a", demo2_threadA, NULL, true, 0));
}
