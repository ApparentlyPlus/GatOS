/*
 * tty.c - TTY Abstraction Implementation
 *
 * Author: u/ApparentlyPlus
 */

#include <kernel/drivers/tty.h>
#include <libc/string.h>

tty_t* g_active_tty = NULL;

/*
 * tty_init - Initializes a TTY instance
 */
void tty_init(tty_t* tty, void (*write_cb)(char)) {
    memset(tty->buffer, 0, TTY_BUFFER_SIZE);
    tty->head = 0;
    tty->tail = 0;
    tty->canon_pos = 0;
    tty->echo = true;
    tty->canon = true;
    tty->write_callback = write_cb;
    spinlock_init(&tty->lock, "tty_lock");
}

/*
 * tty_wait_for_input - Blocks execution until data is available
 */
static void tty_wait_for_input(tty_t* tty) {
    // FUTURE PROOFING: 
    // In a multitasking environment, this function MUST NOT busy-wait.
    // Instead, it should put the current thread/process into a 'WAITING' state
    // and call the scheduler. The tty_push_char function (invoked by IRQ)
    // would then be responsible for waking up any threads waiting on this TTY.
    
    while (tty->head == tty->tail) {
        __asm__ volatile("hlt");
    }
}

/*
 * tty_push_char - Handles input from hardware (called by driver/IRQ)
 */
void tty_push_char(tty_t* tty, char c) {
    bool flags = spinlock_acquire(&tty->lock);

    // Canonical mode logic
    if (tty->canon) {
        if (c == '\b') { // Backspace
            if (tty->head != tty->canon_pos) {
                tty->head = (tty->head - 1) % TTY_BUFFER_SIZE;
                if (tty->echo && tty->write_callback) {
                    tty->write_callback('\b');
                    tty->write_callback(' ');
                    tty->write_callback('\b');
                }
            }
            spinlock_release(&tty->lock, flags);
            return;
        }

        if (c == '\n' || c == '\r') {
            c = '\n'; // Normalize
            tty->buffer[tty->head] = c;
            tty->head = (tty->head + 1) % TTY_BUFFER_SIZE;
            tty->canon_pos = tty->head; // Line is now committed
            
            if (tty->echo && tty->write_callback) {
                tty->write_callback('\n');
            }
            spinlock_release(&tty->lock, flags);
            return;
        }
    }

    // Default push
    uint32_t next = (tty->head + 1) % TTY_BUFFER_SIZE;
    if (next != tty->tail) {
        tty->buffer[tty->head] = c;
        tty->head = next;
        
        if (tty->echo && tty->write_callback) {
            tty->write_callback(c);
        }
    }

    spinlock_release(&tty->lock, flags);
}

/*
 * tty_read_char - Reads one character from the TTY (Blocks)
 */
char tty_read_char(tty_t* tty) {
    tty_wait_for_input(tty);

    bool flags = spinlock_acquire(&tty->lock);
    char c = tty->buffer[tty->tail];
    tty->tail = (tty->tail + 1) % TTY_BUFFER_SIZE;
    spinlock_release(&tty->lock, flags);

    return c;
}

/*
 * tty_read - Reads a block of characters (Blocks until data available)
 */
size_t tty_read(tty_t* tty, char* buf, size_t count) {
    size_t i = 0;
    while (i < count) {
        buf[i++] = tty_read_char(tty);
        if (tty->canon && buf[i-1] == '\n') break;
    }
    return i;
}

/*
 * tty_write - Writes a block of characters to the TTY hardware
 */
void tty_write(tty_t* tty, const char* buf, size_t count) {
    if (!tty || !tty->write_callback) return;
    
    bool flags = spinlock_acquire(&tty->lock);
    for (size_t i = 0; i < count; i++) {
        tty->write_callback(buf[i]);
    }
    spinlock_release(&tty->lock, flags);
}
