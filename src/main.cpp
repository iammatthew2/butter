#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "driver/i2c.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_rom_sys.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "nvs_flash.h"
#include "secrets.h"

namespace {

static const char* TAG = "butter";

constexpr i2c_port_t kI2cPort = I2C_NUM_0;
constexpr gpio_num_t kI2cSda = GPIO_NUM_22;  // D4
constexpr gpio_num_t kI2cScl = GPIO_NUM_23;  // D5
constexpr uint32_t kI2cClockHz = 50000;
constexpr uint8_t kPiPlateMcpAddress = 0x20;

constexpr int kLcdCols = 16;
constexpr int kLcdRows = 2;
constexpr int kMaxTopicChars = 63;
constexpr int kMaxPayloadChars = 63;

constexpr EventBits_t kWiFiConnectedBit = BIT0;
constexpr EventBits_t kMqttConnectedBit = BIT1;

// MCP23017 registers (bank=0, sequential mode)
constexpr uint8_t kRegIodirA = 0x00;
constexpr uint8_t kRegIodirB = 0x01;
constexpr uint8_t kRegGppuA = 0x0C;
constexpr uint8_t kRegGpioA = 0x12;
constexpr uint8_t kRegGpioAB = 0x12;

// Bit indices in GPIOAB space (A=0..7, B=8..15)
constexpr uint8_t kPinBlue = 8;
constexpr uint8_t kPinGreen = 7;
constexpr uint8_t kPinRed = 6;
constexpr uint8_t kPinD7 = 9;
constexpr uint8_t kPinD6 = 10;
constexpr uint8_t kPinD5 = 11;
constexpr uint8_t kPinD4 = 12;
constexpr uint8_t kPinEnable = 13;
constexpr uint8_t kPinRw = 14;
constexpr uint8_t kPinRs = 15;

uint16_t g_gpioab_shadow = 0;
EventGroupHandle_t g_state_bits = nullptr;
SemaphoreHandle_t g_lcd_mutex = nullptr;
SemaphoreHandle_t g_message_mutex = nullptr;
esp_mqtt_client_handle_t g_mqtt_client = nullptr;

bool g_has_new_message = false;
char g_last_topic[kMaxTopicChars + 1] = {0};
char g_last_payload[kMaxPayloadChars + 1] = {0};

esp_err_t mcp_write_reg(uint8_t reg, uint8_t value) {
  const uint8_t payload[2] = {reg, value};
  return i2c_master_write_to_device(kI2cPort, kPiPlateMcpAddress, payload,
                                    sizeof(payload), pdMS_TO_TICKS(50));
}

esp_err_t mcp_read_reg(uint8_t reg, uint8_t* value) {
  return i2c_master_write_read_device(kI2cPort, kPiPlateMcpAddress, &reg,
                                      sizeof(reg), value, 1, pdMS_TO_TICKS(50));
}

esp_err_t mcp_write_gpioab(uint16_t value) {
  const uint8_t payload[3] = {kRegGpioAB, static_cast<uint8_t>(value & 0xFF),
                              static_cast<uint8_t>((value >> 8) & 0xFF)};
  g_gpioab_shadow = value;
  return i2c_master_write_to_device(kI2cPort, kPiPlateMcpAddress, payload,
                                    sizeof(payload), pdMS_TO_TICKS(50));
}

inline void lcd_set_bit(uint8_t pin, bool high) {
  if (high) {
    g_gpioab_shadow |= static_cast<uint16_t>(1U << pin);
  } else {
    g_gpioab_shadow &= static_cast<uint16_t>(~(1U << pin));
  }
}

esp_err_t lcd_apply_shadow() { return mcp_write_gpioab(g_gpioab_shadow); }

esp_err_t lcd_set_backlight(uint8_t rgb_mask) {
  // Backlight pins are active-low in the Adafruit shield design.
  lcd_set_bit(kPinBlue, ((rgb_mask >> 2) & 0x01U) == 0);
  lcd_set_bit(kPinGreen, ((rgb_mask >> 1) & 0x01U) == 0);
  lcd_set_bit(kPinRed, (rgb_mask & 0x01U) == 0);
  return lcd_apply_shadow();
}

esp_err_t lcd_write4bits(uint8_t value) {
  lcd_set_bit(kPinD4, (value & 0x01U) != 0);
  lcd_set_bit(kPinD5, (value & 0x02U) != 0);
  lcd_set_bit(kPinD6, (value & 0x04U) != 0);
  lcd_set_bit(kPinD7, (value & 0x08U) != 0);

  lcd_set_bit(kPinEnable, false);
  esp_err_t err = lcd_apply_shadow();
  if (err != ESP_OK) {
    return err;
  }

  esp_rom_delay_us(1);
  lcd_set_bit(kPinEnable, true);
  err = lcd_apply_shadow();
  if (err != ESP_OK) {
    return err;
  }

  esp_rom_delay_us(1);
  lcd_set_bit(kPinEnable, false);
  err = lcd_apply_shadow();
  if (err != ESP_OK) {
    return err;
  }

  esp_rom_delay_us(100);
  return ESP_OK;
}

esp_err_t lcd_send(uint8_t value, bool rs) {
  lcd_set_bit(kPinRs, rs);
  lcd_set_bit(kPinRw, false);

  esp_err_t err = lcd_write4bits(static_cast<uint8_t>(value >> 4));
  if (err != ESP_OK) {
    return err;
  }
  return lcd_write4bits(static_cast<uint8_t>(value & 0x0F));
}

esp_err_t lcd_command(uint8_t cmd) { return lcd_send(cmd, false); }

esp_err_t lcd_write_char(char c) {
  return lcd_send(static_cast<uint8_t>(c), true);
}

esp_err_t lcd_print(const char* text) {
  for (size_t i = 0; i < strlen(text); ++i) {
    const esp_err_t err = lcd_write_char(text[i]);
    if (err != ESP_OK) {
      return err;
    }
  }
  return ESP_OK;
}

esp_err_t lcd_set_cursor(uint8_t col, uint8_t row) {
  const uint8_t offsets[2] = {0x00, 0x40};
  row = (row > 1) ? 1 : row;
  return lcd_command(static_cast<uint8_t>(0x80U | (col + offsets[row])));
}

void to_lcd_line(const char* in, char out[kLcdCols + 1]) {
  int out_i = 0;
  for (int in_i = 0; in[in_i] != '\0' && out_i < kLcdCols; ++in_i) {
    const unsigned char c = static_cast<unsigned char>(in[in_i]);
    out[out_i++] = isprint(c) ? static_cast<char>(c) : '.';
  }
  while (out_i < kLcdCols) {
    out[out_i++] = ' ';
  }
  out[kLcdCols] = '\0';
}

esp_err_t lcd_write_line(uint8_t row, const char* text) {
  char line[kLcdCols + 1];
  to_lcd_line(text, line);
  esp_err_t err = lcd_set_cursor(0, row);
  if (err != ESP_OK) {
    return err;
  }
  return lcd_print(line);
}

void lcd_show_two_lines(const char* line0, const char* line1) {
  if (g_lcd_mutex == nullptr) {
    return;
  }

  if (xSemaphoreTake(g_lcd_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
    return;
  }

  const esp_err_t err0 = lcd_write_line(0, line0);
  const esp_err_t err1 = lcd_write_line(1, line1);
  if (err0 != ESP_OK || err1 != ESP_OK) {
    ESP_LOGW(TAG, "LCD write failed (line0=%s line1=%s)", esp_err_to_name(err0),
             esp_err_to_name(err1));
  }

  xSemaphoreGive(g_lcd_mutex);
}

esp_err_t lcd_init() {
  // A0..A4 are buttons (inputs). A5..A7 and all B are outputs.
  esp_err_t err = mcp_write_reg(kRegIodirA, 0x1F);
  if (err != ESP_OK) {
    return err;
  }
  err = mcp_write_reg(kRegIodirB, 0x00);
  if (err != ESP_OK) {
    return err;
  }
  err = mcp_write_reg(kRegGppuA, 0x1F);
  if (err != ESP_OK) {
    return err;
  }

  g_gpioab_shadow = 0;
  err = lcd_set_backlight(0x07);
  if (err != ESP_OK) {
    return err;
  }

  lcd_set_bit(kPinRs, false);
  lcd_set_bit(kPinRw, false);
  lcd_set_bit(kPinEnable, false);
  err = lcd_apply_shadow();
  if (err != ESP_OK) {
    return err;
  }

  // HD44780 4-bit startup sequence.
  vTaskDelay(pdMS_TO_TICKS(50));
  err = lcd_write4bits(0x03);
  if (err != ESP_OK) {
    return err;
  }
  esp_rom_delay_us(4500);

  err = lcd_write4bits(0x03);
  if (err != ESP_OK) {
    return err;
  }
  esp_rom_delay_us(4500);

  err = lcd_write4bits(0x03);
  if (err != ESP_OK) {
    return err;
  }
  esp_rom_delay_us(150);

  err = lcd_write4bits(0x02);
  if (err != ESP_OK) {
    return err;
  }

  err = lcd_command(0x28);  // Function set: 4-bit, 2-line, 5x8 dots.
  if (err != ESP_OK) {
    return err;
  }
  err = lcd_command(0x08);  // Display off.
  if (err != ESP_OK) {
    return err;
  }
  err = lcd_command(0x01);  // Clear display.
  if (err != ESP_OK) {
    return err;
  }
  vTaskDelay(pdMS_TO_TICKS(2));

  err = lcd_command(0x06);  // Entry mode set.
  if (err != ESP_OK) {
    return err;
  }
  err = lcd_command(0x0C);  // Display on, cursor off, blink off.
  if (err != ESP_OK) {
    return err;
  }

  return ESP_OK;
}

esp_err_t init_i2c_master() {
  const i2c_config_t conf = {
      .mode = I2C_MODE_MASTER,
      .sda_io_num = kI2cSda,
      .scl_io_num = kI2cScl,
      .sda_pullup_en = GPIO_PULLUP_ENABLE,
      .scl_pullup_en = GPIO_PULLUP_ENABLE,
      .master =
          {
              .clk_speed = kI2cClockHz,
          },
      .clk_flags = 0,
  };

  const esp_err_t config_err = i2c_param_config(kI2cPort, &conf);
  if (config_err != ESP_OK) {
    ESP_LOGE(TAG, "i2c_param_config failed: %s", esp_err_to_name(config_err));
    return config_err;
  }

  return i2c_driver_install(kI2cPort, conf.mode, 0, 0, 0);
}

bool i2c_address_responds(uint8_t address) {
  i2c_cmd_handle_t cmd = i2c_cmd_link_create();
  if (cmd == nullptr) {
    return false;
  }

  i2c_master_start(cmd);
  i2c_master_write_byte(
      cmd, static_cast<uint8_t>((address << 1) | I2C_MASTER_WRITE), true);
  i2c_master_stop(cmd);

  const esp_err_t err = i2c_master_cmd_begin(kI2cPort, cmd, pdMS_TO_TICKS(30));
  i2c_cmd_link_delete(cmd);
  return err == ESP_OK;
}

void remember_message(const char* topic, int topic_len, const char* data,
                      int data_len) {
  if (g_message_mutex == nullptr) {
    return;
  }

  if (xSemaphoreTake(g_message_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
    return;
  }

  const int topic_copy_len =
      (topic_len > kMaxTopicChars) ? kMaxTopicChars : topic_len;
  const int data_copy_len =
      (data_len > kMaxPayloadChars) ? kMaxPayloadChars : data_len;

  if (topic_copy_len > 0) {
    memcpy(g_last_topic, topic, static_cast<size_t>(topic_copy_len));
  }
  g_last_topic[topic_copy_len] = '\0';

  if (data_copy_len > 0) {
    memcpy(g_last_payload, data, static_cast<size_t>(data_copy_len));
  }
  g_last_payload[data_copy_len] = '\0';

  g_has_new_message = true;
  xSemaphoreGive(g_message_mutex);
}

void wifi_event_handler(void* arg, esp_event_base_t event_base,
                        int32_t event_id, void* event_data) {
  (void)arg;
  (void)event_data;

  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    ESP_LOGI(TAG, "Wi-Fi start, connecting...");
    esp_wifi_connect();
    lcd_show_two_lines("WiFi starting", "Connecting...");
    return;
  }

  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
    xEventGroupClearBits(g_state_bits, kWiFiConnectedBit | kMqttConnectedBit);
    ESP_LOGW(TAG, "Wi-Fi disconnected, reconnecting...");
    lcd_show_two_lines("WiFi disconnected", "Retrying...");
    esp_wifi_connect();
    return;
  }

  if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    xEventGroupSetBits(g_state_bits, kWiFiConnectedBit);
    const auto* got_ip = static_cast<ip_event_got_ip_t*>(event_data);
    ESP_LOGI(TAG, "Wi-Fi connected, IP=" IPSTR, IP2STR(&got_ip->ip_info.ip));
    lcd_show_two_lines("WiFi connected", "MQTT pending");
  }
}

