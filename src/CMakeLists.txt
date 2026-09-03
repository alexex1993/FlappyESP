# boards/${FLAPPY_BOARD}/board_pins.h is what makes this one main.c serve both
# boards: it carries the pin map plus the BSP_HAS_TOUCH / BSP_PANEL_* feature
# macros that main.c switches on.
if(FLAPPY_BOARD STREQUAL "touch_lcd147")
    # JD9853 panel + AXS5106L touch, both vendored under components/.
    set(board_requires jd9853 axs5106l esp_driver_i2c)
else()
    # ST7789 lives in esp_lcd itself, and there is nothing to touch.
    set(board_requires "")
endif()

idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "." "../include" "../boards/${FLAPPY_BOARD}"
                       REQUIRES ${board_requires}
                                esp_lcd nvs_flash
                                esp_driver_spi
                                esp_driver_ledc esp_driver_gpio
                                esp_timer esp_hw_support)
