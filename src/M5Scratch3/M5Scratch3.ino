#include <Wire.h>

#include <M5Unified.h>
#include <M5Dial.h>
//#include <M5StackUpdater.h>

//// Mic for M5StickC/Plus
#include <driver/i2s.h>

/// Drawing Function for M5Stack
#define Draw M5.Lcd

//// BLE related headers.
#include <NimBLEDevice.h>

//// Global variables.
// For Stack-chan
bool stackchan_mode = false;

// Screen size
int screen_w = 320;
int screen_h = 240;

// Converter 32bit float little endian and uint8_t x 4 each other.
static union {
  uint8_t b[sizeof(float)];
  float f;
} cd;

// M5Stack can handle PortA, PortB, PortC
int pin[6];

enum pin_mode_t {
  PIN_ANALOG_INPUT,
  PIN_ANALOG_OUTPUT,
  PIN_DIGITAL_INPUT,
  PIN_DIGITAL_OUTPUT,
  PIN_PWM,
  PIN_SERVO,
  PIN_PULL,
  PIN_EVENT
};

pin_mode_t pin_mode[6] = { PIN_ANALOG_INPUT };

//// Global variables for M5Stack.
// Board name
m5::board_t myBoard = m5gfx::board_unknown;

//// FastLED for Atom Matrix, Lite.
#if !defined(CONFIG_IDF_TARGET_ESP32S3)
#include <FastLED.h>
#define NUM_LEDS 25
#define LED_DATA_PIN 27
CRGB leds[NUM_LEDS];
#endif

#define PIN_CLK 0
#define PIN_DATA 34
#define READ_LEN (2 * 256)
#define SAMPLING_RATE 11025
uint8_t BUFFER[READ_LEN] = { 0 };

int16_t *adcBuffer = NULL;
int soundLevel = 0;

void i2sInit() {
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_PDM),
    .sample_rate = SAMPLING_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,  // is fixed at 12bit, stereo, MSB
    .channel_format = I2S_CHANNEL_FMT_ALL_RIGHT,
    .communication_format = I2S_COMM_FORMAT_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 2,
    .dma_buf_len = 128,
  };

  i2s_pin_config_t pin_config;
  pin_config.bck_io_num = I2S_PIN_NO_CHANGE;
  pin_config.ws_io_num = PIN_CLK;
  pin_config.data_out_num = I2S_PIN_NO_CHANGE;
  pin_config.data_in_num = PIN_DATA;

  i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &pin_config);
  i2s_set_clk(I2S_NUM_0, SAMPLING_RATE, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_MONO);
}

void mic_record_task(void *arg) {
  size_t bytesread;
  while (1) {
    int total = 0;
    i2s_read(I2S_NUM_0, (char *)BUFFER, READ_LEN, &bytesread, (100 / portTICK_RATE_MS));
    adcBuffer = (int16_t *)BUFFER;
    for (int i = 0; i < READ_LEN / 2; i++) {
      total += abs(adcBuffer[i]);
    }
    soundLevel = total / (READ_LEN / 2);
    vTaskDelay(100 / portTICK_RATE_MS);
  }
}

//// BLE characteristics.
#define MBIT_MORE_SERVICE "0b50f3e4-607f-4151-9091-7d008d6ffc5c"
#define MBIT_MORE_CH_COMMAND "0b500100-607f-4151-9091-7d008d6ffc5c"       // R&W(20byte)
#define MBIT_MORE_CH_STATE "0b500101-607f-4151-9091-7d008d6ffc5c"         // R(7byte)
#define MBIT_MORE_CH_MOTION "0b500102-607f-4151-9091-7d008d6ffc5c"        // R(18byte)    :pitch,roll,accel,and gyro
#define MBIT_MORE_CH_PIN_EVENT "0b500110-607f-4151-9091-7d008d6ffc5c"     // R&N
#define MBIT_MORE_CH_ACTION_EVENT "0b500111-607f-4151-9091-7d008d6ffc5c"  // R&N(20byte)  :Buttons with timestamp
#define MBIT_MORE_CH_ANALOG_IN_P0 "0b500120-607f-4151-9091-7d008d6ffc5c"  // R
#define MBIT_MORE_CH_ANALOG_IN_P1 "0b500121-607f-4151-9091-7d008d6ffc5c"  // R
#define MBIT_MORE_CH_ANALOG_IN_P2 "0b500122-607f-4151-9091-7d008d6ffc5c"  // R
#define MBIT_MORE_CH_ANALOG_IN_P3 "0b500123-607f-4151-9091-7d008d6ffc5c"  // R
/* UART
#define MBIT_MORE_CH_ANALOG_IN_P4 "0b500124-607f-4151-9091-7d008d6ffc5c"  // R
#define MBIT_MORE_CH_ANALOG_IN_P5 "0b500125-607f-4151-9091-7d008d6ffc5c"  // R
*/
#define MBIT_MORE_CH_MESSAGE "0b500130-607f-4151-9091-7d008d6ffc5c"  // R : only for v2
#define ADVERTISING_STRING "BBC micro:bit [m5scr]"

/// Data of channels.
// COMMAND CH 20byte
uint8_t cmd[] = { 0x02,  // microbit version (v1:0x01, v2:0x02)
                  0x02,  // protocol 0x02 only
                  0x00, 0x00, 0x00, 0x00, 0x00,
                  0x00, 0x00, 0x00, 0x00, 0x00,
                  0x00, 0x00, 0x00, 0x00, 0x00,
                  0x00, 0x00, 0x00 };

