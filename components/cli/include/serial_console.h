#pragma once

/**
 * Start the serial console task on the default UART.
 * Commands:
 *   add <chat_id> [name] - Add authorized technician ID
 *   list                 - List registered chat IDs and names
 *   set_bot_admin <name> - Set shared Telegram login admin name
 *   set_bot_password <p> - Set shared Telegram login password
 *   remove <index>       - Remove a chat ID by index (0-4)
 *   clear                - Remove all technician IDs
 *   help                 - Show this help
 *   reboot               - Reboot the device
 *   status               - Show firmware version and OTA status
 */
void serial_console_start(void);
