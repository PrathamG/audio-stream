#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "esp_http_client.h"
#include "sdkconfig.h"
#include "audio_event_iface.h"
#include "audio_common.h"
#include "esp_peripherals.h"
#include "periph_button.h"
#include "periph_adc_button.h"
#include "periph_wifi.h"
#include "board.h"
#include "periph_led.h"
#include "google_sr.h"
#include "google_tts.h"
#include "audio_idf_version.h"
#include "esp_netif.h"

static const char *TAG = "CLOUD_API_TEST";

#define GOOGLE_SR_LANG "en-US"                  //https://cloud.google.com/speech-to-text/docs/languages
#define GOOGLE_TTS_LANG "en-US"                 //https://cloud.google.com/text-to-speech/docs/voices
#define RECORD_PLAYBACK_SAMPLE_RATE (16000) 

static esp_periph_set_handle_t periph_set;
static google_sr_handle_t sr;
static google_tts_handle_t tts;
static audio_event_iface_handle_t evt_listener;

void google_sr_begin(google_sr_handle_t sr)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "Start speaking now");
    ESP_LOGI(TAG, "========================================");
}

static void audio_board_codec_init_start(){
    // Initialze audio board and onboard codec chip
    audio_board_handle_t board_handle = audio_board_init();
    audio_hal_ctrl_codec(board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_BOTH, AUDIO_HAL_CTRL_START);
    ESP_LOGI(TAG, "Audio board and codec started");
}

static void audio_board_peripherals_setup(){
    // Initialize board peripherals and keys
    esp_periph_config_t periph_cfg = DEFAULT_ESP_PERIPH_SET_CONFIG();
    periph_set = esp_periph_set_init(&periph_cfg);

    // Initialize keys on audio board
    audio_board_key_init(periph_set);
    ESP_LOGI(TAG, "Audio board peripherals and keys initialized");
}

static void wifi_init_start(){
    // Initialize and start WiFi connection
    periph_wifi_cfg_t wifi_cfg = {
        .ssid = CONFIG_WIFI_SSID,
        .password = CONFIG_WIFI_PASSWORD,
    };
    esp_periph_handle_t wifi_handle = periph_wifi_init(&wifi_cfg);
    esp_periph_start(periph_set, wifi_handle);
    periph_wifi_wait_for_connected(wifi_handle, portMAX_DELAY);
    ESP_LOGI(TAG, "WiFi Connected");
}

static void google_sr_init_start(){
    // Initialize google sr handler
    google_sr_config_t sr_config = {
        .api_key = CONFIG_GOOGLE_API_KEY,
        .lang_code = GOOGLE_SR_LANG,
        .record_sample_rates = RECORD_PLAYBACK_SAMPLE_RATE,
        .encoding = ENCODING_LINEAR16,
        .on_begin = google_sr_begin,
    };
    sr = google_sr_init(&sr_config);
    ESP_LOGI(TAG, "%s", CONFIG_GOOGLE_API_KEY);
    ESP_LOGI(TAG, "I2S->HTTP SR Audio pipeline initialized");
}

static void google_tts_init_start(){
    // Initialize google tts handler
    google_tts_config_t tts_config = {
        .api_key = CONFIG_GOOGLE_API_KEY,
        .playback_sample_rate = RECORD_PLAYBACK_SAMPLE_RATE,
    };
    tts = google_tts_init(&tts_config);
}

static void audio_event_listener_setup_start(){
    // Initialize audio event listener
    audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    evt_listener = audio_event_iface_init(&evt_cfg);

    // Connect event listener to the SR adf pipeline, so that it can monitor SR pipeline events
    google_sr_set_listener(sr, evt_listener);
    // Connect event listener to the TTS adf pipeline, so that it can monitor TTS pipeline events
    google_tts_set_listener(tts, evt_listener);
    // Connect event listener to board peripherals, so that it can listen to peripherals events
    audio_event_iface_set_listener(esp_periph_set_get_event_iface(periph_set), evt_listener);

    ESP_LOGI(TAG, "Audio event listener initialized and setup");
}