// STATE CH 7byte
uint8_t state[] = {
  0x00, 0x00, 0x00, 0x00,  // GPIO 0-3
  0x00,                    // lightlevel
  0x00,                    // temperature(+128)
  0x00                     // soundlevel
};

// MOTION CH 24 byte
uint8_t motion[] = {
  0x00, 0x00,  // pitch
  0x00, 0x00,  // roll
  0xff, 0xff,  // ax
  0xff, 0x00,  // ay
  0x00, 0xff,  // az
  0x00, 0x00,  // gx
  0x00, 0x00,  // gy
  0x00, 0x00,  // gz
  0x00, 0x00,  // tx
  0x00, 0x00,  // ty
  0x00, 0x00,  // enc
  0x00, 0x00   // ??
};

// ACTION CH 20 byte
uint8_t action[] = {
  0x01,                    // BUTTON cmd; BUTTON:0x01, GESTURE: 0x02
  0x01, 0x00,              // Button Name;1:A,2:B,3:C,100:P0,101:P1,102:P2,121:LOGO
  0x00,                    // Event Name;1:DOWN, 2:UP, 3:CLICK, 4:LONG_CLICK, 5:HOLD, 6:DOUBLE_CLICK
  0x00, 0x00, 0x00, 0x00,  // Timestamp
  0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00,
  0x12  // ACTION Event
};
// Define for label command
#define DATA_NUMBER 0x13
#define DATA_TEXT 0x14

// ANALOG PIN 2 byte
uint8_t analog[] = { 0x00, 0x00 };

NimBLEServer *pServer = NULL;
NimBLECharacteristic *pCharacteristic[10] = { 0 };
bool deviceConnected = false;

// for pixel pattern
#define TEXT_SPACE 30
uint16_t pixel[5][5] = { 0 };

void drawPixel(int x, int y, int c) {
  int ps = (screen_w < (screen_h - TEXT_SPACE)) ? screen_w / 5 : (screen_h - TEXT_SPACE) / 5;  // Pixel size

  if (c == TFT_RED && (myBoard == m5gfx::board_M5StackCoreInk || myBoard == m5gfx::board_M5Paper)) {
    Draw.fillRect(x * ps, y * ps + TEXT_SPACE, ps, ps, TFT_WHITE);
  } else {
    Draw.fillRect(x * ps, y * ps + TEXT_SPACE, ps, ps, c);
  }

#if !defined(CONFIG_IDF_TARGET_ESP32S3)
  if (myBoard == m5gfx::board_M5Atom) {
    if (c == TFT_BLACK) {
      leds[x + y * 5] = CRGB::Black;
    } else if (c == TFT_RED) {
      leds[x + y * 5] = CRGB::Red;
    } else if (c == TFT_BLUE) {
      leds[x + y * 5] = CRGB::Blue;
    }
    FastLED.show();
  }
#endif
};

void displayShowPixel() {
  for (int y = 0; y < 5; y++) {
    for (int x = 0; x < 5; x++) {
      log_i("%1d", pixel[y][x] & 0b1);
      if (pixel[y][x] & 0b1) {
        drawPixel(x, y, TFT_RED);
      } else {
        drawPixel(x, y, TFT_BLACK);
      }
    }
  }
};

void fillScreen(int c) {
  Draw.fillScreen(c);
  // for Atom Matrix and Lite
  if (myBoard == m5gfx::board_M5Atom) {
    for (int x = 0; x < 5; x++) {
      for (int y = 0; y < 5; y++) {
        drawPixel(x, y, c);
      }
    }
  }
};

class MyServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *pServer, NimBLEConnInfo &connInfo) override {
    log_i("connect\n");
    deviceConnected = true;
    fillScreen(TFT_BLACK);
  };

  void onDisconnect(NimBLEServer *pServer, NimBLEConnInfo &connInfo, int reason) override {
    log_i("disconnect\n");
    deviceConnected = false;
    ESP.restart();
  }
} myServerCallbacks;

// dummy callback
class DummyCallbacks : public NimBLECharacteristicCallbacks {
  void onRead(NimBLECharacteristic *pCharacteristic, NimBLEConnInfo &connInfo) override {
    log_i("DUMMY Read\n");
  }
  void onWrite(NimBLECharacteristic *pCharacteristic, NimBLEConnInfo &connInfo) override {
    log_i("DUMMY Write\n");
  }
} dummyCallbacks;

// for cmd
// Global variable for drawing graphics using data & label.
uint32_t label_flag = 0;
uint32_t x_0, y_0, x_1, y_1, x_2, y_2 = 0;  // (x,y) axis
uint32_t x_c, y_c = 0;                      // position of cursor for text
String str = "";                            // String for text output
uint32_t size = 1;                          // text size
uint32_t tc = 0;                            // text color
uint32_t w, h = 0;                          // width & height
uint32_t r = 0;                             // radius for circle
uint32_t c = 0;                             // color

void getLabelDataValue(char *var_name, String label_str, uint32_t *var, int data_val) {
  if (!label_str.compareTo(var_name)) {
    *var = data_val;
  }
}

// Stackchan Draw command
int norm_x(int x) {
  return (int(x / 320.0 * screen_w));
}

int norm_y(int y) {
  return (int(y / 240.0 * screen_h));
}

