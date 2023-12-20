#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "mbedtls/base64.h"

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
#include "json_utils.h"

#define GOOGLE_SR_URI "http://gappu-nextjs.vercel.app/api/google/speech2text"

#define GOOGLE_SR_LANG "en-US"            // https://cloud.google.com/speech-to-text/docs/languages
#define GOOGLE_SR_ENCODING "LINEAR16"
#define SAMPLE_RATE_HZ (16000)

#define GOOGLE_SR_BEGIN            "{\"language\": \"%s\", \"encoding\": \"%s\", \"sampleRateHertz\": %d, \"speech\": \""
#define GOOGLE_SR_END              "\"}"

#define GOOGLE_SR_TASK_STACK (8*1024)

#define AUDIO_BUFFER_SIZE (6*1024)

/*--------------------------------- Static Variables ---------------------------------*/
static const char* TAG = "ADF_TEST";

static const int DEMO_EXIT_BIT = (BIT0);
static EventGroupHandle_t EXIT_FLAG;

static const char* ssid = "iPhone";
static const char* pass = "asdfghjkl";

static esp_periph_set_handle_t periph_set_handle;
static audio_pipeline_handle_t pipeline;
static audio_element_handle_t http_stream_writer;
static audio_element_handle_t i2s_stream_reader;

typedef struct {
    bool                    is_begin;
    int                     sr_total_write;
    
    int                     buffer_size;
    char*                   buffer;
    char*                   b64_buffer;
    int                     remain_len;

    char*                   lang_code;
    char*                   encoding;
    int                     sample_rates;
    
    char*                   response_text;
} google_sr_t;

static google_sr_t sr_handle;

/*--------------------------------- Static Functions ---------------------------------*/
// Write Chunk to HTTP API
static int _http_write_chunk(esp_http_client_handle_t http, const char *buffer, int len)
{
    char header_chunk_buffer[16];
    int header_chunk_len = sprintf(header_chunk_buffer, "%x\r\n", len);
    if (esp_http_client_write(http, header_chunk_buffer, header_chunk_len) <= 0) {
        return ESP_FAIL;
    }
    int write_len = esp_http_client_write(http, buffer, len);
    if (write_len <= 0) {
        ESP_LOGE(TAG, "Error write chunked content");
        return ESP_FAIL;
    }
    if (esp_http_client_write(http, "\r\n", 2) <= 0) {
        return ESP_FAIL;
    }
    return write_len;
}

// HTTP Stream Event Handler
static esp_err_t _http_stream_event_handle(http_stream_event_msg_t* msg)
{
    esp_http_client_handle_t http = (esp_http_client_handle_t)msg->http_client;
    
    int write_len;
    size_t need_write = 0;
    if (msg->event_id == HTTP_STREAM_PRE_REQUEST) {
        // set header
        sr_handle.sr_total_write = 0;
        sr_handle.is_begin = true;
        sr_handle.remain_len = 0;
        esp_http_client_set_method(http, HTTP_METHOD_POST);
        esp_http_client_set_post_field(http, NULL, -1); // Chunk content
        esp_http_client_set_header(http, "Content-Type", "application/json");
        return ESP_OK;
    }

    if (msg->event_id == HTTP_STREAM_ON_REQUEST) {
        /* Write first chunk */
        if (sr_handle.is_begin) {
            sr_handle.is_begin = false;
            int sr_begin_len = snprintf(sr_handle.buffer, sr_handle.buffer_size, GOOGLE_SR_BEGIN, sr_handle.lang_code, sr_handle.lang_code, sr_handle.sample_rates);

            return _http_write_chunk(http, sr_handle.buffer, sr_begin_len);
        }

        if (msg->buffer_len > sr_handle.buffer_size * 3 / 2) {
            ESP_LOGE(TAG, "Please use SR Buffer size greeter than %d", msg->buffer_len * 3 / 2);
            return ESP_FAIL;
        }

        /* Write b64 audio data */
        memcpy(sr_handle.buffer + sr_handle.remain_len, msg->buffer, msg->buffer_len);
        sr_handle.remain_len += msg->buffer_len;
        int keep_next_time = sr_handle.remain_len % 3;

        sr_handle.remain_len -= keep_next_time;
        if (mbedtls_base64_encode((unsigned char *)sr_handle.b64_buffer, sr_handle.buffer_size,  &need_write, (unsigned char *)sr_handle.buffer, sr_handle.remain_len) != 0) {
            ESP_LOGE(TAG, "Error encode b64");
            return ESP_FAIL;
        }
        if (keep_next_time > 0) {
            memcpy(sr_handle.buffer, sr_handle.buffer + sr_handle.remain_len, keep_next_time);
        }
        sr_handle.remain_len = keep_next_time;
        ESP_LOGD(TAG, "\033[A\33[2K\rTotal bytes written: %d", sr_handle.sr_total_write);

        write_len = _http_write_chunk(http, (const char *)sr_handle.b64_buffer, need_write);
        if (write_len <= 0) {
            return write_len;
        }
        sr_handle.sr_total_write += write_len;
        return write_len;
    }

     /* Write End chunk */
    if (msg->event_id == HTTP_STREAM_POST_REQUEST) {
        need_write = 0;
        if (sr_handle.remain_len) {
            if (mbedtls_base64_encode((unsigned char *)sr_handle.b64_buffer, sr_handle.buffer_size,  &need_write, (unsigned char *)sr_handle.buffer, sr_handle.remain_len) != 0) {
                ESP_LOGE(TAG, "Error encode b64");
                return ESP_FAIL;
            }
            write_len = _http_write_chunk(http, (const char *)sr_handle.b64_buffer, need_write);
            if (write_len <= 0) {
                return write_len;
            }
        }
        write_len = _http_write_chunk(http, GOOGLE_SR_END, strlen(GOOGLE_SR_END));

        if (write_len <= 0) {
            return ESP_FAIL;
        }
        /* Finish chunked */
        if (esp_http_client_write(http, "0\r\n\r\n", 5) <= 0) {
            return ESP_FAIL;
        }
        return write_len;
    }

    if (msg->event_id == HTTP_STREAM_FINISH_REQUEST) {

        int read_len = esp_http_client_read(http, (char*)sr_handle.buffer, sr_handle.buffer_size);
        if (read_len <= 0) {
            return ESP_FAIL;
        }
        if (read_len > sr_handle.buffer_size - 1) {
            read_len = sr_handle.buffer_size - 1;
        }
        sr_handle.buffer[read_len] = 0;
        ESP_LOGI(TAG, "Got HTTP Response = %s", (char*)sr_handle.buffer);
        if (sr_handle.response_text) {
            free(sr_handle.response_text);
        }
        sr_handle.response_text = json_get_token_value(sr_handle.buffer, "transcript");
        return ESP_OK;
    }

    return ESP_OK;
}

