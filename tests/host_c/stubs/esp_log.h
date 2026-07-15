#pragma once
// Host-test stub for esp_log.h — the log macros become no-ops that still "use" the tag argument so a
// file-static TAG (as in lxveos_arm.c) doesn't trip an unused-variable warning. Firmware uses the real header.
#define ESP_LOGE(tag, ...) ((void)(tag))
#define ESP_LOGW(tag, ...) ((void)(tag))
#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGD(tag, ...) ((void)(tag))
#define ESP_LOGV(tag, ...) ((void)(tag))
