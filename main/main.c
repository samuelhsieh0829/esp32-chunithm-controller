#include <stdio.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "driver/touch_sensor_legacy.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TOUCH_PAD_COUNT 8
#define IR_PIN_COUNT 6
#define SENSOR_LINK_BAUD_RATE 2000000

#define SENSOR_PACKET_SYNC 0xFF
#define SENSOR_PACKET_ESCAPE 0xFD
#define SENSOR_PACKET_CMD_REPORT 0x81
#define SENSOR_PACKET_VERSION 0x01
#define SENSOR_PACKET_PAYLOAD_SIZE \
    (4 + (TOUCH_PAD_COUNT * 2) + (IR_PIN_COUNT * 2))

static const touch_pad_t s_touch_pads[TOUCH_PAD_COUNT] = {
    TOUCH_PAD_NUM1,
    TOUCH_PAD_NUM2,
    TOUCH_PAD_NUM3,
    TOUCH_PAD_NUM4,
    TOUCH_PAD_NUM5,
    TOUCH_PAD_NUM6,
    TOUCH_PAD_NUM7,
    TOUCH_PAD_NUM8,
};

static const gpio_num_t s_ir_pins[IR_PIN_COUNT] = {
    GPIO_NUM_9,
    GPIO_NUM_10,
    GPIO_NUM_11,
    GPIO_NUM_12,
    GPIO_NUM_13,
    GPIO_NUM_14,
};

typedef struct {
    gpio_num_t pin;
    bool adc_supported;
    adc_unit_t adc_unit;
    adc_channel_t adc_channel;
} ir_input_t;

static ir_input_t s_ir_inputs[IR_PIN_COUNT];
static adc_oneshot_unit_handle_t s_adc1_handle;
static adc_oneshot_unit_handle_t s_adc2_handle;
static bool s_adc1_ready;
static bool s_adc2_ready;

static uint16_t clamp_u16_from_u32(uint32_t value)
{
    return value > UINT16_MAX ? UINT16_MAX : (uint16_t) value;
}

static void packet_write_escaped_byte(uint8_t value, bool allow_sync)
{
    if ((!allow_sync && value == SENSOR_PACKET_SYNC) || value == SENSOR_PACKET_ESCAPE) {
        putchar(SENSOR_PACKET_ESCAPE);
        putchar((uint8_t) (value - 1));
        return;
    }

    putchar(value);
}

static void packet_send_sensor_report(
    uint8_t seq,
    uint8_t ir_valid_mask,
    const uint16_t touch_values[TOUCH_PAD_COUNT],
    const uint16_t ir_values[IR_PIN_COUNT])
{
    uint8_t payload[SENSOR_PACKET_PAYLOAD_SIZE];
    size_t cursor = 0;

    payload[cursor++] = SENSOR_PACKET_VERSION;
    payload[cursor++] = seq;
    payload[cursor++] = ir_valid_mask;
    payload[cursor++] = 0;

    for (int i = 0; i < TOUCH_PAD_COUNT; i++) {
        payload[cursor++] = (uint8_t) (touch_values[i] & 0xFF);
        payload[cursor++] = (uint8_t) ((touch_values[i] >> 8) & 0xFF);
    }

    for (int i = 0; i < IR_PIN_COUNT; i++) {
        payload[cursor++] = (uint8_t) (ir_values[i] & 0xFF);
        payload[cursor++] = (uint8_t) ((ir_values[i] >> 8) & 0xFF);
    }

    uint8_t checksum = 0;
    checksum += SENSOR_PACKET_SYNC;
    checksum += SENSOR_PACKET_CMD_REPORT;
    checksum += SENSOR_PACKET_PAYLOAD_SIZE;

    for (size_t i = 0; i < cursor; i++) {
        checksum += payload[i];
    }

    const uint8_t checksum_byte = (uint8_t) (0 - checksum);

    putchar(SENSOR_PACKET_SYNC);
    packet_write_escaped_byte(SENSOR_PACKET_CMD_REPORT, false);
    packet_write_escaped_byte(SENSOR_PACKET_PAYLOAD_SIZE, false);

    for (size_t i = 0; i < cursor; i++) {
        packet_write_escaped_byte(payload[i], false);
    }

    packet_write_escaped_byte(checksum_byte, false);
    fflush(stdout);
}

static void touch_read_all(uint16_t values[TOUCH_PAD_COUNT])
{
    for (int i = 0; i < TOUCH_PAD_COUNT; i++) {
        uint32_t raw_value = 0;
        esp_err_t err = touch_pad_read_raw_data(s_touch_pads[i], &raw_value);

        if (err == ESP_OK) {
            values[i] = clamp_u16_from_u32(raw_value);
        } else {
            values[i] = 0;
        }
    }
}