static esp_err_t input_key_service_cb(periph_service_handle_t handle, periph_service_event_t* evt, void* param)
{
    if (evt->type == INPUT_KEY_SERVICE_ACTION_PRESS) {
        switch ((int)evt->data) {
            case INPUT_KEY_USER_ID_REC:
                ESP_LOGI(TAG, "Resuming Pipeline");

                /*
                 * There is no effect when follow APIs output warning message on the first time record
                 */
                audio_pipeline_reset_items_state(pipeline);
                audio_pipeline_reset_ringbuffer(pipeline);

                audio_element_set_uri(http_stream_writer, GOOGLE_SR_URI);
                audio_pipeline_run(pipeline);
                break;
        }
    } else if (evt->type == INPUT_KEY_SERVICE_ACTION_CLICK_RELEASE || evt->type == INPUT_KEY_SERVICE_ACTION_PRESS_RELEASE) {
        switch ((int)evt->data) {
            case INPUT_KEY_USER_ID_REC:
                ESP_LOGI(TAG, "Pause Pipeline");
                audio_pipeline_stop(pipeline);
                audio_pipeline_wait_for_stop(pipeline);
                ESP_LOGI(TAG, "resp text = %s", sr_handle.response_text);
                break;

            case INPUT_KEY_USER_ID_MODE:
                ESP_LOGI(TAG, "Exit");
                xEventGroupSetBits(EXIT_FLAG, DEMO_EXIT_BIT);
                break;
        }
    }

    return ESP_OK;
}

// Initialize WiFi
static void wifi_peripheral_init(){

    // Initialize ADF Peripheral Handler
    esp_periph_config_t periph_cfg = DEFAULT_ESP_PERIPH_SET_CONFIG();
    periph_set_handle = esp_periph_set_init(&periph_cfg);

    periph_wifi_cfg_t wifi_cfg = {
        .ssid = ssid,
        .password = pass,
    };

    esp_periph_handle_t wifi_handle = periph_wifi_init(&wifi_cfg);
    esp_periph_start(periph_set_handle, wifi_handle);
    periph_wifi_wait_for_connected(wifi_handle, portMAX_DELAY);

    return;
}