void mqtt_event_handler(void* handler_args, esp_event_base_t base,
                        int32_t event_id, void* event_data) {
  (void)handler_args;
  (void)base;
  auto* event = static_cast<esp_mqtt_event_handle_t>(event_data);

  switch (static_cast<esp_mqtt_event_id_t>(event_id)) {
    case MQTT_EVENT_CONNECTED:
      xEventGroupSetBits(g_state_bits, kMqttConnectedBit);
      ESP_LOGI(TAG, "MQTT connected, subscribing to '%s'", MQTT_EVENTS_TOPIC);
      esp_mqtt_client_subscribe(g_mqtt_client, MQTT_EVENTS_TOPIC, 0);
      lcd_show_two_lines("MQTT connected", "Listening events");
      break;

    case MQTT_EVENT_DISCONNECTED:
      xEventGroupClearBits(g_state_bits, kMqttConnectedBit);
      ESP_LOGW(TAG, "MQTT disconnected");
      lcd_show_two_lines("MQTT disconnected", "Retrying...");
      break;

    case MQTT_EVENT_DATA:
      ESP_LOGI(TAG, "MQTT message topic='%.*s' payload='%.*s'",
               event->topic_len, event->topic, event->data_len, event->data);
      remember_message(event->topic, event->topic_len, event->data,
                       event->data_len);
      break;

    default:
      break;
  }
}