void clear_eyes() {
  Draw.fillRect(norm_x(0), norm_y(0), norm_x(320), norm_y(120), TFT_BLACK);
}

void clear_mouth() {
  Draw.fillRect(norm_x(0), norm_y(120), norm_x(320), norm_y(120), TFT_BLACK);
}

void draw_eye() {
  clear_eyes();
  Draw.fillCircle(norm_x(90), norm_y(93), norm_y(8), TFT_WHITE);
  Draw.fillCircle(norm_x(230), norm_y(96), norm_y(8), TFT_WHITE);
}

void draw_closeeye() {
  clear_eyes();
  Draw.fillRect(norm_x(82), norm_y(93), norm_x(16), norm_y(4), TFT_WHITE);
  Draw.fillRect(norm_x(222), norm_y(93), norm_x(16), norm_y(4), TFT_WHITE);
}

void draw_mouth() {
  clear_mouth();
  Draw.fillRect(norm_x(163 - 45), norm_y(148), norm_x(90), norm_y(4), TFT_WHITE);
}

void draw_openmouth() {
  clear_mouth();
  Draw.fillRect(norm_x(140), norm_y(130), norm_x(40), norm_y(40), TFT_WHITE);
}

// Microbit More Command handling
class CmdCallbacks : public NimBLECharacteristicCallbacks {
  void onRead(NimBLECharacteristic *pCharacteristic, NimBLEConnInfo &connInfo) override {
    log_i("CMD read\n");
    pCharacteristic->setValue(cmd, 20);
  }

  void cmd_pin(const char *cmd_str) {
    char pin_cmd = (cmd_str[0] & 0x0f);
    int pin_num = cmd_str[1];
    int pin_value = cmd_str[2];

    log_i("CMD_PIN\n");
    log_i(" pin:%d, dat:%d\n", pin_num, pin_value);

    switch (pin_cmd) {
      case 0x01:
        // OUTPUT
        log_i(" OUTPUT\n");
        pin_mode[pin_num] = PIN_DIGITAL_OUTPUT;
        pinMode(pin[pin_num], OUTPUT);
        digitalWrite(pin[pin_num], pin_value);
        break;
      case 0x02:
        // PWM
        log_i(" PWM\n");
        pin_mode[pin_num] = PIN_PWM;
        analogWrite(pin[pin_num], pin_value);
        break;
      case 0x03:
        // SERVO
        log_i(" SERVO\n");
        log_i("  range:%d, center:%d\n", cmd_str[3], cmd_str[4]);
        pin_mode[pin_num] = PIN_SERVO;
        analogWrite(pin[pin_num], pin_value / 1.80 + 2.5);  // The pin_value means angle for servo.
        break;
      case 0x04:
        // PULL
        log_i(" PULL\n");
        pin_mode[pin_num] = PIN_PULL;
        break;
      case 0x05:
        // EVENT
        log_i(" EVENT\n");
        pin_mode[pin_num] = PIN_EVENT;
        break;
    }
  }

  void display_text(const char *text) {
    log_i(">> text\n");
    log_i("%s\n", text);
    Draw.fillRect(0, 0, screen_w, TEXT_SPACE - 1, TFT_BLACK);
    if (stackchan_mode) {
      Draw.fillEllipse(0, 0, screen_w, TEXT_SPACE, TFT_WHITE);
      Draw.fillTriangle(screen_w / 2 - screen_w * 0.1, TEXT_SPACE * 0.8,
                        screen_w / 2, TEXT_SPACE * 1.5,
                        screen_w / 2 + screen_w * 0.1, TEXT_SPACE * 0.5, TFT_WHITE);
      Draw.setTextColor(TFT_BLACK);
    } else {
      Draw.setTextColor(TFT_WHITE);
    }

    int text_size = 4;
    if (myBoard == m5gfx::board_M5StickC || myBoard == m5gfx::board_M5AtomS3) {
      text_size = 2;
    } else if (myBoard == m5gfx::board_M5StickCPlus || myBoard == m5gfx::board_M5StickCPlus2) {
      text_size = 3;
    }

    Draw.setCursor(0, 0);
    Draw.setTextSize(text_size);
    Draw.println(text);
  }

  void cmd_display(const char *cmd_str) {
    char cmd_display = cmd_str[0] & 0b11111;
    switch (cmd_display) {
      case 0x00:
        // CLEAR
        log_i(">> clear\n");
        fillScreen(TFT_BLACK);
        break;
      case 0x01:
        // TEXT
        display_text(&(cmd_str[1]));
        break;
      case 0x02:
        // PIXELS_0
        log_i(">> pixel0\n");
        for (int y = 0; y < 3; y++) {
          for (int x = 0; x < 5; x++) {
            pixel[y][x] = (cmd_str[y * 5 + (x + 1)] & 0xb);
          }
        }
        break;
      case 0x03:
        // PIXELS_1
        log_i(">> pixel1\n");
        for (int y = 3; y < 5; y++) {
          for (int x = 0; x < 5; x++) {
            pixel[y][x] = (cmd_str[(y - 3) * 5 + (x + 1)] & 0xb);
          }
        }
        displayShowPixel();
        break;
    }
  }

