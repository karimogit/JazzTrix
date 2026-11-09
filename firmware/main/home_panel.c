/*
 * NOTE4 and NOTE4C adapter. Same pins. NOTE4C is four-colour. NOTE4 is black
 * and white: yellow and red in the frame are sent as black. The film does not
 * report a name; a black-and-white setup that makes BUSY pulse is NOTE4.
 * LazyYoun/youn-ink-fourcolor-firmware at
 * 51812e4ab3fa80ba7a5a5a274635ca2cf3901a25, config.h and custom_lcd_display.cc.
 *
 * MIT License (upstream notices retained for the adapted portions)
 * Copyright (c) 2026 macheng2017
 * Copyright (c) 2025 Shenzhen Xinzhi Future Technology Co., Ltd.
 * Copyright (c) 2025 Project Contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "home_panel.h"
#include "jazztrix_settings.h"

#include <stdbool.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#define PANEL_RAIL GPIO_NUM_6
#define PANEL_BUSY GPIO_NUM_8
#define PANEL_RST GPIO_NUM_9
#define PANEL_DC GPIO_NUM_10
#define PANEL_CS GPIO_NUM_11
#define PANEL_SCK GPIO_NUM_12
#define PANEL_MOSI GPIO_NUM_13
#define PANEL_SPI SPI3_HOST
#define BUSY_TIMEOUT_US INT64_C(120000000)
#define REFRESH_ASSERT_TIMEOUT_US INT64_C(1000000)
#define SPI_TIMEOUT_MS 1000U

typedef enum {
    PANEL_NEW,
    PANEL_OFF,
    PANEL_ACTIVE,
    PANEL_REFRESHED,
    PANEL_FAULT
} panel_state_t;

static const char *TAG = "home_panel";
static panel_state_t panel_state = PANEL_NEW;
static spi_device_handle_t panel_spi;
static bool panel_mono;
static TaskHandle_t owner_task;
static portMUX_TYPE owner_guard = portMUX_INITIALIZER_UNLOCKED;
/* A timed-out transaction may still belong to IDF. Static storage must remain
 * alive and unchanged after FAULT; no automatic bus teardown or reuse. */
static DMA_ATTR uint8_t transfer_buffer[HOME_PANEL_ROW_BYTES];
static spi_transaction_t transaction;

static TickType_t ticks_at_least_one(uint32_t milliseconds)
{
    TickType_t ticks = pdMS_TO_TICKS(milliseconds);
    return ticks > 0 ? ticks : 1;
}

static void delay_ms(uint32_t milliseconds)
{
    /* Round upward and allow for the current partial tick, preserving minimum
     * vendor delays even when called just before the next scheduler tick. */
    TickType_t ticks = (milliseconds + portTICK_PERIOD_MS - 1U) / portTICK_PERIOD_MS;
    vTaskDelay(ticks + 1U);
}

static esp_err_t check_owner(bool claim)
{
    if (xPortInIsrContext()) {
        return ESP_ERR_INVALID_STATE;
    }
    TaskHandle_t current = xTaskGetCurrentTaskHandle();
    portENTER_CRITICAL(&owner_guard);
    if (claim && owner_task == NULL) {
        owner_task = current;
    }
    bool matches = owner_task == current;
    portEXIT_CRITICAL(&owner_guard);
    return matches ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static esp_err_t fault(esp_err_t error, const char *stage)
{
    panel_state = PANEL_FAULT;
    ESP_LOGE(TAG, "FAULT stage=%s error=%s; recovery decision required, no retry",
             stage, esp_err_to_name(error));
    return error;
}

#define PANEL_TRY(call, stage) do { \
    esp_err_t panel_error_ = (call); \
    if (panel_error_ != ESP_OK) { return fault(panel_error_, (stage)); } \
} while (0)

