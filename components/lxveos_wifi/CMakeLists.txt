# lxveos_wifi — Wi-Fi recon (M1). Passive AP scan; the Wi-Fi stack is brought up lazily on first use.
# REQUIRES the Wi-Fi/networking stack + the event loop it posts on; nvs_flash is already inited by main.
idf_component_register(
    SRCS "src/lxveos_wifi.c" "src/lxveos_wifi_labels.c" "src/lxveos_wifi_eapol.c" "src/lxveos_wifi_essidmap.c"
         "src/lxveos_wifi_audit.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_netif esp_event esp_timer
)