  void cmd_audio(const char *cmd_str) {
    char cmd_audio = cmd_str[0] & 0b11111;
    switch (cmd_audio) {
      case 0x00:
        // STOP_TONE  0x00
        log_i(">> Stop tone\n");
        M5.Speaker.stop();
        break;
      case 0x01:
        // PLAY_TONE  0x01
        const uint8_t max_volume = 255;
        log_i(">> Play tone\n");
        uint32_t duration = (cmd_str[4] & 0xff) << 24
                            | (cmd_str[3] & 0xff) << 16
                            | (cmd_str[2] & 0xff) << 8
                            | (cmd_str[1] & 0xff);
        uint16_t freq = 1000000 / duration;
        uint8_t volume = map(cmd_str[5], 0, 255, 0, max_volume);
        log_i("Volume:%d\n", volume);
        log_i("Duration:%d\n", duration);
        log_i("Freq:%d\n", freq);
        M5.Speaker.setVolume(volume);
        M5.Speaker.tone(freq);
        break;
    }
  }

  void display_label_data(char *label, char *data, float data_val) {
    int label_location_x = 210;
    int label_location_y = 40;
    int font_height = 10;
    int text_size = 1;

    if (myBoard == m5gfx::board_M5Stack || myBoard == m5gfx::board_M5StackCore2 || myBoard == m5gfx::board_M5StackCoreS3) {
      label_location_x = 210;
      label_location_y = 40;
    } else if (myBoard == m5gfx::board_M5StickC || myBoard == m5gfx::board_M5StickCPlus || myBoard == m5gfx::board_M5StickCPlus2) {
      label_location_x = 0;
      if (myBoard == m5gfx::board_M5StickC) {
        label_location_y = 110;
      } else if (myBoard == m5gfx::board_M5StickCPlus || myBoard == m5gfx::board_M5StickCPlus2) {
        label_location_y = 170;
      }
    } else if (myBoard == m5gfx::board_M5Cardputer) {
      label_location_x = 0;
      label_location_y = 170;
    } else if (myBoard == m5gfx::board_M5AtomS3) {
      label_location_x = 0;
      label_location_y = 100;
    }
    Draw.setTextSize(text_size);
    Draw.setTextColor(TFT_WHITE);

    // Draw.fillRect(0, label_location_y, screen_w, screen_h - label_location_y, TFT_BLACK);
    // Draw.fillRect(label_location_x, label_location_y, screen_w - label_location_x, screen_h, TFT_BLACK);
    Draw.fillRect(label_location_x, label_location_y, screen_w - label_location_x, screen_h - label_location_y, TFT_BLACK);

    Draw.setCursor(label_location_x, label_location_y);
    Draw.printf("Label:%s\n", label);
    Draw.setCursor(label_location_x, label_location_y + font_height * 1);
    Draw.printf("Data:%s\n", data);
    Draw.setCursor(label_location_x, label_location_y + font_height * 2);
    Draw.printf(" val:");
    if (data_val < 100000) {
      Draw.printf("%8.2f", data_val);
    } else {
      Draw.printf("too big");
    }
  }

  void
  set_variables(String label_str, float data_val, String data_str) {
    // Display label & data?
    getLabelDataValue("label", label_str, &label_flag, data_val);
    // Store variables
    getLabelDataValue("x0", label_str, &x_0, data_val);
    getLabelDataValue("y0", label_str, &y_0, data_val);
    getLabelDataValue("x1", label_str, &x_1, data_val);
    getLabelDataValue("y1", label_str, &y_1, data_val);
    getLabelDataValue("x2", label_str, &x_2, data_val);
    getLabelDataValue("y2", label_str, &y_2, data_val);
    getLabelDataValue("xc", label_str, &x_c, data_val);
    getLabelDataValue("yc", label_str, &y_c, data_val);
    if (!label_str.compareTo("str")) {
      str = data_str;
    }
    getLabelDataValue("size", label_str, &size, data_val);
    getLabelDataValue("tc", label_str, &tc, data_val);
    getLabelDataValue("w", label_str, &w, data_val);
    getLabelDataValue("h", label_str, &h, data_val);
    getLabelDataValue("r", label_str, &r, data_val);
    getLabelDataValue("c", label_str, &c, data_val);
  }

  void label_draw_cmd(String label_str, String data_str) {
    if (!label_str.compareTo("cmd")) {
      if (!data_str.compareTo("drawPixel")) {
        Draw.drawPixel(x_0, y_0, c);
      } else if (!data_str.compareTo("drawLine")) {
        Draw.drawLine(x_0, y_0, x_1, y_1, c);
      } else if (!data_str.compareTo("drawRect")) {
        Draw.drawRect(x_0, y_0, w, h, c);
      } else if (!data_str.compareTo("drawTriangl")) {  // "drawTriangle" is over data length limit.
        Draw.drawTriangle(x_0, y_0, x_1, y_1, x_2, y_2, c);
      } else if (!data_str.compareTo("drawRoundRe")) {  // "drawRoundRect" is over data length limit.
        Draw.drawRoundRect(x_0, y_0, w, h, r, c);
      } else if (!data_str.compareTo("fillScreen")) {
        Draw.fillScreen(c);
      } else if (!data_str.compareTo("fillRect")) {
        Draw.fillRect(x_0, y_0, w, h, c);
      } else if (!data_str.compareTo("fillCircle")) {
        Draw.fillCircle(x_0, y_0, r, c);
      } else if (!data_str.compareTo("fillTriangl")) {  // "fillTriangle" is over data length limit.
        Draw.fillTriangle(x_0, y_0, x_1, y_1, x_2, y_2, c);
      } else if (!data_str.compareTo("fillRoundRe")) {  // "fillRoundRect" is over data length limit.
        Draw.fillRoundRect(x_0, y_0, w, h, r, c);
      } else if (!data_str.compareTo("print")) {
        Draw.setTextColor(tc);
        Draw.setTextSize(size);
        Draw.setCursor(x_c, y_c);
        Draw.print(str);
      }
    }
  }

