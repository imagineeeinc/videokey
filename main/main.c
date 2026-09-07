#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <dirent.h>
#include "sdkconfig.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_sleep.h"
#include "esp_flash.h"
#include "esp_system.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_jpeg_common.h"
#include "esp_jpeg_dec.h"

#include <sys/unistd.h>
#include <sys/stat.h>
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/gpio.h"
#include "driver/ledc.h"

#include "st7789.h"
#include "fontx.h"
#include "battery.h"

#define MOUNT_POINT "/sdcard"

#define BOOT_BUTTON_GPIO       GPIO_NUM_0

#define SD_MISO 16
#define SD_MOSI 15
#define SD_CLK  14
#define SD_CS   21

#define SD_SPI_HOST SPI3_HOST

#define CHUNK_SIZE    (4 * 1024)   // 4KB
#define MAX_JPEG_SIZE (64 * 1024)  // 64KB

#define MAX_VOLTS 4.22
#define MIN_VOLTS 3.2

static QueueHandle_t gpio_evt_queue = NULL;

TFT_t disp;
FontxFile fx[2];

int brigthtness = 100;
const uint32_t max_duty = (1 << 13) - 1;
uint32_t duty;

// Mode
// 0 = Video mode (mjpeg)
// 1 = menu
// 2 = Static jpeg
int mode = 0;
int cur_mode = 0;
bool clicked = false;

TaskHandle_t video_applet_handle = NULL;

uint8_t ascii[128];

int play_file = 0;
bool skip_requested = false;

int menu_selected = 0;
const char * const menu_items[] = {
  "Sleep",
  "+ Brigthtness",
  "- Brigthtness",
  "Exit"
};
#define MENU_SIZE (sizeof(menu_items) / sizeof(menu_items[0]))

static const char *TAG = "MAIN";

static void go_sleep(void) {
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_OFF);
  esp_sleep_pd_config(ESP_PD_DOMAIN_RC_FAST, ESP_PD_OPTION_OFF);

  esp_sleep_config_gpio_isolate();

  fflush(stdout);

  esp_deep_sleep_start();
}

static void IRAM_ATTR gpio_isr_handler(void* args) {
  uint32_t gpio_num = (uint32_t) args;
  xQueueSendFromISR(gpio_evt_queue, &gpio_num, NULL);
}

static void boot_pressed(void* args) {
  uint32_t io_num;
  TickType_t press_tick = 0;

  while (1) {
    if (xQueueReceive(gpio_evt_queue, &io_num, portMAX_DELAY)) {
      int level = gpio_get_level(io_num);
      if (level == 0) {
        press_tick = xTaskGetTickCount();
      } else {
        TickType_t hold_ticks = xTaskGetTickCount() - press_tick;
        uint32_t hold_time_ms = pdTICKS_TO_MS(hold_ticks);
        if (hold_time_ms > 500) {
          if (mode == 0) {
            mode = 1;
          } else if (mode == 1) {
            clicked = true;
          }
        } else if (hold_time_ms > 50) {
          if (mode == 0) {
            skip_requested = true;
            play_file++;
          } else {
            menu_selected++;
          }
        }
        press_tick = 0;
      }
    }
  }
}

void set_brightness(void) {
  duty = (brigthtness * max_duty) / 100;
  ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
  ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

esp_err_t list_files(char **dir_path , char ***file_list, size_t *file_count) {
  DIR *dir = opendir(*dir_path);

  if (!dir) {
    ESP_LOGE(TAG, "Failed to open directory: %s", dir_path);
    *file_list = NULL;
    *file_count = 0;
    return ESP_FAIL;
  }

  size_t capacity = 16;
  size_t count = 0;
  char **list = malloc(capacity * sizeof(char *));

  if (!list) {
    closedir(dir);
    return ESP_ERR_NO_MEM;
  }

  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (entry->d_name[0] == '.') {
      continue;
    }

    if (count >= capacity) {
      capacity *= 2;
      char **temp = realloc(list, capacity * sizeof(char *));
      if (!temp) {
        for (size_t i = 0; i < count; i++) {
          free(list[i]);
        }
        free(list);
        closedir(dir);
        return ESP_ERR_NO_MEM;
      }
      list = temp;
    }

    list[count] = strdup(entry->d_name);
    if (list[count] == NULL) {
      break;
    }
    count++;
  }
  closedir(dir);

  *file_list = list;
  *file_count = count;

  return ESP_OK;
}

