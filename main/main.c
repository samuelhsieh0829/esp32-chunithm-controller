#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/uart.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TOUCH_SURFACE_COUNT 16
#define TOUCH_DEVICE_COUNT 2
#define IR_PIN_COUNT 6
#define SENSOR_LINK_BAUD_RATE 2000000

#define MPR121_I2C_PORT I2C_NUM_0
#define MPR121_I2C_SDA_PIN GPIO_NUM_7
#define MPR121_I2C_SCL_PIN GPIO_NUM_8
#define MPR121_I2C_FREQ_HZ 400000

#define MPR121_ADDR_GND 0x5A
#define MPR121_ADDR_SDA 0x5C

#define MPR121_TOUCH_THRESHOLD 12
#define MPR121_RELEASE_THRESHOLD 6
#define MPR121_ELECTRODE_COUNT 12
#define MPR121_DELTA_DEADZONE 2
#define MPR121_DELTA_FULL_SCALE 48

#define MPR121_REG_TOUCH_STATUS_L 0x00
#define MPR121_REG_FILTERED_DATA_BASE 0x04
#define MPR121_REG_BASELINE_BASE 0x1E
#define MPR121_REG_SOFTRESET 0x80
#define MPR121_REG_ELE_CFG 0x5E
#define MPR121_REG_DEBOUNCE 0x5B
#define MPR121_REG_CONFIG1 0x5C
#define MPR121_REG_CONFIG2 0x5D
#define MPR121_REG_TOUCH_THRESHOLD_BASE 0x41

#define SENSOR_PACKET_SYNC 0xFF
#define SENSOR_PACKET_ESCAPE 0xFD
#define SENSOR_PACKET_CMD_REPORT 0x81
#define SENSOR_PACKET_VERSION 0x01
#define SENSOR_PACKET_PAYLOAD_SIZE \
    (4 + (TOUCH_SURFACE_COUNT * 2) + (IR_PIN_COUNT * 2))

typedef struct {
    uint8_t i2c_addr;
    uint8_t enabled_electrodes;
    bool ready;
} mpr121_device_t;

typedef struct {
    uint8_t device_index;
    uint8_t electrode;
} touch_channel_map_t;

typedef struct {
    gpio_num_t pin;
    bool adc_supported;
    adc_unit_t adc_unit;
    adc_channel_t adc_channel;
} ir_input_t;

static mpr121_device_t s_touch_devices[TOUCH_DEVICE_COUNT] = {
    {
        .i2c_addr = MPR121_ADDR_GND,
        .enabled_electrodes = 12,
        .ready = false,
    },
    {
        .i2c_addr = MPR121_ADDR_SDA,
        .enabled_electrodes = 4,
        .ready = false,
    },
};

// Touch channels 1..16, ordered right-to-left to match in-game order 1..32.
static const touch_channel_map_t s_touch_map[TOUCH_SURFACE_COUNT] = {
    {0, 0},
    {0, 1},
    {0, 2},
    {0, 3},
    {0, 4},
    {0, 5},
    {0, 6},
    {0, 7},
    {0, 8},
    {0, 9},
    {0, 10},
    {0, 11},
    {1, 0},
    {1, 1},
    {1, 2},
    {1, 3},
};

static const gpio_num_t s_ir_pins[IR_PIN_COUNT] = {
    GPIO_NUM_9,
    GPIO_NUM_10,
    GPIO_NUM_11,
    GPIO_NUM_12,
    GPIO_NUM_13,
    GPIO_NUM_14,
};

static ir_input_t s_ir_inputs[IR_PIN_COUNT];
static adc_oneshot_unit_handle_t s_adc1_handle;
static adc_oneshot_unit_handle_t s_adc2_handle;
static bool s_adc1_ready;
static bool s_adc2_ready;

static esp_err_t mpr121_write_reg(uint8_t addr, uint8_t reg, uint8_t value)
{
    const uint8_t payload[2] = {reg, value};
    return i2c_master_write_to_device(
        MPR121_I2C_PORT,
        addr,
        payload,
        sizeof(payload),
        pdMS_TO_TICKS(20));
}

static esp_err_t mpr121_read_regs(uint8_t addr, uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_write_read_device(
        MPR121_I2C_PORT,
        addr,
        &reg,
        1,
        buf,
        len,
        pdMS_TO_TICKS(20));
}

static esp_err_t mpr121_init_device(uint8_t addr, uint8_t enabled_electrodes)
{
    esp_err_t err = mpr121_write_reg(addr, MPR121_REG_SOFTRESET, 0x63);
    if (err != ESP_OK) {
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(1));

    err = mpr121_write_reg(addr, MPR121_REG_ELE_CFG, 0x00);
    if (err != ESP_OK) {
        return err;
    }

    for (uint8_t i = 0; i < 12; i++) {
        const uint8_t base = (uint8_t) (MPR121_REG_TOUCH_THRESHOLD_BASE + (i * 2));

        err = mpr121_write_reg(addr, base, MPR121_TOUCH_THRESHOLD);
        if (err != ESP_OK) {
            return err;
        }

        err = mpr121_write_reg(addr, (uint8_t) (base + 1), MPR121_RELEASE_THRESHOLD);
        if (err != ESP_OK) {
            return err;
        }
    }

    err = mpr121_write_reg(addr, MPR121_REG_DEBOUNCE, 0x00);
    if (err != ESP_OK) {
        return err;
    }

    err = mpr121_write_reg(addr, MPR121_REG_CONFIG1, 0x10);
    if (err != ESP_OK) {
        return err;
    }

    err = mpr121_write_reg(addr, MPR121_REG_CONFIG2, 0x20);
    if (err != ESP_OK) {
        return err;
    }

    return mpr121_write_reg(
        addr,
        MPR121_REG_ELE_CFG,
        (uint8_t) (0x80 | (enabled_electrodes & 0x0F)));
}

