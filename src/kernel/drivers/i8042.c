/*
 * i8042.c - Intel 8042 PS/2 Controller Driver Implementation
 *
 * This implementation follows the standard initialization sequence:
 * Disable devices
 * Flush buffer
 * Set config byte
 * Self-test controller
 * Check for dual channel
 * Interface tests
 * Enable devices
 *
 * Author: u/ApparentlyPlus
 */

#include <kernel/drivers/i8042.h>
#include <arch/x86_64/cpu/io.h>
#include <kernel/sys/timers.h>
#include <kernel/debug.h>

#define I8042_TIMEOUT_US 100000 // 100ms timeout for hardware sync

#pragma region Helper Functions

/*
 * i8042_wait_read - Wait until output buffer is full (data available to read)
 */
bool i8042_wait_read(void) {
    for (int i = 0; i < 1000; i++) { // Polling with short sleeps
        if (inb(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL) {
            return true;
        }
        sleep_us(100);
    }
    return false;
}

/*
 * i8042_wait_write - Wait until input buffer is empty (ready to receive data/command)
 */
bool i8042_wait_write(void) {
    for (int i = 0; i < 1000; i++) {
        if (!(inb(PS2_STATUS_PORT) & PS2_STATUS_INPUT_FULL)) {
            return true;
        }
        sleep_us(100);
    }
    return false;
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

    // Disable Devices
    i8042_write_command(PS2_CMD_DISABLE_PORT1);
    i8042_write_command(PS2_CMD_DISABLE_PORT2);

    // Flush Buffer
    i8042_flush();

    // Set Controller Configuration Byte
    i8042_write_command(PS2_CMD_READ_CONFIG);
    uint8_t config = i8042_read_data();
    
    // Disable interrupts and translation for now during test
    config &= ~(PS2_CFG_PORT1_INT | PS2_CFG_PORT2_INT | PS2_CFG_PORT1_TRANS);
    bool is_dual_channel = (config & PS2_CFG_PORT2_CLOCK); // If clock is disabled, might be dual

    i8042_write_command(PS2_CMD_WRITE_CONFIG);
    i8042_write_data(config);

    // Controller Self-Test
    i8042_write_command(PS2_CMD_TEST_CONTROLLER);
    if (i8042_read_data() != 0x55) {
        LOGF("[PS2 ERROR] Controller self-test failed.\n");
        return false;
    }

    // Determine if Dual Channel exists
    if (is_dual_channel) {
        i8042_write_command(PS2_CMD_ENABLE_PORT2);
        i8042_write_command(PS2_CMD_READ_CONFIG);
        config = i8042_read_data();
        if (!(config & PS2_CFG_PORT2_CLOCK)) {
            is_dual_channel = true;
        } else {
            is_dual_channel = false;
        }
        i8042_write_command(PS2_CMD_DISABLE_PORT2);
    }

    // Interface Tests
    i8042_write_command(PS2_CMD_TEST_PORT1);
    if (i8042_read_data() != 0x00) {
        LOGF("[PS2 ERROR] Port 1 test failed.\n");
        return false;
    }

    if (is_dual_channel) {
        i8042_write_command(PS2_CMD_TEST_PORT2);
        if (i8042_read_data() != 0x00) {
            LOGF("[PS2 WARN] Port 2 test failed, disabling dual channel.\n");
            is_dual_channel = false;
        }
    }

    // Enable Devices & Translation
    i8042_write_command(PS2_CMD_READ_CONFIG);
    config = i8042_read_data();
    config |= PS2_CFG_PORT1_INT | PS2_CFG_PORT1_TRANS; // Enable Port 1 IRQs and Set 2 -> Set 1 translation
    if (is_dual_channel) {
        config |= PS2_CFG_PORT2_INT;
    }
    
    i8042_write_command(PS2_CMD_WRITE_CONFIG);
    i8042_write_data(config);

    i8042_write_command(PS2_CMD_ENABLE_PORT1);
    if (is_dual_channel) {
        i8042_write_command(PS2_CMD_ENABLE_PORT2);
    }

    LOGF("[PS2] Controller initialized. Dual Channel: %s\n", is_dual_channel ? "Yes" : "No");
    return true;
}

#pragma endregion