void event_process_Task(void *pv)
{       
    audio_event_iface_msg_t msg;

    while (1) {
        if(audio_event_iface_listen(evt_listener, &msg, portMAX_DELAY) != ESP_OK) {
            ESP_LOGE(TAG, "[ * ] Event process failed: src_type:%d, source:%p cmd:%d, data:%p, data_len:%d",
                     msg.source_type, msg.source, msg.cmd, msg.data, msg.data_len);
            continue;
        }

        if(google_tts_check_event_finish(tts, &msg)) {
            ESP_LOGI(TAG, "[ * ] TTS Finish");
            continue;
        }
        
        //ESP_LOGI(TAG, "[ * ] Event received: src_type:%d, source:%p cmd:%d, data:%p, data_len:%d", msg.source_type, msg.source, msg.cmd, msg.data, msg.data_len);
        /*
        if ((int)msg.data != get_input_rec_id() && (int)msg.data != get_input_mode_id()) {
            ESP_LOGI(TAG, "[ * ] Pressed button %d was not record or mode, skip event", (int)msg.data);
            continue;
        }*/

        if ((msg.source_type == PERIPH_ID_TOUCH || msg.source_type == PERIPH_ID_BUTTON || msg.source_type == PERIPH_ID_ADC_BTN)) {
            if((int)msg.data == get_input_rec_id()) {
                if(msg.cmd == PERIPH_BUTTON_PRESSED) {
                    google_tts_stop(tts);
                    ESP_LOGI(TAG, "[ * ] Resuming SR pipeline");
                    google_sr_start(sr);
                } 
                else if(msg.cmd == PERIPH_BUTTON_RELEASE || msg.cmd == PERIPH_BUTTON_LONG_RELEASE){
                    ESP_LOGI(TAG, "[ * ] Stop SR pipeline");

                    char* response_text = google_sr_stop(sr);
                    if (response_text == NULL) {
                        continue;
                    }
                    ESP_LOGI(TAG, "response text = %s", response_text);
                    vTaskDelay(500 / portTICK_PERIOD_MS);
                    ESP_LOGI(TAG, "TTS Start");
                    google_tts_start(tts, response_text, GOOGLE_TTS_LANG);   
                } 
                else if ((int)msg.data == get_input_mode_id()) {
                    ESP_LOGI(TAG, "Mode button was pressed, exit now");
                    break;
                }
            }
        }
    }

    ESP_LOGI(TAG, "[ 6 ] Stop audio_pipeline");
    google_sr_destroy(sr);
    google_tts_destroy(tts);
    // Stop all periph before removing the listener 
    esp_periph_set_stop_all(periph_set);
    audio_event_iface_remove_listener(esp_periph_set_get_event_iface(periph_set), evt_listener);

    // Make sure audio_pipeline_remove_listener & audio_event_iface_remove_listener are called before destroying event_iface
    audio_event_iface_destroy(evt_listener);
    esp_periph_set_destroy(periph_set);
    vTaskDelete(NULL);
}

void app_main(void)
{
    // Initialize NVS
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    // Initialize network interface
    ESP_ERROR_CHECK(esp_netif_init());

    audio_board_codec_init_start();                     //Initialize audio board and codec
    audio_board_peripherals_setup(periph_set);          //Initialize audio board peripherals
    wifi_init_start();                                  //Start wifi
    google_sr_init_start();                             //Initialize (i2s_read)->(http_write) audio pipeline for sr
    google_tts_init_start();                            //Initialize (http_write)->(mp3_decoder)->(i2s_write) audio pipeline for tts
    audio_event_listener_setup_start();                 //Init audio event listener and connect it to pipelines + peripherals

    xTaskCreate(event_process_Task, "event_process", 4 * 4096, NULL, 5, 0);  
}