static esp_err_t set_rail(int level)
{
    PANEL_TRY(gpio_hold_dis(PANEL_RAIL), "rail hold disable");
    PANEL_TRY(gpio_set_level(PANEL_RAIL, level), "rail level");
    PANEL_TRY(gpio_hold_en(PANEL_RAIL), "rail hold enable");
    return ESP_OK;
}

/* How many times we waited for BUSY during the last picture. Milliseconds alone cannot tell why
 * one refresh takes 24.7 s and another 32.6 s: it may be longer because the panel really worked
 * longer (cold paper), or because OUR polling fell behind, since from 0.6.0 the chip may sleep
 * between samples. Time divided by the number of samples separates the two cases: a cold panel
 * gives more samples, a late wake-up gives the same number of samples, each of them just
 * longer. */
static uint32_t busy_polls, busy_polls_last;
uint32_t home_panel_busy_polls(void)
{
    return busy_polls_last;
}
static void (*idle_hook)(void);
void home_panel_set_idle_hook(void (*hook)(void))
{
    idle_hook = hook;
}
static esp_err_t wait_idle(const char *stage)
{
    int64_t start = esp_timer_get_time();
    busy_polls = 0;
    int64_t last_log = start;
    while (gpio_get_level(PANEL_BUSY) == 0) {
        int64_t now = esp_timer_get_time();
        if (now - start >= BUSY_TIMEOUT_US) {
            return fault(ESP_ERR_TIMEOUT, stage);
        }
        if (now - last_log >= INT64_C(5000000)) {
            ESP_LOGI(TAG, "BUSY stage=%s elapsed_ms=%ld", stage,
                     (long)((now - start) / 1000));
            last_log = now;
        }
        delay_ms(50);
        busy_polls++;
        if (idle_hook)
            idle_hook();
    }
    return ESP_OK;
}

static esp_err_t wait_refresh_asserted(void)
{
    int64_t start = esp_timer_get_time();
    while (gpio_get_level(PANEL_BUSY) != 0) {
        if (esp_timer_get_time() - start >= REFRESH_ASSERT_TIMEOUT_US) {
            return fault(ESP_ERR_TIMEOUT, "refresh did not assert BUSY");
        }
        vTaskDelay(1);
    }
    return ESP_OK;
}

static esp_err_t transfer(int data_mode, const uint8_t *bytes, size_t len)
{
    if (bytes == NULL || len == 0 || len > sizeof(transfer_buffer)) {
        return fault(ESP_ERR_INVALID_ARG, "internal transfer bounds");
    }
    memcpy(transfer_buffer, bytes, len);
    memset(&transaction, 0, sizeof(transaction));
    transaction.length = len * 8U;
    transaction.tx_buffer = transfer_buffer;
    PANEL_TRY(gpio_set_level(PANEL_DC, data_mode), "SPI DC");
    PANEL_TRY(gpio_set_level(PANEL_CS, 0), "SPI CS assert");
    PANEL_TRY(spi_device_queue_trans(panel_spi, &transaction,
                                    ticks_at_least_one(SPI_TIMEOUT_MS)), "SPI queue");
    spi_transaction_t *completed = NULL;
    PANEL_TRY(spi_device_get_trans_result(panel_spi, &completed,
                                         ticks_at_least_one(SPI_TIMEOUT_MS)), "SPI completion");
    if (completed != &transaction) {
        return fault(ESP_ERR_INVALID_STATE, "unexpected SPI transaction");
    }
    PANEL_TRY(gpio_set_level(PANEL_CS, 1), "SPI CS release");
    return ESP_OK;
}

static esp_err_t command(uint8_t byte)
{
    return transfer(0, &byte, 1);
}

static esp_err_t data(uint8_t byte)
{
    return transfer(1, &byte, 1);
}