static void draw_fast(TFT_t *dev, uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t *pixels) {
  if (w == 0 || h == 0) return;

  uint16_t x1 = x + dev->_offsetx;
  uint16_t x2 = x + w - 1 + dev->_offsetx;
  uint16_t y1 = y + dev->_offsety;
  uint16_t y2 = y + h - 1 + dev->_offsety;

  gpio_set_level(dev->_dc, 0);
  uint8_t cmd_caset = 0x2A;
  spi_transaction_t t_cmd = { .length = 8, .tx_buffer = &cmd_caset };
  spi_device_polling_transmit(dev->_SPIHandle, &t_cmd);

  gpio_set_level(dev->_dc, 1);
  uint8_t data_x[] = { x1 >> 8, x1 & 0xFF, x2 >> 8, x2 & 0xFF };
  spi_transaction_t t_data_x = { .length = 32, .tx_buffer = data_x };
  spi_device_polling_transmit(dev->_SPIHandle, &t_data_x);

  gpio_set_level(dev->_dc, 0);
  uint8_t cmd_raset = 0x2B;
  t_cmd.tx_buffer = &cmd_raset;
  spi_device_polling_transmit(dev->_SPIHandle, &t_cmd);

  gpio_set_level(dev->_dc, 1);
  uint8_t data_y[] = { y1 >> 8, y1 & 0xFF, y2 >> 8, y2 & 0xFF };
  spi_transaction_t t_data_y = { .length = 32, .tx_buffer = data_y };
  spi_device_polling_transmit(dev->_SPIHandle, &t_data_y);

  gpio_set_level(dev->_dc, 0);
  uint8_t cmd_ramwr = 0x2C;
  t_cmd.tx_buffer = &cmd_ramwr;
  spi_device_polling_transmit(dev->_SPIHandle, &t_cmd);

  gpio_set_level(dev->_dc, 1);
  size_t total_bytes = w * h * sizeof(uint16_t);
  const uint8_t *ptr = (const uint8_t *)pixels;
  size_t chunk_size = 4096; // 4 kb

  while (total_bytes > 0) {
    size_t send_bytes = (total_bytes > chunk_size) ? chunk_size : total_bytes;
    spi_transaction_t t_pixels = {
      .length = send_bytes * 8,
      .tx_buffer = ptr,
    };
    spi_device_polling_transmit(dev->_SPIHandle, &t_pixels);

    ptr += send_bytes;
    total_bytes -= send_bytes;
  }
}

void menu_applet(void) {
  float volts;
  float percent;
  char res[128];
  int cur_menu = -1;
  while (mode == 1) {
    if (cur_menu != menu_selected || clicked == true) {
      if (clicked == true) {
        if (cur_menu == 0) {
          go_sleep();
        } else if (cur_menu == 1) {
          if (brigthtness >= 25) {
            brigthtness+=25;
          } else {
            brigthtness+=5;
          }
          if (brigthtness > 100) {
            brigthtness = 100;
          }
          set_brightness();
        } else if (cur_menu == 2) {
          if (brigthtness > 25) {
            brigthtness-=25;
          } else {
            brigthtness-=5;
          }
          if (brigthtness < 5) {
            brigthtness = 5;
          }
          set_brightness();
        } else if (cur_menu == 3) {
          mode = 0;
          break;
        }
        clicked = false;
      }
      if (menu_selected > MENU_SIZE - 1) {
        menu_selected = 0;
      }
      lcdDrawFillRect(&disp, 10, 10, CONFIG_WIDTH - 10, CONFIG_HEIGHT - 10, BLACK);

      volts = BAT_Get_Volts();
      percent = ((volts - MIN_VOLTS) / (MAX_VOLTS - MIN_VOLTS)) * 100.0f;
      snprintf(res, sizeof(res), "Battery %i%% (%.1fV)", (int)percent, volts);
      memcpy(ascii, res, strlen(res) + 1);
      lcdDrawString(&disp, fx, 15, 25, ascii, WHITE);
      snprintf(res, sizeof(res), "Brigthtness %i%%", brigthtness);
      memcpy(ascii, res, strlen(res) + 1);
      lcdDrawString(&disp, fx, 15, 40, ascii, WHITE);
      for (size_t i = 0; i < MENU_SIZE; i++) {
        if (i == menu_selected) {
          snprintf(res, sizeof(res), "> %s", menu_items[i]);
        } else {
          snprintf(res, sizeof(res), "  %s", menu_items[i]);
        }
        memcpy(ascii, res, strlen(res) + 1);;
        lcdDrawString(&disp, fx, 15, 55+(16*(i+1)), ascii, WHITE);
      }
      cur_menu = menu_selected;
    }
    vTaskDelay(1);
  }
}