  void label_stackchan_cmd(String label_str, String data_str) {
    if (!label_str.compareTo("stack")) {

      if (!data_str.compareTo("eye")) {
        draw_eye();
      } else if (!data_str.compareTo("closeeye")) {
        draw_closeeye();
      } else if (!data_str.compareTo("mouth")) {
        draw_mouth();
      } else if (!data_str.compareTo("openmouth")) {
        draw_openmouth();
      } else if (!data_str.compareTo("say")) {
        draw_openmouth();
        delay(10);
        draw_mouth();
        delay(10);
      } else if (!data_str.compareTo("on")) {
        stackchan_mode = true;
      } else if (!data_str.compareTo("off")) {
        stackchan_mode = false;
      }
    }
  }

  void cmd_data(const char *cmd_str) {
    log_i("CMD DATA\n");

    // Show input data.
    log_i(">>> Data input:");
    for (int i = 0; i <= 20; i++) {
      log_i("(%d)%02x%c:", i, cmd_str[i], cmd_str[i]);
    }
    log_i("\n");

    // Convert from input data to label & data.
    char label[9] = { 0 };
    strncpy(label, &cmd_str[1], sizeof(label) - 1);
    String label_str = String(label);

    char data[12] = { 0 };
    strncpy(data, &cmd_str[9], sizeof(data) - 1);
    String data_str = String(data);

    // Convert from 8bit uint8_t x 4 to 32bit float with little endian.
    cd.b[0] = cmd_str[9];
    cd.b[1] = cmd_str[10];
    cd.b[2] = cmd_str[11];
    cd.b[3] = cmd_str[12];
    float data_val = cd.f;

    log_i("Label str:%s, Data str:%s, Data value:%f.\n", label_str, data_str, data_val);

    // Can't get correct command for number=0x13 and text=0x14. Why?
    char cmd_data = cmd_str[20];
    if (cmd_data == 0x13) {
      log_i("Data is Number.\n");
    } else if (cmd_data == 0x14) {
      log_i("Data is Text.\n");
    } else {
      log_i("Data is Unknown:%02x.\n", cmd_data);
    }

    // Show label & data at display.
    if (label_flag != 0) {
      display_label_data(label, data, data_val);
    }

    // Set variables for drawing object.
    set_variables(label_str, data_val, data_str);

    // Do command: for drawing and stackchan
    label_draw_cmd(label_str, data_str);
    label_stackchan_cmd(label_str, data_str);
  }

  void cmd_led(const char *cmd_str) {
    char led_value = cmd_str[1] & 0xff;

    // On and off LED.
    M5.Power.setLed(constrain(led_value, 0, 255));
  }

  void onWrite(NimBLECharacteristic *pCharacteristic, NimBLEConnInfo &connInfo) override {
    log_i("CMD write\n");
    ////// MUST implement!!
    //// CMD_CONFIG 0x00
    // MIC    0x01
    // TOUCH  0x02
    //// CMD_PIN  0x01
    // SET_OUTPUT 0x01
    // SET_PWM    0x02
    // SET_SERVO  0x03
    // SET_PULL   0x04
    // SET_EVENT  0x05

    std::string value = pCharacteristic->getValue();
    log_i("CMD len:%d\n", value.length());
    log_i("%s\n", value.c_str());
    const char *cmd_str = value.c_str();
    log_i("%s\n", cmd_str);
    char cmd = (cmd_str[0] >> 5);
    switch (cmd) {
      case 0x01:
        // CMD_PIN
        log_i("CMD pin\n");
        cmd_pin(cmd_str);
        break;
      case 0x02:
        //// CMD_DISPLAY
        log_i("CMD display\n");
        cmd_display(cmd_str);
        break;
      case 0x03:
        //// CMD_AUDIO
        log_i("CMD audio\n");
        cmd_audio(cmd_str);
        break;
      case 0x04:
        //// CMD_DATA (only v2)
        log_i("CMD data\n");
        cmd_data(cmd_str);
        break;
      case 0x05:
        //// CMD_LED
        log_i("CMD LED\n");
        cmd_led(cmd_str);
        break;
    }
  }
} cmdCallbacks;

// for state
class StateCallbacks : public NimBLECharacteristicCallbacks {
  void onRead(NimBLECharacteristic *pCharacteristic, NimBLEConnInfo &connInfo) override {
    float temp = 0;

    // GPIO digital input using analog input from PortA1, PortA2, PortB1, and PortB2.
    state[0] = 0;
    if (analogRead(pin[0]) >= 2048) {
      state[0] |= 1 << 0;  // PortA 1 as Pin[0]
    }
    if (analogRead(pin[1]) >= 2048) {
      state[0] |= 1 << 1;  // PortA 2 as Pin[1]
    }
    if (analogRead(pin[2]) >= 2048) {
      state[0] |= 1 << 2;  // PortB 1 as Pin[2]
    }
    if (analogRead(pin[3]) >= 2048) {
      state[0] |= 1 << 3;  // PortB 2 as Pin[3]
    }

    if (myBoard == m5gfx::board_M5StickC || myBoard == m5gfx::board_M5StickCPlus || myBoard == m5gfx::board_M5StickCPlus2) {
      state[6] = ((int)map(soundLevel, 0, 1024, 0, 255) & 0xff);
    } else {
      state[6] = (random(256) & 0xff);  // Random sensor value for soundlevel
    }

    M5.Imu.getTemp(&temp);                  // get temperature from IMU
    state[5] = ((int)(temp + 128) & 0xff);  // temperature(+128)
    log_i("STATE read %s", (char *)state);
    pCharacteristic->setValue(state, 7);
  }
} stateCallbacks;