static uint8_t ir_read_all(uint16_t values[IR_PIN_COUNT])
{
    uint8_t valid_mask = 0;

    for (int i = 0; i < IR_PIN_COUNT; i++) {
        int raw_value = 0;

        if (!s_ir_inputs[i].adc_supported) {
            values[i] = 0;
            continue;
        }

        adc_oneshot_unit_handle_t handle = NULL;
        if (s_ir_inputs[i].adc_unit == ADC_UNIT_1 && s_adc1_ready) {
            handle = s_adc1_handle;
        } else if (s_ir_inputs[i].adc_unit == ADC_UNIT_2 && s_adc2_ready) {
            handle = s_adc2_handle;
        }

        if (handle == NULL) {
            values[i] = 0;
            continue;
        }

        esp_err_t err = adc_oneshot_read(handle, s_ir_inputs[i].adc_channel, &raw_value);
        if (err == ESP_OK) {
            values[i] = raw_value < 0 ? 0 : (uint16_t) raw_value;
            valid_mask |= (1u << i);
        } else {
            values[i] = 0;
        }
    }

    return valid_mask;
}

static void ir_init_all(void)
{
    uint64_t pin_mask = 0;
    for (int i = 0; i < IR_PIN_COUNT; i++) {
        pin_mask |= (1ULL << s_ir_pins[i]);
    }

    gpio_config_t io_conf = {
        .pin_bit_mask = pin_mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    adc_oneshot_unit_init_cfg_t adc1_cfg = {
        .unit_id = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };

    esp_err_t err = adc_oneshot_new_unit(&adc1_cfg, &s_adc1_handle);
    if (err != ESP_OK) {
        printf("IR ADC init failed (%s)\n", esp_err_to_name(err));
        s_adc1_ready = false;
    } else {
        s_adc1_ready = true;
    }

    adc_oneshot_unit_init_cfg_t adc2_cfg = {
        .unit_id = ADC_UNIT_2,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };

    err = adc_oneshot_new_unit(&adc2_cfg, &s_adc2_handle);
    if (err != ESP_OK) {
        printf("IR ADC2 init failed (%s)\n", esp_err_to_name(err));
        s_adc2_ready = false;
    } else {
        s_adc2_ready = true;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };

    for (int i = 0; i < IR_PIN_COUNT; i++) {
        s_ir_inputs[i].pin = s_ir_pins[i];
        s_ir_inputs[i].adc_supported = false;
        s_ir_inputs[i].adc_unit = ADC_UNIT_1;

        adc_unit_t unit;
        adc_channel_t channel;
        err = adc_oneshot_io_to_channel(s_ir_pins[i], &unit, &channel);
        if (err != ESP_OK) {
            continue;
        }

        if (unit == ADC_UNIT_1 && s_adc1_ready) {
            err = adc_oneshot_config_channel(s_adc1_handle, channel, &chan_cfg);
            if (err != ESP_OK) {
                continue;
            }
        } else if (unit == ADC_UNIT_2 && s_adc2_ready) {
            err = adc_oneshot_config_channel(s_adc2_handle, channel, &chan_cfg);
            if (err != ESP_OK) {
                continue;
            }
        } else {
            continue;
        }

        s_ir_inputs[i].adc_supported = true;
        s_ir_inputs[i].adc_unit = unit;
        s_ir_inputs[i].adc_channel = channel;
    }
}

static void touch_init_all(void)
{
    ESP_ERROR_CHECK(touch_pad_init());

    // Low-latency scan settings.
    ESP_ERROR_CHECK(touch_pad_set_charge_discharge_times(2));
    ESP_ERROR_CHECK(touch_pad_set_measurement_interval(0x400));

    // Prevent FSM from stopping when a channel measurement gets too long.
    ESP_ERROR_CHECK(touch_pad_timeout_set(false, 0));

    for (int i = 0; i < TOUCH_PAD_COUNT; i++) {
        // Use default threshold because raw values are sent to host-side conversion.
        ESP_ERROR_CHECK(touch_pad_config(s_touch_pads[i]));
    }

    ESP_ERROR_CHECK(touch_pad_set_fsm_mode(TOUCH_FSM_MODE_TIMER));
    ESP_ERROR_CHECK(touch_pad_fsm_start());

    // Wait at least one measurement cycle before the first read.
    vTaskDelay(pdMS_TO_TICKS(50));
}

void app_main(void)
{
    // Match host serial baud rate to maximize report throughput.
    ESP_ERROR_CHECK(uart_set_baudrate(UART_NUM_0, SENSOR_LINK_BAUD_RATE));

    touch_init_all();
    ir_init_all();
    uint8_t seq = 0;
    uint16_t touch_values[TOUCH_PAD_COUNT];
    uint16_t ir_values[IR_PIN_COUNT];

    while (1) {
        touch_read_all(touch_values);
        uint8_t ir_valid_mask = ir_read_all(ir_values);
        packet_send_sensor_report(seq++, ir_valid_mask, touch_values, ir_values);
    }
}
