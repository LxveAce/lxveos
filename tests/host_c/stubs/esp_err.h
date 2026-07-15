#pragma once
// Minimal host-test stub for ESP-IDF's esp_err.h — just enough for lxveos_arm.c to compile and run under a
// plain host compiler. Values are distinct (their exact numbers don't matter to the tests, only that the code
// and the test agree via these same macros). NOT used by the firmware build, which uses the real ESP-IDF header.
#include <stdint.h>

typedef int esp_err_t;

#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_NOT_SUPPORTED 0x106
#define ESP_ERR_TIMEOUT 0x107