// for accelerometer related values
#define ACC_MULT 512
#if !defined(RAD_TO_DEG)
#define RAD_TO_DEG 57.324
#endif
float ax, ay, az;
int16_t iax, iay, iaz;
float gx, gy, gz;
float pitch, roll, yaw;

void updateIMU() {
  M5.Imu.getAccel(&ax, &ay, &az);  // get accel
  M5.Imu.getGyro(&gx, &gy, &gz);   // get gyro
  iax = (int16_t)(ax * ACC_MULT);
  iay = (int16_t)(ay * ACC_MULT);
  iaz = (int16_t)(az * ACC_MULT);
}

int16_t tx, ty;
int16_t enc;

void updateTouchpanel() {
  if (myBoard == m5gfx::board_M5Dial) {
    auto t = M5Dial.Touch.getDetail();
    tx = t.x;
    ty = t.y;
  } else {
    auto t = M5.Touch.getDetail();
    tx = t.x;
    ty = t.y;
  }
}

void updateEncoder() {
  if (myBoard == m5gfx::board_M5Dial) {
    enc = M5Dial.Encoder.read();
  }
}

class MotionCallbacks : public NimBLECharacteristicCallbacks {
  void onRead(NimBLECharacteristic *pCharacteristic, NimBLEConnInfo &connInfo) override {
    updateIMU();
    updateTouchpanel();
    updateEncoder();

    motion[0] = ((int)(pitch * ACC_MULT) & 0xff);
    motion[1] = (((int)(pitch * ACC_MULT) >> 8) & 0xff);
    motion[2] = ((int)(roll * ACC_MULT) & 0xff);
    motion[3] = (((int)(roll * ACC_MULT) >> 8) & 0xff);
    motion[4] = (iax & 0xff);
    motion[5] = ((iax >> 8) & 0xff);
    motion[6] = (iay & 0xff);
    motion[7] = ((iay >> 8) & 0xff);
    motion[8] = (-iaz & 0xff);
    motion[9] = ((-iaz >> 8) & 0xff);
    // motion[10-11] = gx;
    // motion[12-13] = gy;
    // motion[14-15] = gz;
    // motion[16-17] = ???;
    motion[18] = (tx & 0xff);
    motion[19] = ((tx >> 8) & 0xff);
    motion[20] = (ty & 0xff);
    motion[21] = ((ty >> 8) & 0xff);
    motion[22] = (enc & 0xff);
    motion[23] = ((enc >> 8) & 0xff);
    pCharacteristic->setValue(motion, 26);

    // debug print
    char msg[256] = { 0 };
    for (int i = 0; i < sizeof(motion); i++) {
      sprintf(&msg[i * 3], "%02x,", motion[i], sizeof(motion) * 3 - 3 * i);
    }
    log_i("MOTION read: %s\n", msg);
  }
} motionCallbacks;

// for button
class ActionCallbacks : public NimBLECharacteristicCallbacks {
  void onRead(NimBLECharacteristic *pCharacteristic, NimBLEConnInfo &connInfo) override {
    log_i("BTN read\n");
    pCharacteristic->setValue("Read me!!");  // dummy data
  }
} actionCallbacks;

// for Analog pin
class AnalogPinCallback : public NimBLECharacteristicCallbacks {
  void onRead(NimBLECharacteristic *pChar, NimBLEConnInfo &connInfo) override {
    int r = 0;
    int c;
    // Search pin number
    //   Characteristic for analog input start from 5 until 8.
    for (c = 5; c <= 8; c++) {
      if (pChar->toString().compare(pCharacteristic[c]->toString()) == 0) {
        break;
      }
    }
    int pin_num = c - 5;  // Fit for pin number.
    r = map(analogRead(pin[pin_num]), 0, 4095, 0, 1023);
    log_i("Analog Pin[%d] Read:%d\n", pin_num, r);

    analog[0] = (r & 0xff);
    analog[1] = ((r >> 8) & 0xff);

    pChar->setValue(analog, 2);
  }
} analogPinCallback;

void setup_M5Stack() {
  // Init M5Stack.
  auto cfg = M5.config();
  M5.begin(cfg);
  // for support BinsPack-for-StackChan-Core2 https://github.com/NoRi-230401/BinsPack-for-StackChan-Core2

  /*
  if (m5gfx::board_M5StackCore2) {
    checkSDUpdater(SD, MENU_BIN, 2000);
  }
  */

  M5.Display.init();

  // Init speaker.
  auto spk_cfg = M5.Speaker.config();
  M5.Speaker.config(spk_cfg);
  M5.Speaker.begin();
  myBoard = M5.getBoard();

  // Setup M5Dial
  if (myBoard == m5gfx::board_M5Dial) {
    M5Dial.begin(cfg, true, false);
  }

#if !defined(CONFIG_IDF_TARGET_ESP32S3)
  // Init FastLED(NeoPixel).
  if (myBoard == m5gfx::board_M5Atom) {
    FastLED.addLeds<WS2811, LED_DATA_PIN, GRB>(leds, NUM_LEDS);
    FastLED.setBrightness(20);
  }
#endif

  // for Mic input
  if (myBoard == m5gfx::board_M5StickC || myBoard == m5gfx::board_M5StickCPlus || myBoard == m5gfx::board_M5StickCPlus2) {
    i2sInit();
    xTaskCreate(mic_record_task, "mic_record_task", 2048, NULL, 1, NULL);
  }
}

