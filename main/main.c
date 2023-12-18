#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "esp_http_client.h"
#include "sdkconfig.h"
#include "audio_element.h"
#include "audio_pipeline.h"
#include "audio_event_iface.h"
#include "audio_common.h"
#include "board.h"
#include "http_stream.h"
#include "i2s_stream.h"
#include "wav_encoder.h"
#include "esp_peripherals.h"
#include "periph_button.h"
#include "periph_wifi.h"
#include "filter_resample.h"
#include "input_key_service.h"
#include "audio_idf_version.h"

#include "esp_netif.h"

#define DEMO_EXIT_BIT (BIT0)

#define CONFIG_SERVER_URI "http://gappu-nextjs.vercel.app/api/google/translate"

/*--------------------------------- Static Variables ---------------------------------*/
static const char* TAG = "ADF_TEST";

static const char* ssid = "iPhone";
static const char* pass = "asdfghjkl";

static esp_periph_set_handle_t periph_set;
static esp_periph_handle_t wifi_handle;
static esp_periph_handle_t button_handle;

static audio_pipeline_handle_t pipeline;
static audio_element_handle_t http_stream_writer, i2s_stream_reader;

static EventGroupHandle_t EXIT_FLAG;

/*--------------------------------- Static Functions ---------------------------------*/
// HTTP Stream Event Handler
static esp_err_t _http_stream_event_handle(http_stream_event_msg_t *msg)
{
    esp_http_client_handle_t http = (esp_http_client_handle_t)msg->http_client;
    char len_buf[16];
    static int total_write = 0;

    if (msg->event_id == HTTP_STREAM_PRE_REQUEST) {
        // set header
        ESP_LOGI(TAG, "[ + ] HTTP client HTTP_STREAM_PRE_REQUEST, lenght=%d", msg->buffer_len);
        esp_http_client_set_header(http, "x-audio-sample-rates", "16000");
        esp_http_client_set_header(http, "x-audio-bits", "16");
        esp_http_client_set_header(http, "x-audio-channel", "2");
        total_write = 0;
        return ESP_OK;
    }

    if (msg->event_id == HTTP_STREAM_ON_REQUEST) {
        // write data
        int wlen = sprintf(len_buf, "%x\r\n", msg->buffer_len);
        if (esp_http_client_write(http, len_buf, wlen) <= 0) {
            return ESP_FAIL;
        }
        if (esp_http_client_write(http, msg->buffer, msg->buffer_len) <= 0) {
            return ESP_FAIL;
        }
        if (esp_http_client_write(http, "\r\n", 2) <= 0) {
            return ESP_FAIL;
        }
        total_write += msg->buffer_len;
        printf("\033[A\33[2K\rTotal bytes written: %d\n", total_write);
        return msg->buffer_len;
    }

    if (msg->event_id == HTTP_STREAM_POST_REQUEST) {
        ESP_LOGI(TAG, "[ + ] HTTP client HTTP_STREAM_POST_REQUEST, write end chunked marker");
        if (esp_http_client_write(http, "0\r\n\r\n", 5) <= 0) {
            return ESP_FAIL;
        }
        return ESP_OK;
    }

    if (msg->event_id == HTTP_STREAM_FINISH_REQUEST) {
        ESP_LOGI(TAG, "[ + ] HTTP client HTTP_STREAM_FINISH_REQUEST");
        char *buf = calloc(1, 64);
        assert(buf);
        int read_len = esp_http_client_read(http, buf, 64);
        if (read_len <= 0) {
            free(buf);
            return ESP_FAIL;
        }
        buf[read_len] = 0;
        ESP_LOGI(TAG, "Got HTTP Response = %s", (char *)buf);
        free(buf);
        return ESP_OK;
    }
    return ESP_OK;
}

