# lxveos_evilportal — authorized-lab rogue AP + captive credential-capture portal (the evil_portal op).
# Needs the Wi-Fi/AP stack, the event loop, the HTTP server, and the arm gate it checks before emitting.
idf_component_register(
    SRCS "src/lxveos_evilportal.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_netif esp_event esp_http_server lxveos_arm
)
