/*
 * i8042.h - Intel 8042 PS/2 Controller Driver
 *
 * This driver manages the PS/2 controller, which typically handles the
 * keyboard and mouse on legacy systems.
 *
 * Author: u/ApparentlyPlus
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

// I/O Ports
#define PS2_DATA_PORT           0x60
#define PS2_STATUS_PORT         0x64
#define PS2_COMMAND_PORT        0x64

// Status Register Bits
#define PS2_STATUS_OUTPUT_FULL  (1 << 0)
#define PS2_STATUS_INPUT_FULL   (1 << 1)
#define PS2_STATUS_SYSTEM       (1 << 2)
#define PS2_STATUS_CMD_DATA     (1 << 3)
#define PS2_STATUS_KEYBOARD_LCK (1 << 4)
#define PS2_STATUS_AUX_OUTPUT   (1 << 5)
#define PS2_STATUS_TIMEOUT      (1 << 6)
#define PS2_STATUS_PARITY_ERR   (1 << 7)

// Controller Commands
#define PS2_CMD_READ_CONFIG     0x20
#define PS2_CMD_WRITE_CONFIG    0x60
#define PS2_CMD_DISABLE_PORT2   0xA7
#define PS2_CMD_ENABLE_PORT2    0xA8
#define PS2_CMD_TEST_PORT2      0xA9
#define PS2_CMD_TEST_CONTROLLER 0xAA
#define PS2_CMD_TEST_PORT1      0xAB
#define PS2_CMD_DISABLE_PORT1   0xAD
#define PS2_CMD_ENABLE_PORT1    0xAE
#define PS2_CMD_READ_OUTPUT     0xD0
#define PS2_CMD_WRITE_OUTPUT    0xD1

// Config Byte Bits
#define PS2_CFG_PORT1_INT       (1 << 0)
#define PS2_CFG_PORT2_INT       (1 << 1)
#define PS2_CFG_SYSTEM          (1 << 2)
#define PS2_CFG_PORT1_CLOCK     (1 << 4)
#define PS2_CFG_PORT2_CLOCK     (1 << 5)
#define PS2_CFG_PORT1_TRANS     (1 << 6)

// Public API
bool i8042_init(void);
void i8042_write_command(uint8_t cmd);
void i8042_write_data(uint8_t data);
uint8_t i8042_read_data(void);
bool i8042_wait_read(void);
bool i8042_wait_write(void);
