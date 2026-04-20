#include "tg_token.h"
#include "config_store.h"

void tg_get_token(char *out, size_t out_sz)
{
    if (!out || out_sz == 0) return;
    if (config_get_telegram_token(out, out_sz) != ESP_OK) {
        out[0] = '\0';
    }
}