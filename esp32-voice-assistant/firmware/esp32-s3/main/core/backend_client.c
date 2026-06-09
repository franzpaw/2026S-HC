#include "backend_client.h"

#include <stdbool.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "mbedtls/base64.h"
#include "sdkconfig.h"

static const char *TAG = "backend_client";
static const char *CHAT_BOUNDARY = "----voice-client-boundary";

static esp_err_t make_url(char *buffer, size_t buffer_size, const char *path) {
    int written = snprintf(buffer, buffer_size, "%s%s", CONFIG_VOICE_BACKEND_BASE_URL, path);
    if (written < 0 || (size_t)written >= buffer_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static esp_err_t make_auth_header(char *buffer, size_t buffer_size) {
    int written = snprintf(buffer, buffer_size, "Bearer %s", CONFIG_VOICE_API_TOKEN);
    if (written < 0 || (size_t)written >= buffer_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static esp_err_t ensure_backend_config(void) {
    if (strlen(CONFIG_VOICE_BACKEND_BASE_URL) == 0) {
        ESP_LOGE(TAG, "CONFIG_VOICE_BACKEND_BASE_URL is empty; run idf.py menuconfig");
        return ESP_ERR_INVALID_STATE;
    }
    if (strlen(CONFIG_VOICE_API_TOKEN) == 0) {
        ESP_LOGE(TAG, "CONFIG_VOICE_API_TOKEN is empty; run idf.py menuconfig");
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

static esp_err_t write_exact(esp_http_client_handle_t client, const char *data, int len) {
    int written = esp_http_client_write(client, data, len);
    if (written < 0) {
        return ESP_FAIL;
    }
    if (written != len) {
        ESP_LOGE(TAG, "short http write %d/%d", written, len);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static const char *json_string(cJSON *root, const char *name) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!cJSON_IsString(item)) {
        return NULL;
    }
    return item->valuestring;
}

static esp_err_t decode_and_report_audio(const char *audio_base64,
                                         const char *audio_format,
                                         backend_audio_callback_t audio_callback,
                                         void *user_ctx) {
    size_t audio_base64_len = strlen(audio_base64);
    size_t audio_len = 0;
    int ret = mbedtls_base64_decode(NULL, 0, &audio_len, (const unsigned char *)audio_base64, audio_base64_len);
    if (ret != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) {
        ESP_LOGE(TAG, "audio_base64 size check failed ret=%d", ret);
        return ESP_FAIL;
    }

    unsigned char *audio = heap_caps_malloc(audio_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(audio != NULL, ESP_ERR_NO_MEM, TAG, "alloc audio bytes");

    ret = mbedtls_base64_decode(audio, audio_len, &audio_len, (const unsigned char *)audio_base64, audio_base64_len);
    if (ret != 0) {
        free(audio);
        ESP_LOGE(TAG, "audio_base64 decode failed ret=%d", ret);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "final audio_base64 chars=%u", (unsigned)audio_base64_len);
    ESP_LOGI(TAG, "final audio bytes=%u", (unsigned)audio_len);
    if (audio_callback != NULL) {
        audio_callback(audio, audio_len, audio_format != NULL ? audio_format : "mp3", user_ctx);
    }
    free(audio);
    return ESP_OK;
}

static esp_err_t log_sse_line(const char *line, backend_audio_callback_t audio_callback, void *user_ctx) {
    cJSON *root = cJSON_Parse(line);
    if (root == NULL) {
        ESP_LOGW(TAG, "skip invalid sse json");
        return ESP_OK;
    }

    const char *type = json_string(root, "type");
    const char *phase = json_string(root, "phase");
    const char *text = json_string(root, "text");
    const char *audio_base64 = json_string(root, "audio_base64");
    const char *audio_format = json_string(root, "audio_format");

    esp_err_t ret = ESP_OK;
    if (type != NULL && strcmp(type, "done") == 0) {
        ESP_LOGI(TAG, "chat stream done");
    } else if (type != NULL && strcmp(type, "error") == 0) {
        ESP_LOGE(TAG, "sse error %s", text != NULL ? text : "");
        ret = ESP_FAIL;
    } else if (phase != NULL && strcmp(phase, "input") == 0) {
        ESP_LOGI(TAG, "sse user text=\"%s\"", text != NULL ? text : "");
    } else if (phase != NULL && strcmp(phase, "commentary") == 0) {
        ESP_LOGI(TAG, "sse commentary text=\"%s\"", text != NULL ? text : "");
        if (audio_base64 != NULL) {
            ret = decode_and_report_audio(audio_base64, audio_format, audio_callback, user_ctx);
        }
    } else if (phase != NULL && strcmp(phase, "final") == 0) {
        ESP_LOGI(TAG, "sse final text=\"%s\"", text != NULL ? text : "");
        if (audio_base64 != NULL) {
            ret = decode_and_report_audio(audio_base64, audio_format, audio_callback, user_ctx);
        } else {
            ESP_LOGE(TAG, "final response missing audio_base64");
            ret = ESP_ERR_NOT_FOUND;
        }
    }

    cJSON_Delete(root);
    return ret;
}

static esp_err_t process_sse_chunk(const char *chunk,
                                   int chunk_len,
                                   char *line,
                                   size_t line_size,
                                   size_t *line_len,
                                   backend_audio_callback_t audio_callback,
                                   void *user_ctx) {
    for (int i = 0; i < chunk_len; i++) {
        char c = chunk[i];
        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            line[*line_len] = '\0';
            if (strncmp(line, "data: ", 6) == 0) {
                esp_err_t ret = log_sse_line(line + 6, audio_callback, user_ctx);
                *line_len = 0;
                if (ret != ESP_OK) {
                    return ret;
                }
                continue;
            }
            *line_len = 0;
            continue;
        }
        if (*line_len + 1 >= line_size) {
            ESP_LOGE(TAG, "sse line too large max=%u", (unsigned)line_size);
            return ESP_ERR_INVALID_SIZE;
        }
        line[*line_len] = c;
        (*line_len)++;
    }
    return ESP_OK;
}

esp_err_t backend_client_health_check(void) {
    ESP_RETURN_ON_ERROR(ensure_backend_config(), TAG, "backend config");

    char url[256];
    ESP_RETURN_ON_ERROR(make_url(url, sizeof(url), "/health"), TAG, "build health url");

    char auth_header[160];
    ESP_RETURN_ON_ERROR(make_auth_header(auth_header, sizeof(auth_header)), TAG, "build auth header");

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = CONFIG_VOICE_BACKEND_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    ESP_LOGI(TAG, "GET %s", url);
    esp_http_client_handle_t client = esp_http_client_init(&config);
    ESP_RETURN_ON_FALSE(client != NULL, ESP_FAIL, TAG, "init http client");

    esp_err_t ret = esp_http_client_set_header(client, "Authorization", auth_header);
    if (ret == ESP_OK) {
        ret = esp_http_client_perform(client);
    }

    int status = esp_http_client_get_status_code(client);
    int64_t content_length = esp_http_client_get_content_length(client);
    esp_http_client_cleanup(client);

    ESP_RETURN_ON_ERROR(ret, TAG, "perform health request");

    if (status != 200) {
        ESP_LOGE(TAG, "health failed status=%d content_length=%lld", status, content_length);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "health ok status=%d content_length=%lld", status, content_length);
    return ESP_OK;
}

esp_err_t backend_client_chat_audio(const uint8_t *wav_data,
                                    size_t wav_size,
                                    backend_audio_callback_t audio_callback,
                                    void *user_ctx) {
    ESP_RETURN_ON_FALSE(wav_data != NULL, ESP_ERR_INVALID_ARG, TAG, "wav_data is null");
    ESP_RETURN_ON_FALSE(wav_size > 0, ESP_ERR_INVALID_ARG, TAG, "wav_size is empty");
    ESP_RETURN_ON_ERROR(ensure_backend_config(), TAG, "backend config");

    char url[256];
    ESP_RETURN_ON_ERROR(make_url(url, sizeof(url), "/chat/stream"), TAG, "build chat url");

    char auth_header[160];
    ESP_RETURN_ON_ERROR(make_auth_header(auth_header, sizeof(auth_header)), TAG, "build auth header");

    char content_type[96];
    int content_type_len = snprintf(content_type, sizeof(content_type), "multipart/form-data; boundary=%s", CHAT_BOUNDARY);
    ESP_RETURN_ON_FALSE(content_type_len > 0 && content_type_len < (int)sizeof(content_type), ESP_ERR_INVALID_SIZE, TAG, "content type too long");

    char tts_part[160];
    int tts_part_len = snprintf(
        tts_part,
        sizeof(tts_part),
        "--%s\r\nContent-Disposition: form-data; name=\"tts_enabled\"\r\n\r\ntrue\r\n",
        CHAT_BOUNDARY);
    ESP_RETURN_ON_FALSE(tts_part_len > 0 && tts_part_len < (int)sizeof(tts_part), ESP_ERR_INVALID_SIZE, TAG, "tts part too long");

    char tts_format_part[160];
    int tts_format_part_len = snprintf(
        tts_format_part,
        sizeof(tts_format_part),
        "--%s\r\nContent-Disposition: form-data; name=\"tts_format\"\r\n\r\nwav\r\n",
        CHAT_BOUNDARY);
    ESP_RETURN_ON_FALSE(tts_format_part_len > 0 && tts_format_part_len < (int)sizeof(tts_format_part), ESP_ERR_INVALID_SIZE, TAG, "tts format part too long");

    char audio_header[192];
    int audio_header_len = snprintf(
        audio_header,
        sizeof(audio_header),
        "--%s\r\nContent-Disposition: form-data; name=\"audio\"; filename=\"recording.wav\"\r\nContent-Type: audio/wav\r\n\r\n",
        CHAT_BOUNDARY);
    ESP_RETURN_ON_FALSE(audio_header_len > 0 && audio_header_len < (int)sizeof(audio_header), ESP_ERR_INVALID_SIZE, TAG, "audio header too long");

    char closing[64];
    int closing_len = snprintf(closing, sizeof(closing), "\r\n--%s--\r\n", CHAT_BOUNDARY);
    ESP_RETURN_ON_FALSE(closing_len > 0 && closing_len < (int)sizeof(closing), ESP_ERR_INVALID_SIZE, TAG, "closing too long");

    int64_t content_length = (int64_t)tts_part_len + tts_format_part_len + audio_header_len + wav_size + closing_len;
    ESP_RETURN_ON_FALSE(content_length <= INT32_MAX, ESP_ERR_INVALID_SIZE, TAG, "multipart body too large");

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = CONFIG_VOICE_CHAT_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    ESP_LOGI(TAG, "POST %s wav_bytes=%u", url, (unsigned)wav_size);
    esp_http_client_handle_t client = esp_http_client_init(&config);
    ESP_RETURN_ON_FALSE(client != NULL, ESP_FAIL, TAG, "init http client");

    esp_err_t ret = esp_http_client_set_header(client, "Authorization", auth_header);
    if (ret == ESP_OK) {
        ret = esp_http_client_set_header(client, "Content-Type", content_type);
    }
    if (ret == ESP_OK) {
        ret = esp_http_client_open(client, (int)content_length);
    }
    if (ret == ESP_OK) {
        ret = write_exact(client, tts_part, tts_part_len);
    }
    if (ret == ESP_OK) {
        ret = write_exact(client, tts_format_part, tts_format_part_len);
    }
    if (ret == ESP_OK) {
        ret = write_exact(client, audio_header, audio_header_len);
    }
    if (ret == ESP_OK) {
        int written = esp_http_client_write(client, (const char *)wav_data, wav_size);
        if (written < 0 || (size_t)written != wav_size) {
            ESP_LOGE(TAG, "short wav write %d/%u", written, (unsigned)wav_size);
            ret = ESP_FAIL;
        }
    }
    if (ret == ESP_OK) {
        ret = write_exact(client, closing, closing_len);
    }

    if (ret != ESP_OK) {
        esp_http_client_cleanup(client);
        ESP_RETURN_ON_ERROR(ret, TAG, "write chat request");
    }

    int header_status = esp_http_client_fetch_headers(client);
    if (header_status < 0) {
        esp_http_client_cleanup(client);
        ESP_LOGE(TAG, "fetch chat headers failed");
        return ESP_FAIL;
    }

    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        ESP_LOGE(TAG, "chat failed status=%d", status);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    char chunk[256];
    char *line = heap_caps_malloc(CONFIG_VOICE_SSE_LINE_MAX_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (line == NULL) {
        esp_http_client_cleanup(client);
        ESP_LOGE(TAG, "alloc sse line buffer");
        return ESP_ERR_NO_MEM;
    }

    size_t line_len = 0;
    while (true) {
        int read_len = esp_http_client_read(client, chunk, sizeof(chunk));
        if (read_len < 0) {
            ESP_LOGE(TAG, "read chat stream failed");
            free(line);
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }
        if (read_len == 0) {
            break;
        }
        ret = process_sse_chunk(chunk,
                                read_len,
                                line,
                                CONFIG_VOICE_SSE_LINE_MAX_BYTES,
                                &line_len,
                                audio_callback,
                                user_ctx);
        if (ret != ESP_OK) {
            free(line);
            esp_http_client_cleanup(client);
            return ret;
        }
    }

    free(line);
    esp_http_client_cleanup(client);
    ESP_LOGI(TAG, "chat upload complete");
    return ESP_OK;
}