static void button_peripheral_init(){
    audio_board_key_init(periph_set_handle);
    input_key_service_info_t input_key_info[] = INPUT_KEY_DEFAULT_INFO();
    input_key_service_cfg_t input_cfg = INPUT_KEY_SERVICE_DEFAULT_CONFIG();
    input_cfg.handle = periph_set_handle;
    periph_service_handle_t input_ser = input_key_service_create(&input_cfg);
    input_key_service_add_key(input_ser, input_key_info, INPUT_KEY_NUM);
    periph_service_set_callback(input_ser, input_key_service_cb, NULL);

    return;
}

// Initialize Codec
static void board_codec_init(){
    audio_board_handle_t board_handle = audio_board_init();
    audio_hal_ctrl_codec(board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_BOTH, AUDIO_HAL_CTRL_START);

    return;
}

// Initialize ADF Pipeline
static void adf_pipeline_init(){
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    pipeline = audio_pipeline_init(&pipeline_cfg);
    mem_assert(pipeline);

    return;
}

// Initialize I2S In Stream
static void i2s_in_stream_init(){
    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
    i2s_cfg.type = AUDIO_STREAM_READER;
    i2s_cfg.out_rb_size = 16 * 1024; // Increase buffer to avoid missing data in bad network conditions
    i2s_cfg.i2s_port = CODEC_ADC_I2S_PORT;
    i2s_stream_reader = i2s_stream_init(&i2s_cfg);

    return;
}

// Initialize HTTP Out Stream
static void http_out_stream_init(){
    http_stream_cfg_t http_cfg = HTTP_STREAM_CFG_DEFAULT();
    http_cfg.type = AUDIO_STREAM_WRITER;
    http_cfg.event_handle = _http_stream_event_handle;
    http_cfg.task_stack = GOOGLE_SR_TASK_STACK;
    http_stream_writer = http_stream_init(&http_cfg);

    return;
}

static void start_adf_pipeline(){
    wifi_peripheral_init();
    board_codec_init();
    adf_pipeline_init();

    i2s_in_stream_init(); // Initialize I2S input stream and store stream handle to i2s_stream_reader handle
    http_out_stream_init(); // Initialize I2S output stream and store stream handle to http_stream_writer
    
    // Define audio pipeline
    audio_pipeline_register(pipeline, i2s_stream_reader, "i2s");
    audio_pipeline_register(pipeline, http_stream_writer, "http");
    const char *link_tag[2] = {"i2s", "http"};
    audio_pipeline_link(pipeline, &link_tag[0], 2);
    
    i2s_stream_set_clk(i2s_stream_reader, SAMPLE_RATE_HZ, 16, 1);
    
    button_peripheral_init();

    ESP_LOGI(TAG, "Press [Rec] button to record, Press [Mode] to exit");
    xEventGroupWaitBits(EXIT_FLAG, DEMO_EXIT_BIT, true, false, portMAX_DELAY);    
}

static void end_adf_pipeline(){
    audio_pipeline_stop(pipeline);
    audio_pipeline_wait_for_stop(pipeline);
    audio_pipeline_terminate(pipeline);
    audio_pipeline_deinit(pipeline);

    /* Stop all periph before removing the listener */
    esp_periph_set_stop_all(periph_set_handle);
    esp_periph_set_destroy(periph_set_handle);

    free(sr_handle.buffer);
    free(sr_handle.b64_buffer);
    free(sr_handle.lang_code);
    free(sr_handle.encoding);
    free(&sr_handle);
}

static google_sr_t* google_sr_init()
{
    google_sr_t* sr = calloc(1, sizeof(google_sr_t));
    AUDIO_MEM_CHECK(TAG, sr, return NULL);

    sr->buffer_size = AUDIO_BUFFER_SIZE;
    if (sr->buffer_size <= 0) {
        ESP_LOGE(TAG, "Invalid audio buffer size");
    }

    sr->buffer = malloc(sr->buffer_size);
    AUDIO_MEM_CHECK(TAG, sr->buffer, goto exit_sr_init);
    sr->b64_buffer = malloc(sr->buffer_size);
    AUDIO_MEM_CHECK(TAG, sr->b64_buffer, goto exit_sr_init);
    sr->lang_code = strdup(GOOGLE_SR_LANG);
    AUDIO_MEM_CHECK(TAG, sr->lang_code, goto exit_sr_init);
    sr->encoding = strdup(GOOGLE_SR_ENCODING);
    AUDIO_MEM_CHECK(TAG, sr->encoding, goto exit_sr_init);
    sr->sample_rates = SAMPLE_RATE_HZ;

    return sr;

exit_sr_init:
    ESP_LOGE(TAG, "Memory Error");
    return NULL;
}

/*--------------------------------- Main ---------------------------------*/

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());

    EXIT_FLAG = xEventGroupCreate();
    
    sr_handle = *(google_sr_init());
    start_adf_pipeline();
    end_adf_pipeline();
}