static bool mpr121_read_touch_status(uint8_t addr, uint16_t *status)
{
    uint8_t status_regs[2] = {0};
    esp_err_t err = mpr121_read_regs(addr, MPR121_REG_TOUCH_STATUS_L, status_regs, sizeof(status_regs));

    if (err != ESP_OK) {
        return false;
    }

    *status =
        ((uint16_t) status_regs[0] | ((uint16_t) status_regs[1] << 8))
        & 0x0FFF;
    return true;
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
    const uint16_t touch_values[TOUCH_SURFACE_COUNT],
    const uint16_t ir_values[IR_PIN_COUNT])
{
    uint8_t payload[SENSOR_PACKET_PAYLOAD_SIZE];
    size_t cursor = 0;

    payload[cursor++] = SENSOR_PACKET_VERSION;
    payload[cursor++] = seq;
    payload[cursor++] = ir_valid_mask;
    payload[cursor++] = 0;

    for (int i = 0; i < TOUCH_SURFACE_COUNT; i++) {
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

static void touch_read_all(uint16_t values[TOUCH_SURFACE_COUNT])
{
    uint16_t touch_status[TOUCH_DEVICE_COUNT] = {0};
    uint8_t filtered_data[TOUCH_DEVICE_COUNT][MPR121_ELECTRODE_COUNT * 2] = {{0}};
    uint8_t baseline_data[TOUCH_DEVICE_COUNT][MPR121_ELECTRODE_COUNT] = {{0}};
    bool device_valid[TOUCH_DEVICE_COUNT] = {false};

    for (int i = 0; i < TOUCH_DEVICE_COUNT; i++) {
        if (!s_touch_devices[i].ready) {
            continue;
        }

        if (!mpr121_read_touch_status(s_touch_devices[i].i2c_addr, &touch_status[i])) {
            continue;
        }

        esp_err_t err = mpr121_read_regs(
            s_touch_devices[i].i2c_addr,
            MPR121_REG_FILTERED_DATA_BASE,
            filtered_data[i],
            sizeof(filtered_data[i]));
        if (err != ESP_OK) {
            continue;
        }

        err = mpr121_read_regs(
            s_touch_devices[i].i2c_addr,
            MPR121_REG_BASELINE_BASE,
            baseline_data[i],
            sizeof(baseline_data[i]));
        if (err != ESP_OK) {
            continue;
        }

        device_valid[i] = true;
    }

    for (int i = 0; i < TOUCH_SURFACE_COUNT; i++) {
        const touch_channel_map_t map = s_touch_map[i];

        if (map.device_index >= TOUCH_DEVICE_COUNT || !device_valid[map.device_index]) {
            values[i] = 0;
            continue;
        }

        const uint8_t electrode = map.electrode;
        const uint8_t byte_index = (uint8_t) (electrode * 2);
        const uint16_t filtered =
            (uint16_t) filtered_data[map.device_index][byte_index]
            | ((uint16_t) filtered_data[map.device_index][byte_index + 1] << 8);
        const uint16_t baseline = (uint16_t) baseline_data[map.device_index][electrode] << 2;

        int32_t delta = (int32_t) baseline - (int32_t) filtered;
        if (delta < 0) {
            delta = 0;
        }

        if (delta <= MPR121_DELTA_DEADZONE) {
            values[i] = 0;
            continue;
        }

        uint32_t scaled = (uint32_t) (delta - MPR121_DELTA_DEADZONE) * 255u / MPR121_DELTA_FULL_SCALE;
        if (scaled > 255u) {
            scaled = 255u;
        }

        const uint16_t touch_bit = (uint16_t) (1u << electrode);
        if ((touch_status[map.device_index] & touch_bit) != 0 && scaled == 0u) {
            scaled = 1u;
        }

        values[i] = (uint16_t) scaled;
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
    i2c_config_t i2c_cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = MPR121_I2C_SDA_PIN,
        .scl_io_num = MPR121_I2C_SCL_PIN,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = MPR121_I2C_FREQ_HZ,
    };

    ESP_ERROR_CHECK(i2c_param_config(MPR121_I2C_PORT, &i2c_cfg));
    ESP_ERROR_CHECK(i2c_driver_install(MPR121_I2C_PORT, i2c_cfg.mode, 0, 0, 0));

    for (int i = 0; i < TOUCH_DEVICE_COUNT; i++) {
        esp_err_t err = mpr121_init_device(
            s_touch_devices[i].i2c_addr,
            s_touch_devices[i].enabled_electrodes);

        if (err == ESP_OK) {
            s_touch_devices[i].ready = true;
        } else {
            s_touch_devices[i].ready = false;
            printf(
                "MPR121 init failed addr=0x%02X (%s)\n",
                s_touch_devices[i].i2c_addr,
                esp_err_to_name(err));
        }
    }
}

void app_main(void)
{
    // Match host serial baud rate to maximize report throughput.
    ESP_ERROR_CHECK(uart_set_baudrate(UART_NUM_0, SENSOR_LINK_BAUD_RATE));

    touch_init_all();
    ir_init_all();

    uint8_t seq = 0;
    uint16_t touch_values[TOUCH_SURFACE_COUNT];
    uint16_t ir_values[IR_PIN_COUNT];

    while (1) {
        touch_read_all(touch_values);
        uint8_t ir_valid_mask = ir_read_all(ir_values);
        packet_send_sensor_report(seq++, ir_valid_mask, touch_values, ir_values);
    }
}