void wifi_init_sta() {
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_create_default_wifi_sta();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                             &wifi_event_handler, nullptr));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                             &wifi_event_handler, nullptr));

  wifi_config_t wifi_config = {};
  strncpy(reinterpret_cast<char*>(wifi_config.sta.ssid), WIFI_SSID,
          sizeof(wifi_config.sta.ssid) - 1);
  strncpy(reinterpret_cast<char*>(wifi_config.sta.password), WIFI_PASSWORD,
          sizeof(wifi_config.sta.password) - 1);
  wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
  wifi_config.sta.pmf_cfg.capable = true;
  wifi_config.sta.pmf_cfg.required = false;

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
  ESP_ERROR_CHECK(esp_wifi_start());
}

void mqtt_start() {
  char mqtt_uri[96];
  snprintf(mqtt_uri, sizeof(mqtt_uri), "mqtt://%s:%u", MQTT_BROKER, MQTT_PORT);

  const esp_mqtt_client_config_t mqtt_cfg = {
      .broker =
          {
              .address =
                  {
                      .uri = mqtt_uri,
                  },
          },
      .credentials =
          {
              .client_id = MQTT_CLIENT_ID,
          },
  };

  g_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
  ESP_ERROR_CHECK(esp_mqtt_client_register_event(g_mqtt_client, MQTT_EVENT_ANY,
                                                 mqtt_event_handler, nullptr));
  ESP_ERROR_CHECK(esp_mqtt_client_start(g_mqtt_client));
}

