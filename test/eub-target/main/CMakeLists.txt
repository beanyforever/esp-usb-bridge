idf_component_register(SRCS "eub_target_main.c"
                       INCLUDE_DIRS "."
                       REQUIRES driver esp_driver_usb_serial_jtag esp_app_format esp_hw_support esp_timer)
# espressif/led_strip is pulled in as a managed dependency (see idf_component.yml).