static esp_err_t input_key_service_cb(periph_service_handle_t handle, periph_service_event_t *evt, void *ctx)
{
    audio_element_handle_t http_stream_writer = (audio_element_handle_t)ctx;
    if (evt->type == INPUT_KEY_SERVICE_ACTION_CLICK) {
        switch ((int)evt->data) {
            case INPUT_KEY_USER_ID_MODE:
                ESP_LOGW(TAG, "[ * ] [Set] input key event, exit the demo ...");
                xEventGroupSetBits(EXIT_FLAG, DEMO_EXIT_BIT);
                break;
            case INPUT_KEY_USER_ID_REC:
                ESP_LOGE(TAG, "[ * ] [Rec] input key event, resuming pipeline ...");
                /*
                 * There is no effect when follow APIs output warning message on the first time record
                 */
                audio_pipeline_stop(pipeline);
                audio_pipeline_wait_for_stop(pipeline);
                audio_pipeline_reset_ringbuffer(pipeline);
                audio_pipeline_reset_elements(pipeline);
                audio_pipeline_terminate(pipeline);

                audio_element_set_uri(http_stream_writer, CONFIG_SERVER_URI);
                audio_pipeline_run(pipeline);
                break;
        }
    } else if (evt->type == INPUT_KEY_SERVICE_ACTION_CLICK_RELEASE || evt->type == INPUT_KEY_SERVICE_ACTION_PRESS_RELEASE) {
        switch ((int)evt->data) {
            case INPUT_KEY_USER_ID_REC:
                ESP_LOGE(TAG, "[ * ] [Rec] key released, stop pipeline ...");
                /*
                 * Set the i2s_stream_reader ringbuffer is done to flush the buffering voice data.
                 */
                audio_element_set_ringbuf_done(i2s_stream_reader);
                break;
        }
    }

    return ESP_OK;
}

// Initialize WiFi and GPIO button
static void board_peripheral_init(){

    // Initialize ADF Peripheral Handler
    esp_periph_config_t periph_cfg = DEFAULT_ESP_PERIPH_SET_CONFIG();
    periph_set = esp_periph_set_init(&periph_cfg);

    // Initialize WiFi
    periph_wifi_cfg_t wifi_cfg = {
        .ssid = ssid,
        .password = pass,
    };
    wifi_handle = periph_wifi_init(&wifi_cfg);
    esp_periph_start(periph_set, wifi_handle);
    periph_wifi_wait_for_connected(wifi_handle, portMAX_DELAY);

    return;
}

// Initialize Codec
static void board_codec_init(){
    audio_board_handle_t board_handle = audio_board_init();
    audio_hal_ctrl_codec(board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_BOTH, AUDIO_HAL_CTRL_START);
}

// Initialize ADF Pipeline
static void adf_pipeline_init(){
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    pipeline = audio_pipeline_init(&pipeline_cfg);
    mem_assert(pipeline);
}

// Initialize I2S In Stream
static void i2s_in_stream_init(){
    ESP_LOGI(TAG, "[3.2] Create i2s stream to read audio data from codec chip");
    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
    i2s_cfg.type = AUDIO_STREAM_READER;
    i2s_cfg.out_rb_size = 16 * 1024; // Increase buffer to avoid missing data in bad network conditions
    i2s_cfg.i2s_port = CODEC_ADC_I2S_PORT;
    i2s_stream_reader = i2s_stream_init(&i2s_cfg);
}

// Initialize HTTP Out Stream
static void http_out_stream_init(){
    http_stream_cfg_t http_cfg = HTTP_STREAM_CFG_DEFAULT();
    http_cfg.type = AUDIO_STREAM_WRITER;
    http_cfg.event_handle = _http_stream_event_handle;
    http_stream_writer = http_stream_init(&http_cfg);
}

// Initialize Event Listener
static void evt_listener_init(){
    audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    evt_listener = audio_event_iface_init(&evt_cfg);

    audio_pipeline_set_listener(pipeline, evt_listener); //Add listener to audio pipeline
    
    // Set up the listener to listen  events from registered peripherals
    audio_event_iface_set_listener(esp_periph_set_get_event_iface(periph_set), evt_listener); 
}

static void start_adf_pipeline(){
    nvs_flash_init();
    ESP_ERROR_CHECK(esp_netif_init());

    board_peripheral_init();
    board_codec_init();
    adf_pipeline_init();

    i2s_in_stream_init(); // Initialize I2S input stream and store stream handle to i2s_stream_reader handle
    http_out_stream_init(); // Initialize I2S output stream and store stream handle to http_stream_writer

    // Define audio pipeline
    audio_pipeline_register(pipeline, i2s_stream_reader, "i2s");
    audio_pipeline_register(pipeline, http_stream_writer, "http");
    const char *link_tag[2] = {"i2s", "http"};
    audio_pipeline_link(pipeline, &link_tag[0], 2);

    i2s_stream_set_clk(i2s_stream_reader, 16000, 16, 2);

    ESP_LOGI(TAG, "[ 4 ] Press [Rec] button to record, Press [Mode] to exit");
    xEventGroupWaitBits(EXIT_FLAG, DEMO_EXIT_BIT, true, false, portMAX_DELAY);    
}

static void end_adf_pipeline(){

}

/*--------------------------------- Main ---------------------------------*/

void app_main(void)
{

}

