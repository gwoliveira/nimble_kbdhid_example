/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include <string.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"

#include "hid_codes.h"
#include "hid_func.h"
#include "gpio_func.h"

#include "host/ble_store.h"

static const char *tag = "NimBLEKBD_main";

/* from ble_func.c */
extern void ble_init();

void
app_main(void)
{
    ESP_LOGI(tag, "app_main start");

    /* Initialize NVS — it is used to store PHY calibration data and bonding data */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(tag, "NVS partition was truncated; erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(tag, "NVS initialized");

    ESP_LOGI(tag, "Starting BLE initialization...");
    ble_init();
    ESP_LOGI(tag, "BLE init ok");

        // Optional: wipe bonds and IRKs to recover from bad state
    #ifdef CONFIG_EXAMPLE_WIPE_BONDS
        esp_err_t nvs_rc = nvs_flash_init();
        if (nvs_rc == ESP_ERR_NVS_NO_FREE_PAGES || nvs_rc == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            ESP_LOGW(tag, "NVS partition was truncated; erasing...");
            nvs_flash_erase();
            nvs_rc = nvs_flash_init();
        }
        if (nvs_rc == ESP_OK) {
            ble_store_util_delete_all();
        }
    #endif

    // Delay to let BLE stack initialize fully
    vTaskDelay(pdMS_TO_TICKS(1000));

    QueueHandle_t buttons_queue = xQueueCreate(10, sizeof(uint32_t));
    if (!buttons_queue) {
        ESP_LOGE(tag, "Can not create queue!");
        vTaskDelay(pdMS_TO_TICKS(30000));
        esp_restart();
    }

    ESP_LOGI(tag, "Creating GPIO task...");
    if (xTaskCreate(gpio_btn_task, "gpio_btn_task", 2048, buttons_queue, 10, NULL) != pdPASS) {
        ESP_LOGE(tag, "Can not create gpio_btn_task!");
        vTaskDelay(pdMS_TO_TICKS(30000));
        esp_restart();
    }
    ESP_LOGI(tag, "GPIO task created, waiting for buttons ...");

    while (1) {
        uint32_t button, key_to_send;
        if (xQueueReceive(buttons_queue, &button, portMAX_DELAY) == pdTRUE) {

            // released or pressed?
            bool pressed = true;
            if (button & BUTTON_RELEASED_BIT) pressed = false;

            // byte 0 have a key code
            key_to_send = button & 0xff;

            ESP_LOGI(tag, "button %d type %08X (src %08X) %s",
                key_to_send, button & BUTTON_TYPE_MASK, button,
                pressed ? "pressed" : "released");

            switch (button & BUTTON_TYPE_MASK) {
                case BUTTON_TYPE_KEYBOARD:
                    hid_keyboard_change_key(key_to_send, pressed);
                    break;

                case BUTTON_TYPE_CC:
                    hid_cc_change_key(key_to_send, pressed);
                    break;

                case BUTTON_TYPE_MOUSE:
                    hid_mouse_change_key(key_to_send, 0, 0, pressed);
                    break;

                default:
                    ESP_LOGI(tag, "unknown button type %d", (button & BUTTON_TYPE_MASK) >> 24);
            }
        }
    }
}