static esp_err_t spi_attach(bool receive)
{
    if (panel_spi) {
        spi_bus_remove_device(panel_spi);
        panel_spi = NULL;
    }
    spi_bus_free(PANEL_SPI);
    spi_bus_config_t bus_config = {
        .mosi_io_num = receive ? -1 : PANEL_MOSI,
        .miso_io_num = receive ? PANEL_MOSI : -1,
        .sclk_io_num = PANEL_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = HOME_PANEL_ROW_BYTES,
    };
    spi_device_interface_config_t device_config = {
        .clock_speed_hz = receive ? 8 * 1000 * 1000 : 40 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = -1,
        .queue_size = 1,
    };
    esp_err_t err = spi_bus_initialize(PANEL_SPI, &bus_config, SPI_DMA_CH_AUTO);
    if (err != ESP_OK)
        return err;
    err = spi_bus_add_device(PANEL_SPI, &device_config, &panel_spi);
    if (err != ESP_OK)
        spi_bus_free(PANEL_SPI);
    return err;
}

/* The temperature byte is clocked back on the data pin. The bus is put back
 * the way the picture transfer left it. */
static esp_err_t read_data(uint8_t *value)
{
    esp_err_t err = spi_attach(true);
    if (err == ESP_OK) {
        spi_transaction_t transaction = {0};
        transaction.length = 8;
        transaction.flags = SPI_TRANS_USE_RXDATA;
        gpio_set_level(PANEL_DC, 1);
        gpio_set_level(PANEL_CS, 0);
        err = spi_device_polling_transmit(panel_spi, &transaction);
        gpio_set_level(PANEL_CS, 1);
        if (err == ESP_OK)
            *value = transaction.rx_data[0];
    }
    esp_err_t back = spi_attach(false);
    if (err != ESP_OK)
        return err;
    return back;
}

static esp_err_t panel_reset(void)
{
    PANEL_TRY(gpio_set_level(PANEL_RST, 1), "reset high");
    delay_ms(10);
    PANEL_TRY(gpio_set_level(PANEL_RST, 0), "reset low");
    delay_ms(20);
    PANEL_TRY(gpio_set_level(PANEL_RST, 1), "reset release");
    delay_ms(10);
    return wait_idle("reset");
}

/* NOTE4 loads its black-and-white table and pulls BUSY. NOTE4C stays idle
 * through that same pair of bytes, which is how the two films are told apart. */
static bool mono_otp_answered(void)
{
    int64_t start = esp_timer_get_time();
    int64_t low_at = 0;
    while (esp_timer_get_time() - start < INT64_C(2000000)) {
        int64_t now = esp_timer_get_time();
        if (gpio_get_level(PANEL_BUSY) == 0) {
            if (!low_at)
                low_at = now;
        } else if (low_at && now - low_at >= INT64_C(20000))
            return true;
        else
            low_at = 0;
        if (!low_at && now - start >= INT64_C(400000))
            return false;
        delay_ms(10);
    }
    return false;
}

static void remember_kind(int kind)
{
    nvs_handle_t h;
    if (nvs_open_from_partition("home_nvs", "panel", NVS_READWRITE, &h) != ESP_OK)
        return;
    nvs_set_u8(h, "k", (uint8_t)kind);
    nvs_commit(h);
    nvs_close(h);
}

static int saved_kind(void)
{
    nvs_handle_t h;
    uint8_t saved = 0;
    if (nvs_open_from_partition("home_nvs", "panel", NVS_READONLY, &h) != ESP_OK)
        return 0;
    nvs_get_u8(h, "k", &saved);
    nvs_close(h);
    return saved == 1 || saved == 2 ? saved : 0;
}

/* 1 NOTE4, 2 NOTE4C. A probe leaves the controller mid-command; the caller resets. */
static int paper_kind(bool *probed)
{
    *probed = false;
    uint8_t mode = jazztrix_paper_mode();
    if (mode == 1 || mode == 2)
        return mode;
    int saved = saved_kind();
    if (saved)
        return saved;
    *probed = true;
    if (command(0x00) != ESP_OK || data(0x2F) != ESP_OK || data(0x2E) != ESP_OK ||
        command(0xE9) != ESP_OK || data(0x01) != ESP_OK)
        return 0;
    int kind = mono_otp_answered() ? 1 : 2;
    remember_kind(kind);
    ESP_LOGI(TAG, "Paper recognized as %s", kind == 1 ? "NOTE4" : "NOTE4C");
    return kind;
}