void play_mjpeg(const char *file) {
  esp_err_t ret;

  FILE *f = fopen(file, "rb");
  if (!f) {
    ESP_LOGE(TAG, "Failed to open video file: \"%s\".", file);
    return;
  }

  uint8_t *chunk_buf = heap_caps_aligned_alloc(16, CHUNK_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!chunk_buf) chunk_buf = heap_caps_aligned_alloc(16, CHUNK_SIZE, MALLOC_CAP_8BIT);
  uint8_t *jpeg_buf = heap_caps_aligned_alloc(16, MAX_JPEG_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!jpeg_buf) jpeg_buf = heap_caps_aligned_alloc(16, MAX_JPEG_SIZE, MALLOC_CAP_8BIT);
  size_t out_size = CONFIG_WIDTH * CONFIG_HEIGHT * sizeof(uint16_t);
  uint16_t *out_buf = heap_caps_aligned_alloc(16, out_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!out_buf) out_buf = heap_caps_aligned_alloc(16, out_size, MALLOC_CAP_8BIT);
  if (!chunk_buf || !jpeg_buf || !out_buf) {
    ESP_LOGE(TAG, "Failed to allocate playback buffers.");
    if (chunk_buf) free(chunk_buf);
    if (jpeg_buf) free(jpeg_buf);
    if (out_buf) free(out_buf);
    fclose(f);
    return;
  }

  jpeg_dec_config_t config = {
    .output_type = JPEG_PIXEL_FORMAT_RGB565_BE,
    .rotate = JPEG_ROTATE_0D,
  };

  jpeg_dec_handle_t jpeg_dec = NULL;
  ret = jpeg_dec_open(&config, &jpeg_dec);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to initialize JPEG decoder.");
    free(chunk_buf);
    free(jpeg_buf);
    free(out_buf);
    fclose(f);
    return;
  }

  size_t bytes_read = 0;
  size_t frame_pos = 0;
  bool receiving_frame = false;
  uint8_t byte_prev = 0;
  uint32_t frame_count = 0;

  while ((bytes_read = fread(chunk_buf, 1, CHUNK_SIZE, f)) > 0) {
    if (skip_requested == true) {
      skip_requested = false;
      break;
    }
    if (mode != 0) {
      menu_applet();
    }
    for (size_t i = 0; i < bytes_read; i++) {
      uint8_t b = chunk_buf[i];

      if (!receiving_frame) {
        // start of frame
        if ((i > 0 && chunk_buf[i - 1] == 0xFF && b == 0xD8) || (i == 0 && byte_prev == 0xFF && b == 0xD8)) {
          jpeg_buf[0] = 0xFF;
          jpeg_buf[1] = 0xD8;
          frame_pos = 2;
          receiving_frame = true;
        }
      } else {
        if (frame_pos < MAX_JPEG_SIZE) {
          jpeg_buf[frame_pos++] = b;
        }
        // End of frame
        if ((i > 0 && chunk_buf[i - 1] == 0xFF && b == 0xD9) || (i == 0 && byte_prev == 0xFF && b == 0xD9)) {
          jpeg_dec_io_t io = {
            .inbuf = jpeg_buf,
            .inbuf_len = frame_pos,
            .outbuf = (uint8_t *)out_buf,
          };
          jpeg_dec_header_info_t header_info;
          ret = jpeg_dec_parse_header(jpeg_dec, &io, &header_info);
          if (ret == ESP_OK) {
            ret = jpeg_dec_process(jpeg_dec, &io);
            if (ret == ESP_OK) {
              uint16_t draw_width = (header_info.width > CONFIG_WIDTH) ? CONFIG_WIDTH : header_info.width;
              uint16_t draw_height = (header_info.height > CONFIG_HEIGHT) ? CONFIG_HEIGHT : header_info.height;
              draw_fast(&disp, 0, 0, draw_width, draw_height, out_buf);
              frame_count++;
            } else {
              ESP_LOGE(TAG, "JPEG process failed: %s (0x%x)", esp_err_to_name(ret), ret);
            }
          } else {
            ESP_LOGE(TAG, "JPEG header parse failed: %s (0x%x)", esp_err_to_name(ret), ret);
          }

          receiving_frame = false;
          frame_pos = 0;
        }
      }
      byte_prev = b;
    }
  }

  jpeg_dec_close(jpeg_dec);
  free(chunk_buf);
  free(jpeg_buf);
  free(out_buf);
  fclose(f);
}

