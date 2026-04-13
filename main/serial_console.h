#pragma once

/**
 * Start the serial console task on the default UART.
 * Commands:
 *   add <chat_id>    — Add a technician chat ID to NVS
 *   list             — List registered chat IDs
 *   remove <index>   — Remove a chat ID by index (0-4)
 *   clear            — Remove all technician IDs
 *   help             — Show this help
 *   reboot           — Reboot the device
 *   status           — Show firmware version and OTA status
 */
void serial_console_start(void);