void setup_pins() {
  // M5Stack GPIO
  pin[0] = M5.getPin(m5::pin_name_t::port_a_pin1);
  pin[1] = M5.getPin(m5::pin_name_t::port_a_pin2);
  pin[2] = M5.getPin(m5::pin_name_t::port_b_pin1);
  pin[3] = M5.getPin(m5::pin_name_t::port_b_pin2);
  /* UART
  pin[4] = M5.getPin(m5::pin_name_t::port_c_pin1);
  pin[5] = M5.getPin(m5::pin_name_t::port_c_pin2);
  */
}

void setup_BLE() {
  // Create MAC address base fixed ID
  uint8_t mac0[6] = { 0 };
  for (int i = 0; i < sizeof(mac0); i++) {
    mac0[i] = random(256);
  }
  String ID;
  for (int i = 0; i < 6; i++) {
    char ID_char = (((mac0[i] - 0x61) & 0b0011110) >> 1) + 0x61;
    ID += ID_char;
  }
  log_i("ID char:%s\n", ID.c_str());
  char adv_str[32] = { 0 };
  String("BBC micro:bit [" + ID + "]").toCharArray(adv_str, sizeof(adv_str));

  // Start up screen
  fillScreen(TFT_BLUE);
  Draw.setTextSize(2);
  Draw.setCursor(0, 0);
  Draw.print("Welcome to\nM5bit Less!!\nPlease connect to\n");
  Draw.println(adv_str);


  log_i("BLE start.\n");
  log_i("%s\n", adv_str);
  NimBLEDevice::init(adv_str);
  NimBLEServer *pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(&myServerCallbacks);
  NimBLEService *pService = pServer->createService(BLEUUID(MBIT_MORE_SERVICE));

  // CMD
  pCharacteristic[0] = pService->createCharacteristic(
    MBIT_MORE_CH_COMMAND,
    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  pCharacteristic[0]->setCallbacks(&cmdCallbacks);

  // STATE
  pCharacteristic[1] = pService->createCharacteristic(
    MBIT_MORE_CH_STATE,
    NIMBLE_PROPERTY::READ);
  pCharacteristic[1]->setCallbacks(&stateCallbacks);

  // MOTION
  pCharacteristic[2] = pService->createCharacteristic(
    MBIT_MORE_CH_MOTION,
    NIMBLE_PROPERTY::READ);
  pCharacteristic[2]->setCallbacks(&motionCallbacks);

  pCharacteristic[3] = pService->createCharacteristic(
    MBIT_MORE_CH_PIN_EVENT,
    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  pCharacteristic[3]->setCallbacks(&dummyCallbacks);

  // ACTION
  pCharacteristic[4] = pService->createCharacteristic(
    MBIT_MORE_CH_ACTION_EVENT,
    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  pCharacteristic[4]->setCallbacks(&actionCallbacks);

  // PINS
  pCharacteristic[5] = pService->createCharacteristic(
    MBIT_MORE_CH_ANALOG_IN_P0,
    NIMBLE_PROPERTY::READ);
  pCharacteristic[5]->setCallbacks(&analogPinCallback);

  pCharacteristic[6] = pService->createCharacteristic(
    MBIT_MORE_CH_ANALOG_IN_P1,
    NIMBLE_PROPERTY::READ);
  pCharacteristic[6]->setCallbacks(&analogPinCallback);

  pCharacteristic[7] = pService->createCharacteristic(
    MBIT_MORE_CH_ANALOG_IN_P2,
    NIMBLE_PROPERTY::READ);
  pCharacteristic[7]->setCallbacks(&analogPinCallback);

  pCharacteristic[8] = pService->createCharacteristic(
    MBIT_MORE_CH_ANALOG_IN_P3,
    NIMBLE_PROPERTY::READ);
  pCharacteristic[8]->setCallbacks(&analogPinCallback);

  // MESSAGE (only for v2)
  pCharacteristic[9] = pService->createCharacteristic(
    MBIT_MORE_CH_MESSAGE,
    NIMBLE_PROPERTY::READ);
  pCharacteristic[9]->setCallbacks(&dummyCallbacks);

  pService->start();
  NimBLEAdvertising *pAdvertising = pServer->getAdvertising();
  pAdvertising->setName(adv_str);
  pAdvertising->start();
}

void setup() {
  Serial.begin(115200);
  Serial2.begin(115200);
  Wire.begin();

  setup_M5Stack();
  screen_w = M5.Lcd.width();
  screen_h = M5.Lcd.height();
  setup_pins();
  setup_BLE();
}

void sendBtn(uint8_t btnID, uint8_t btn, uint8_t btn_status, uint8_t prev) {
  memset((char *)(action), 0, 20);  // clear action buffer

  action[0] = 0x01;   // for Button event
  action[19] = 0x12;  // ACTION_EVENT

  action[1] = btnID;  // btnID 0x01:BtnA, 0x02:BtnB, 0x03:BtnC

  // Set TimeStamp (Little Endian)
  uint32_t time = (uint32_t)millis();
  action[4] = (time & 0xff);
  action[5] = (time >> 8) & 0xff;
  action[6] = (time >> 16) & 0xff;
  action[7] = (time >> 24) & 0xff;

  if (btn) {
    // Button CLICK
    log_i(" button clicked!\n");
    action[3] = 0x03;
    pCharacteristic[4]->setValue(action, 20);
    pCharacteristic[4]->notify();
  }
  if (btn_status == 0 && prev == 1) {
    // Button Up
    log_i(" button up!\n");
    action[3] = 0x02;
    pCharacteristic[4]->setValue(action, 20);
    pCharacteristic[4]->notify();
  } else if (btn_status == 1 && prev == 0) {
    // Button Down
    log_i(" button down!\n");
    action[3] = 0x01;
    pCharacteristic[4]->setValue(action, 20);
    pCharacteristic[4]->notify();
  }
}

// Previous button state
uint8_t prevA = 0, prevB = 0, prevC = 0;
uint32_t old_label_time = 0;

void loop() {
  // Board status update.
  M5.update();
  if (myBoard == m5gfx::board_M5Dial) {
    M5Dial.update();
  }

  if (deviceConnected) {
    // Send notify data for button A, B and C.
    uint8_t btnA = 0, btnB = 0, btnC = 0,
            btn_statusA = 0, btn_statusB = 0, btn_statusC = 0;

    // Get all button status
    btnA = M5.BtnA.wasPressed();
    btn_statusA = M5.BtnA.isPressed();
    btnB = M5.BtnB.wasPressed();
    btn_statusB = M5.BtnB.isPressed();
    btnC = M5.BtnC.wasPressed();
    btn_statusC = M5.BtnC.isPressed();

#define BUTTON_DELAY 50

    //// Button A
    action[1] = 0x01;
    sendBtn(0x01, btnA, btn_statusA, prevA);
    prevA = btn_statusA;
    delay(BUTTON_DELAY);

    //// Button B
    action[1] = 0x02;
    sendBtn(0x02, btnB, btn_statusB, prevB);
    prevB = btn_statusB;
    delay(BUTTON_DELAY);

    //// Button C
    action[1] = 0x03;
    sendBtn(0x03, btnC, btn_statusC, prevC);
    prevC = btn_statusC;
    delay(BUTTON_DELAY);

    updateGesture();

    uint32_t label_time = (uint32_t)millis();
    if (label_time - old_label_time > 50) {
      // Send dummy data label='a' data=random('a'-'z') every 50ms
      memset((char *)(action), 0, 20);  // clear action buffer
      action[19] = DATA_TEXT;
      action[0] = 'a';  // Label 'a'
      action[1] = 0;
      action[8] = 'a' + random(26);  // 'a-z'
      action[9] = 0;
      pCharacteristic[4]->setValue(action, 20);
      pCharacteristic[4]->notify();

      if (myBoard == m5gfx::board_M5Stack) {
        // keyboard input for M5Stack Faces
        if (digitalRead(5) == LOW) {
          Wire.requestFrom(0x08, 1);  // 0x08 means FACES_KEYBOARD_I2C_ADDR.
          while (Wire.available()) {
            char c = Wire.read();             // receive a byte as character
            Serial.printf("Key:%c\n", c);     // print the character
            memset((char *)(action), 0, 20);  // clear action buffer
            action[19] = DATA_TEXT;
            action[0] = 'K';  // Label 'Key'
            action[1] = 'e';
            action[2] = 'y';
            action[3] = 0x00;
            action[8] = c;  // Key character
            action[9] = 0;
            delay(50);  // Wait 50ms
            pCharacteristic[4]->setValue(action, 20);
            pCharacteristic[4]->notify();
          }
        }
      }

      old_label_time = label_time;
    }
  }

  //// Get data from Serial2
  //     Data format is "label:value".
  //     If value is full float format, pass data as float.
  //     Otherwise pass data as string.
  int av = Serial2.available();
  // When serial2 data is available...
  while (av > 0) {
    String label = Serial2.readStringUntil(':');   // The left side of ':'.
    String value = Serial2.readStringUntil('\n');  // The right side of ':'.
    av = Serial2.available();                      // Are there more data?

    // Convert value to float
    bool is_float;  // Is data float?
    char *stopstr;
    float f_value = strtod(value.c_str(), &stopstr);
    if (stopstr[0] == NULL) {
      is_float = true;
    } else {
      is_float = false;
    }

    memset((char *)(action), 0, 20);  // clear action buffer

    const char *label_str = label.c_str();
    strncpy((char *)action, label_str, 7);

    if (is_float) {
      // Data is passed as float.
      action[19] = DATA_NUMBER;  // Data send as number.

      cd.f = f_value;
      action[8] = cd.b[0];
      action[9] = cd.b[1];
      action[10] = cd.b[2];
      action[11] = cd.b[3];
    } else {
      // Data is passed as String(TEXT).
      action[19] = DATA_TEXT;  // Data send as text.

      // The data buffer can handle 18 - 8 characters.
      const char *buf = value.c_str();
      for (int i = 0; i < av && i + 8 < 19; i++) {
        action[i + 8] = buf[i];
      }
    }

    // Send data to Scratch with label left side of ':'.
    pCharacteristic[4]->setValue(action, 20);
    pCharacteristic[4]->notify();
  }
};