void video_applet(void *args) {
  cur_mode = 0;
  esp_err_t ret;

  ESP_LOGI(TAG, "Starting Video Mode.");

  char *file_location = "/sdcard/mjpeg";
  char **mjpeg_files = NULL;
  size_t count = 0;

  ret = list_files(&file_location, &mjpeg_files, &count);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to list file in \"/mjpeg\", see errors above");
    exit(ESP_FAIL);
  }

  char file[128];
  while (1) {
    if (play_file > count - 1) {
      play_file = 0;
    }
    snprintf(file, sizeof(file), "/sdcard/mjpeg/%s", mjpeg_files[play_file]);
    play_mjpeg(file);
  }
}

esp_err_t sd_setup(void) {
  esp_err_t ret;

  esp_vfs_fat_sdmmc_mount_config_t mount_config = {
    .format_if_mount_failed = false,
    .max_files = 8
  };
  sdmmc_card_t *card;
  const char mount_point[] = MOUNT_POINT;
  ESP_LOGI(TAG, "Initializing SD card");

  sdmmc_host_t sd_host = SDSPI_HOST_DEFAULT();
  sd_host.unaligned_multi_block_rw_max_chunk_size = 8;
  sd_host.max_freq_khz = 25000000UL;
  sd_host.slot = SD_SPI_HOST;

  spi_bus_config_t bus_cfg = {
    .mosi_io_num = SD_MOSI,
    .miso_io_num = SD_MISO,
    .sclk_io_num = SD_CLK,
    .quadwp_io_num = -1,
    .quadhd_io_num = -1,
    .max_transfer_sz = 4000,
  };

  ret = spi_bus_initialize(sd_host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to initialize bus.");
    return ret;
  }

  sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
  slot_config.gpio_cs = SD_CS;
  slot_config.host_id = sd_host.slot;

  ESP_LOGI(TAG, "Mounting filesystem");
  ret = esp_vfs_fat_sdspi_mount(mount_point, &sd_host, &slot_config, &mount_config, &card);

  if (ret != ESP_OK) {
    if (ret == ESP_FAIL) {
      ESP_LOGE(TAG, "Failed to mount filesystem.");
    } else {
      ESP_LOGE(TAG, "Failed to initialize the card: %s.", esp_err_to_name(ret));
    }
    return ret;
  }
  ESP_LOGI(TAG, "Filesystem mounted");

  return ESP_OK;
}
esp_err_t display_setup(void) {
  spi_clock_speed(40000000);
  spi_master_init(&disp, CONFIG_MOSI_GPIO, CONFIG_SCLK_GPIO, CONFIG_CS_GPIO, CONFIG_DC_GPIO, CONFIG_RESET_GPIO, -1);
  ledc_timer_config_t bl_timer = {
    .speed_mode       = LEDC_LOW_SPEED_MODE,
    .timer_num        = LEDC_TIMER_0,
    .duty_resolution  = LEDC_TIMER_13_BIT,
    .freq_hz          = 5000,
    .clk_cfg          = LEDC_AUTO_CLK
  };
  ledc_timer_config(&bl_timer);
  ledc_channel_config_t bl_channel = {
    .speed_mode     = LEDC_LOW_SPEED_MODE,
    .channel        = LEDC_CHANNEL_0,
    .timer_sel      = LEDC_TIMER_0,
    .intr_type      = LEDC_INTR_DISABLE,
    .gpio_num       = CONFIG_BL_GPIO,
    .duty           = 0,
    .hpoint         = 0
  };
  ledc_channel_config(&bl_channel);
  set_brightness();

  lcdInit(&disp, CONFIG_WIDTH, CONFIG_HEIGHT, 34, 0);

  InitFontx(fx,"/sdcard/font/inconsolata.fnt","");

  return ESP_OK;
}

void app_main(void) {
  esp_err_t ret;

  ret = display_setup();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to initialize display, see errors above");
  }

  ret = sd_setup();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to mount SD Card, see errors above");
  }

  BAT_Init();

  gpio_config_t io_conf = {
    .pin_bit_mask = (1ULL << BOOT_BUTTON_GPIO),
    .mode = GPIO_MODE_INPUT,
    .pull_up_en = GPIO_PULLUP_ENABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type = GPIO_INTR_ANYEDGE
  };
  gpio_config(&io_conf);

  gpio_evt_queue = xQueueCreate(10, sizeof(uint32_t));
  xTaskCreate(boot_pressed, "boot_btn_applet", 2048, NULL, 5, NULL);

  gpio_install_isr_service(ESP_INTR_FLAG_LEVEL3);
  gpio_isr_handler_add(BOOT_BUTTON_GPIO, gpio_isr_handler, (void*) BOOT_BUTTON_GPIO);
  xTaskCreatePinnedToCore(video_applet, "video_applet", 4096, NULL, 10, &video_applet_handle, 1);
}