void handle_latest_message_for_lcd() {
  if (g_message_mutex == nullptr) {
    return;
  }

  char topic[kMaxTopicChars + 1];
  char payload[kMaxPayloadChars + 1];
  bool has_new = false;

  if (xSemaphoreTake(g_message_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    has_new = g_has_new_message;
    if (has_new) {
      strncpy(topic, g_last_topic, sizeof(topic) - 1);
      topic[sizeof(topic) - 1] = '\0';
      strncpy(payload, g_last_payload, sizeof(payload) - 1);
      payload[sizeof(payload) - 1] = '\0';
      g_has_new_message = false;
    }
    xSemaphoreGive(g_message_mutex);
  }

  if (!has_new) {
    return;
  }

  char line0[kLcdCols + 1];
  char line1[kLcdCols + 1];

  memset(line0, ' ', sizeof(line0) - 1);
  memset(line1, ' ', sizeof(line1) - 1);
  line0[kLcdCols] = '\0';
  line1[kLcdCols] = '\0';

  line0[0] = 'T';
  line0[1] = ':';
  line1[0] = 'M';
  line1[1] = ':';

  strncpy(&line0[2], topic, kLcdCols - 2);
  strncpy(&line1[2], payload, kLcdCols - 2);

  lcd_show_two_lines(line0, line1);
}

void init_nvs() {
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);
}

}  // namespace

extern "C" void app_main(void) {
  ESP_LOGI(TAG, "Project Butter startup");
  ESP_LOGI(TAG, "I2C config: SDA=%d SCL=%d freq=%luHz", kI2cSda, kI2cScl,
           static_cast<unsigned long>(kI2cClockHz));

  g_state_bits = xEventGroupCreate();
  g_lcd_mutex = xSemaphoreCreateMutex();
  g_message_mutex = xSemaphoreCreateMutex();

  init_nvs();

  ESP_ERROR_CHECK(init_i2c_master());

  if (!i2c_address_responds(kPiPlateMcpAddress)) {
    ESP_LOGE(TAG, "Pi Plate expander 0x%02X not detected", kPiPlateMcpAddress);
    return;
  }

  ESP_ERROR_CHECK(lcd_init());
  lcd_show_two_lines("Butter booting", "Init network");

  wifi_init_sta();
  mqtt_start();

  while (true) {
    handle_latest_message_for_lcd();
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}
