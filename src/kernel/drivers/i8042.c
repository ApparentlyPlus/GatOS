/*
 * i8042.c - Intel 8042 PS/2 Controller Driver Implementation
 *
 * Init sequence: disable both ports, flush the buffer, set the config byte, self-test,
 * then enable port 1 and reset the keyboard. Port 2 (the mouse) stays off.
 *
 * Reference: the OSDev wiki "8042 PS/2 Controller" page and the Linux kernel's
 * i8042 driver (drivers/input/serio/i8042.c, GPL-2.0).
 *
 * Author: u/ApparentlyPlus
 */

#include <kernel/drivers/i8042.h>
#include <arch/x86_64/cpu/io.h>
#include <kernel/sys/timers.h>
#include <kernel/debug.h>

#define I8042_TIMEOUT_US 100000  // 100ms timeout for hardware sync
#define I8042_BAT_US 1000000 // a real keyboard takes 500-750ms to finish its self-test after a reset

#pragma region Helper Functions

/*
 * wait_status - Poll the status register in 100us steps until (status & mask) == want, or us runs out
 */
static bool wait_status(uint8_t mask, uint8_t want, uint32_t us) {
    for (uint32_t i = 0; i < us / 100; i++) {
        if ((inb(PS2_STATUS_PORT) & mask) == want) {
            return true;
        }
        sleep_us(100);
    }
    return false;
}

/*
 * i8042_wait_read - Wait until output buffer is full (data available to read)
 */
bool i8042_wait_read(void) {
    return wait_status(PS2_STATUS_OUTPUT_FULL, PS2_STATUS_OUTPUT_FULL, I8042_TIMEOUT_US);
}

/*
 * i8042_wait_write - Wait until input buffer is empty (ready to receive data/command)
 */
bool i8042_wait_write(void) {
    return wait_status(PS2_STATUS_INPUT_FULL, 0, I8042_TIMEOUT_US);
}

/*
 * i8042_write_command - Sends a command byte to the controller
 */
void i8042_write_command(uint8_t cmd) {
    i8042_wait_write();
    outb(PS2_COMMAND_PORT, cmd);
}

/*
 * i8042_write_data - Sends a data byte to the controller
 */
void i8042_write_data(uint8_t data) {
    i8042_wait_write();
    outb(PS2_DATA_PORT, data);
}

/*
 * i8042_read_data - Reads a data byte from the controller
 */
uint8_t i8042_read_data(void) {
    if (i8042_wait_read()) {
        return inb(PS2_DATA_PORT);
    }
    return 0;
}

/*
 * i8042_flush - Flushes the output buffer
 */
static void i8042_flush(void) {
    while (inb(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL) {
        inb(PS2_DATA_PORT);
    }
}

#pragma endregion

#pragma region Initialization

/*
 * i8042_init - Performs full controller initialization and discovery
 */
bool i8042_init(void) {
    LOGF("[PS2] Initializing i8042 Controller...\n");

    // This whole thing is mostly legacy PS/2 stuff, but we still need
    // to do it for the keyboard controller since that's how we get input events (blame x86 squared)

    // Disable Devices
    i8042_write_command(PS2_CMD_DISABLE_PORT1);
    i8042_write_command(PS2_CMD_DISABLE_PORT2);

    // Flush Buffer
    i8042_flush();

    // Set Controller Configuration Byte
    i8042_write_command(PS2_CMD_READ_CONFIG);
    uint8_t config = i8042_read_data();
    
    // Enable Port 1 Interrupts
    // Enable Port 1 Translation (Set 2 -> Set 1)
    config |= PS2_CFG_PORT1_INT | PS2_CFG_PORT1_TRANS | PS2_CFG_PORT2_CLOCK;
    config &= ~(PS2_CFG_PORT1_CLOCK | PS2_CFG_PORT2_INT);

    i8042_write_command(PS2_CMD_WRITE_CONFIG);
    i8042_write_data(config);

    // Controller Self Test
    i8042_write_command(PS2_CMD_TEST_CONTROLLER);
    uint8_t self_test = i8042_read_data();
    if (self_test != 0x55) {
        LOGF("[PS2 ERROR] Controller self-test failed (0x%x).\n", self_test);
        return false;
    }

    // Some controllers reset themselves during the self-test, so write the config again
    i8042_write_command(PS2_CMD_WRITE_CONFIG);
    i8042_write_data(config);

    // Enable Port 1
    i8042_write_command(PS2_CMD_ENABLE_PORT1);

    // Keyboard Reset
    i8042_write_data(0xFF);
    uint8_t ack = i8042_read_data();
    uint8_t bat = wait_status(PS2_STATUS_OUTPUT_FULL, PS2_STATUS_OUTPUT_FULL, I8042_BAT_US) ? inb(PS2_DATA_PORT) : 0;
    if (ack != 0xFA || bat != 0xAA) {
        LOGF("[PS2] Keyboard reset answered 0x%x 0x%x (want 0xFA 0xAA).\n", ack, bat);
    }
    
    // Enable Scanning
    i8042_write_data(0xF4);
    i8042_read_data(); // ACK

    LOGF("[PS2] Controller initialized.\n");
    return true;
}

#pragma endregion