static int mono_temp_code(void)
{
    uint8_t temp = 25;
    if (command(0x40) == ESP_OK) {
        mono_otp_answered();
        uint8_t got = 25;
        if (read_data(&got) == ESP_OK && got <= 127)
            temp = got;
    }
    if (!panel_spi)
        return -1;
    if (temp <= 5)
        return 232;
    if (temp <= 10)
        return 235;
    if (temp <= 20)
        return 238;
    if (temp <= 30)
        return 241;
    return 244;
}

static void mono_row(const uint8_t *src, uint8_t *dst)
{
    for (size_t i = 0; i < HOME_PANEL_ROW_BYTES; ++i) {
        uint8_t s = src[i], o = 0;
        for (int p = 0; p < 4; ++p) {
            /* White stays white. Black, yellow, and red become black. */
            if (((s >> (6 - 2 * p)) & 3) == 1)
                o |= (uint8_t)(1u << (6 - 2 * p));
        }
        dst[i] = o;
    }
}

esp_err_t home_panel_init(void)
{
    if (check_owner(true) != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    if (panel_state == PANEL_OFF) {
        return ESP_OK;
    }
    if (panel_state != PANEL_NEW) {
        return ESP_ERR_INVALID_STATE;
    }
    gpio_config_t output_config = {
        .pin_bit_mask = (1ULL << PANEL_RST) | (1ULL << PANEL_DC) |
                        (1ULL << PANEL_CS) | (1ULL << PANEL_RAIL),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    PANEL_TRY(gpio_config(&output_config), "GPIO outputs");
    PANEL_TRY(set_rail(0), "initial rail off");
    PANEL_TRY(gpio_set_level(PANEL_RST, 1), "initial reset release");
    PANEL_TRY(gpio_set_level(PANEL_CS, 1), "initial chip deselect");
    PANEL_TRY(gpio_set_level(PANEL_DC, 1), "initial data mode");
    gpio_config_t busy_config = {
        .pin_bit_mask = 1ULL << PANEL_BUSY,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    PANEL_TRY(gpio_config(&busy_config), "GPIO BUSY");
    spi_bus_config_t bus_config = {
        .mosi_io_num = PANEL_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = PANEL_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = HOME_PANEL_ROW_BYTES,
    };
    spi_device_interface_config_t device_config = {
        .clock_speed_hz = 40 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = -1,
        .queue_size = 1,
    };
    PANEL_TRY(spi_bus_initialize(PANEL_SPI, &bus_config, SPI_DMA_CH_AUTO), "SPI bus init");
    PANEL_TRY(spi_bus_add_device(PANEL_SPI, &device_config, &panel_spi), "SPI device init");
    panel_state = PANEL_OFF;
    ESP_LOGI(TAG, "Panel transport initialized: 400x300 SPI3 40MHz, rail off");
    return ESP_OK;
}

esp_err_t home_panel_power_off(void)
{
    if (check_owner(false) != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    if (panel_state == PANEL_OFF) {
        return ESP_OK;
    }
    if (panel_state != PANEL_REFRESHED) {
        return ESP_ERR_INVALID_STATE;
    }
    PANEL_TRY(command(0x02), "power-off command");
    PANEL_TRY(data(0x00), "power-off data");
    PANEL_TRY(wait_idle("power off"), "power-off BUSY");
    delay_ms(20);
    if (!panel_mono) {
        PANEL_TRY(command(0x07), "deep-sleep command");
        PANEL_TRY(data(0xA5), "deep-sleep data");
    }
    PANEL_TRY(set_rail(0), "normal rail off");
    panel_state = PANEL_OFF;
    return ESP_OK;
}

esp_err_t home_panel_show(const uint8_t *frame, size_t len)
{
    if (frame == NULL || len != HOME_PANEL_FRAME_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }
    if (check_owner(false) != ESP_OK || panel_state != PANEL_OFF) {
        return ESP_ERR_INVALID_STATE;
    }
    int64_t start = esp_timer_get_time();
    panel_state = PANEL_ACTIVE;
    PANEL_TRY(set_rail(1), "rail on");
    delay_ms(10);
    PANEL_TRY(panel_reset(), "reset");
    bool probed = false;
    int kind = paper_kind(&probed);
    if (kind != 1 && kind != 2)
        return ESP_FAIL;
    if (probed)
        PANEL_TRY(panel_reset(), "reset after recognize");
    panel_mono = kind == 1;
    if (panel_mono) {
        PANEL_TRY(command(0x00), "mono PSR");
        PANEL_TRY(data(0x2F), "mono PSR 2F");
        PANEL_TRY(data(0x2E), "mono PSR 2E");
        PANEL_TRY(command(0xE9), "mono OTP command");
        PANEL_TRY(data(0x01), "mono OTP data");
        PANEL_TRY(wait_idle("mono OTP"), "mono OTP BUSY");
        int encoded = mono_temp_code();
        if (encoded < 0 || panel_state == PANEL_FAULT)
            return fault(ESP_FAIL, "mono temperature");
        PANEL_TRY(command(0xE0), "temperature mode");
        PANEL_TRY(data(0x02), "temperature mode data");
        PANEL_TRY(command(0xE6), "temperature");
        PANEL_TRY(data((uint8_t)encoded), "temperature data");
        PANEL_TRY(command(0xA5), "temperature activate");
        PANEL_TRY(wait_idle("temperature activate"), "temperature BUSY");
        delay_ms(10);
        PANEL_TRY(command(0x10), "frame command");
        for (size_t row = 0; row < HOME_PANEL_HEIGHT; ++row) {
            uint8_t line[HOME_PANEL_ROW_BYTES];
            mono_row(frame + row * HOME_PANEL_ROW_BYTES, line);
            PANEL_TRY(transfer(1, line, HOME_PANEL_ROW_BYTES), "frame row");
            if ((row % 16U) == 15U)
                vTaskDelay(1);
        }
    } else {
        /* Four-colour source path uses OTP and returns here; deliberately omit
         * the monochrome 00 2F 2E and temperature commands. */
        PANEL_TRY(command(0xE9), "OTP command");
        PANEL_TRY(data(0x01), "OTP data");
        PANEL_TRY(command(0x10), "frame command");
        PANEL_TRY(wait_idle("frame write"), "frame BUSY");
        for (size_t row = 0; row < HOME_PANEL_HEIGHT; ++row) {
            PANEL_TRY(transfer(1, frame + row * HOME_PANEL_ROW_BYTES,
                               HOME_PANEL_ROW_BYTES), "frame row");
            if ((row % 16U) == 15U)
                vTaskDelay(1);
        }
    }
    PANEL_TRY(command(0x04), "power-on command");
    PANEL_TRY(wait_idle("power on"), "power-on BUSY");
    delay_ms(10);
    PANEL_TRY(command(0x12), "refresh command");
    PANEL_TRY(data(0x00), "refresh data");
    /* A pulled-up or unconnected BUSY line is not success. */
    PANEL_TRY(wait_refresh_asserted(), "refresh BUSY assertion");
    delay_ms(10);
    PANEL_TRY(wait_idle("refresh"), "refresh BUSY completion");
    panel_state = PANEL_REFRESHED;
    busy_polls_last = busy_polls;
    PANEL_TRY(home_panel_power_off(), "normal power-down");
    ESP_LOGI(TAG, "Refresh cycle completed; rail off; elapsed_ms=%ld busy_polls=%lu",
             (long)((esp_timer_get_time() - start) / 1000), (unsigned long)busy_polls_last);
    return ESP_OK;
}
