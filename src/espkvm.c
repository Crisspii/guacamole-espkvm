/*
 * guacamole-espkvm
 *
 * Native Apache Guacamole protocol plugin for ESPKVM.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <guacamole/client.h>
#include <guacamole/protocol.h>
#include <guacamole/socket.h>
#include <guacamole/user.h>
#include <guacamole/stream.h>

#include <curl/curl.h>
#include <curl/websockets.h>
#include <json-c/json.h>

#include <pthread.h>
#include <stdint.h>
#include <unistd.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char* ESPKVM_ARGS[] = {
    "hostname",
    "port",
    "username",
    "password",
    "ignore-cert",
    NULL
};

typedef struct espkvm_response {
    char* data;
    size_t length;
} espkvm_response;

typedef struct espkvm_client_data {
    CURL* curl;
    char* base_url;
    int ignore_cert;
} espkvm_client_data;


static size_t espkvm_write_callback(
        char* ptr,
        size_t size,
        size_t nmemb,
        void* userdata) {

    espkvm_response* response = userdata;
    size_t bytes = size * nmemb;

    char* new_data = realloc(
        response->data,
        response->length + bytes + 1
    );

    if (new_data == NULL)
        return 0;

    response->data = new_data;

    memcpy(
        response->data + response->length,
        ptr,
        bytes
    );

    response->length += bytes;
    response->data[response->length] = '\0';

    return bytes;
}

static int espkvm_is_true(const char* value) {

    if (value == NULL)
        return 0;

    return strcasecmp(value, "true") == 0
        || strcmp(value, "1") == 0
        || strcasecmp(value, "yes") == 0
        || strcasecmp(value, "on") == 0;
}

static char* espkvm_build_base_url(
        const char* hostname,
        const char* port) {

    long parsed_port = 443;
    char* end = NULL;
    int ipv6;
    size_t needed;
    char* url;

    if (hostname == NULL || hostname[0] == '\0')
        return NULL;

    if (port != NULL && port[0] != '\0') {

        parsed_port = strtol(port, &end, 10);

        if (end == port
                || *end != '\0'
                || parsed_port < 1
                || parsed_port > 65535)
            return NULL;
    }

    ipv6 = strchr(hostname, ':') != NULL
        && hostname[0] != '[';

    needed = strlen(hostname) + 32;

    url = malloc(needed);
    if (url == NULL)
        return NULL;

    if (ipv6)
        snprintf(
            url,
            needed,
            "https://[%s]:%ld",
            hostname,
            parsed_port
        );
    else
        snprintf(
            url,
            needed,
            "https://%s:%ld",
            hostname,
            parsed_port
        );

    return url;
}

static void espkvm_prepare_curl(
        CURL* curl,
        int ignore_cert) {

    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    curl_easy_setopt(
        curl,
        CURLOPT_CONNECTTIMEOUT,
        10L
    );

    curl_easy_setopt(
        curl,
        CURLOPT_TIMEOUT,
        20L
    );

    curl_easy_setopt(
        curl,
        CURLOPT_USERAGENT,
        "guacamole-espkvm/0.1.0"
    );

    /*
     * Ask libcurl to accept compressed responses and decompress them
     * automatically. ESPKVM serves the embedded web console gzip-compressed.
     */
    curl_easy_setopt(
        curl,
        CURLOPT_ACCEPT_ENCODING,
        ""
    );

    /*
     * Enable the in-memory cookie engine.
     *
     * ESPKVM stores authentication in the kvm_session cookie.
     */
    curl_easy_setopt(
        curl,
        CURLOPT_COOKIEFILE,
        ""
    );

    if (ignore_cert) {

        curl_easy_setopt(
            curl,
            CURLOPT_SSL_VERIFYPEER,
            0L
        );

        curl_easy_setopt(
            curl,
            CURLOPT_SSL_VERIFYHOST,
            0L
        );
    }
    else {

        curl_easy_setopt(
            curl,
            CURLOPT_SSL_VERIFYPEER,
            1L
        );

        curl_easy_setopt(
            curl,
            CURLOPT_SSL_VERIFYHOST,
            2L
        );
    }
}

static int espkvm_get_session(
        guac_client* client,
        espkvm_client_data* data,
        int* required,
        int* authenticated) {

    char url[2048];
    espkvm_response response = { NULL, 0 };

    CURLcode result;
    long http_code = 0;

    json_object* root = NULL;
    json_object* required_obj = NULL;
    json_object* authenticated_obj = NULL;

    snprintf(
        url,
        sizeof(url),
        "%s/api/v1/auth/session",
        data->base_url
    );

    curl_easy_setopt(
        data->curl,
        CURLOPT_URL,
        url
    );

    curl_easy_setopt(
        data->curl,
        CURLOPT_POST,
        0L
    );

    curl_easy_setopt(
        data->curl,
        CURLOPT_POSTFIELDS,
        NULL
    );

    curl_easy_setopt(
        data->curl,
        CURLOPT_HTTPHEADER,
        NULL
    );

    /*
     * CURLOPT_POSTFIELDS may switch libcurl into POST mode.
     * Force GET only after all POST-related options have been cleared.
     */
    curl_easy_setopt(
        data->curl,
        CURLOPT_HTTPGET,
        1L
    );

    curl_easy_setopt(
        data->curl,
        CURLOPT_WRITEFUNCTION,
        espkvm_write_callback
    );

    curl_easy_setopt(
        data->curl,
        CURLOPT_WRITEDATA,
        &response
    );

    result = curl_easy_perform(data->curl);

    if (result != CURLE_OK) {

        guac_client_log(
            client,
            GUAC_LOG_ERROR,
            "ESPKVM session request failed: %s",
            curl_easy_strerror(result)
        );

        free(response.data);
        return 1;
    }

    curl_easy_getinfo(
        data->curl,
        CURLINFO_RESPONSE_CODE,
        &http_code
    );

    if (http_code != 200 || response.data == NULL) {

        guac_client_log(
            client,
            GUAC_LOG_ERROR,
            "ESPKVM session endpoint returned HTTP %ld.",
            http_code
        );

        free(response.data);
        return 1;
    }

    root = json_tokener_parse(response.data);

    free(response.data);

    if (root == NULL)
        return 1;

    if (!json_object_object_get_ex(
            root,
            "required",
            &required_obj)) {

        json_object_put(root);
        return 1;
    }

    if (!json_object_object_get_ex(
            root,
            "authenticated",
            &authenticated_obj)) {

        json_object_put(root);
        return 1;
    }

    *required = json_object_get_boolean(required_obj);
    *authenticated = json_object_get_boolean(authenticated_obj);

    json_object_put(root);

    return 0;
}

static int espkvm_authenticate(
        guac_client* client,
        espkvm_client_data* data,
        const char* username,
        const char* password) {

    int required = 0;
    int authenticated = 0;

    char url[2048];

    espkvm_response response = { NULL, 0 };

    CURLcode result;
    long http_code = 0;

    struct curl_slist* headers = NULL;

    json_object* login = NULL;
    const char* payload;

    /*
     * First ask ESPKVM whether authentication is required.
     */
    if (espkvm_get_session(
            client,
            data,
            &required,
            &authenticated) != 0) {

        guac_client_log(
            client,
            GUAC_LOG_ERROR,
            "Unable to read ESPKVM authentication status."
        );

        return 1;
    }

    /*
     * No login required.
     */
    if (!required)
        return 0;

    /*
     * Already authenticated through an existing cookie.
     */
    if (authenticated)
        return 0;

    if (username == NULL
            || username[0] == '\0'
            || password == NULL
            || password[0] == '\0') {

        guac_client_log(
            client,
            GUAC_LOG_ERROR,
            "ESPKVM requires authentication, but credentials are missing."
        );

        return 1;
    }

    login = json_object_new_object();

    if (login == NULL)
        return 1;

    json_object_object_add(
        login,
        "user",
        json_object_new_string(username)
    );

    json_object_object_add(
        login,
        "password",
        json_object_new_string(password)
    );

    payload = json_object_to_json_string_ext(
        login,
        JSON_C_TO_STRING_PLAIN
    );

    snprintf(
        url,
        sizeof(url),
        "%s/api/v1/auth/login",
        data->base_url
    );

    headers = curl_slist_append(
        headers,
        "Content-Type: application/json"
    );

    curl_easy_setopt(
        data->curl,
        CURLOPT_URL,
        url
    );

    curl_easy_setopt(
        data->curl,
        CURLOPT_HTTPGET,
        0L
    );

    curl_easy_setopt(
        data->curl,
        CURLOPT_POST,
        1L
    );

    curl_easy_setopt(
        data->curl,
        CURLOPT_POSTFIELDS,
        payload
    );

    curl_easy_setopt(
        data->curl,
        CURLOPT_POSTFIELDSIZE,
        (long) strlen(payload)
    );

    curl_easy_setopt(
        data->curl,
        CURLOPT_HTTPHEADER,
        headers
    );

    curl_easy_setopt(
        data->curl,
        CURLOPT_WRITEFUNCTION,
        espkvm_write_callback
    );

    curl_easy_setopt(
        data->curl,
        CURLOPT_WRITEDATA,
        &response
    );

    result = curl_easy_perform(data->curl);

    curl_easy_getinfo(
        data->curl,
        CURLINFO_RESPONSE_CODE,
        &http_code
    );

    curl_easy_setopt(
        data->curl,
        CURLOPT_HTTPHEADER,
        NULL
    );

    curl_slist_free_all(headers);
    json_object_put(login);
    free(response.data);

    if (result != CURLE_OK) {

        guac_client_log(
            client,
            GUAC_LOG_ERROR,
            "HTTPS request to ESPKVM login endpoint failed: %s",
            curl_easy_strerror(result)
        );

        return 1;
    }

    if (http_code != 200) {

        guac_client_log(
            client,
            GUAC_LOG_ERROR,
            "ESPKVM login failed with HTTP status %ld.",
            http_code
        );

        return 1;
    }

    /*
     * Verify that ESPKVM accepted the session cookie.
     */
    if (espkvm_get_session(
            client,
            data,
            &required,
            &authenticated) != 0
            || !authenticated) {

        guac_client_log(
            client,
            GUAC_LOG_ERROR,
            "ESPKVM session was not authenticated after login."
        );

        return 1;
    }

    return 0;
}


/*
 * Per-user ESPKVM control WebSocket.
 *
 * /ws carries keyboard, mouse, HID status and control-state messages.
 *
 * A dedicated CURL handle is used because libcurl easy handles must not be
 * used concurrently from different threads.
 */
typedef struct espkvm_ws_channel {

    guac_user* user;

    guac_stream* tx_stream;
    guac_stream* rx_stream;

    CURL* curl;

    pthread_t thread;
    pthread_mutex_t mutex;

    int thread_started;
    int stop;
    int rx_ended;

} espkvm_ws_channel;


/*
 * Per-user MJPEG /stream transport.
 *
 * This uses its own CURL handle because /stream is a long-lived HTTP
 * response and must never block normal REST requests or the WebSockets.
 */
typedef struct espkvm_mjpeg_stream {

    guac_user* user;

    guac_stream* tx_stream;
    guac_stream* rx_stream;

    CURL* curl;

    pthread_t thread;
    pthread_mutex_t mutex;

    int thread_started;
    int stop;
    int rx_ended;

} espkvm_mjpeg_stream;


typedef struct espkvm_user_data {

    espkvm_ws_channel* control_ws;
    espkvm_ws_channel* video_ws;
    espkvm_mjpeg_stream* mjpeg_stream;

    int video_none_diagnostics_done;

} espkvm_user_data;


static int espkvm_ws_should_stop(
        espkvm_ws_channel* channel) {

    int stop;

    pthread_mutex_lock(&channel->mutex);
    stop = channel->stop;
    pthread_mutex_unlock(&channel->mutex);

    return stop;
}


static void espkvm_ws_end_rx(
        espkvm_ws_channel* channel) {

    int send_end = 0;

    pthread_mutex_lock(&channel->mutex);

    if (!channel->rx_ended) {
        channel->rx_ended = 1;
        send_end = 1;
    }

    pthread_mutex_unlock(&channel->mutex);

    if (send_end && channel->rx_stream != NULL) {

        guac_protocol_send_end(
            channel->user->socket,
            channel->rx_stream
        );

        guac_socket_flush(
            channel->user->socket
        );
    }
}


static void* espkvm_control_ws_receive_thread(
        void* opaque) {

    espkvm_ws_channel* channel =
        (espkvm_ws_channel*) opaque;

    unsigned char buffer[8192];

    while (!espkvm_ws_should_stop(channel)) {

        size_t received = 0;

        const struct curl_ws_frame* meta = NULL;

        CURLcode result;

        pthread_mutex_lock(&channel->mutex);

        if (channel->stop) {
            pthread_mutex_unlock(&channel->mutex);
            break;
        }

        result = curl_ws_recv(
            channel->curl,
            buffer,
            sizeof(buffer),
            &received,
            &meta
        );

        pthread_mutex_unlock(&channel->mutex);

        if (result == CURLE_AGAIN) {
            usleep(10000);
            continue;
        }

        if (result != CURLE_OK) {

            if (!espkvm_ws_should_stop(channel)) {
                guac_client_log(
                    channel->user->client,
                    GUAC_LOG_WARNING,
                    "ESPKVM /ws receive failed: %s",
                    curl_easy_strerror(result)
                );
            }

            break;
        }

        if (meta != NULL
                && (meta->flags & CURLWS_CLOSE)) {
            break;
        }

        /*
         * /ws messages are tiny binary control/status packets.
         */
        if (received > 0
                && meta != NULL
                && (meta->flags & (CURLWS_BINARY | CURLWS_CONT))) {

            if (guac_protocol_send_blob(
                    channel->user->socket,
                    channel->rx_stream,
                    buffer,
                    (int) received) != 0) {

                break;
            }

            guac_socket_flush(
                channel->user->socket
            );
        }
    }

    espkvm_ws_end_rx(channel);

    return NULL;
}


static void espkvm_ws_destroy(
        espkvm_ws_channel* channel) {

    size_t sent = 0;

    if (channel == NULL)
        return;

    /*
     * Tell the receive loop to stop and send a normal WebSocket close frame.
     */
    pthread_mutex_lock(&channel->mutex);

    channel->stop = 1;

    if (channel->curl != NULL) {

        (void) curl_ws_send(
            channel->curl,
            "",
            0,
            &sent,
            0,
            CURLWS_CLOSE
        );
    }

    pthread_mutex_unlock(&channel->mutex);

    if (channel->thread_started) {

        pthread_join(
            channel->thread,
            NULL
        );

        channel->thread_started = 0;
    }

    espkvm_ws_end_rx(channel);

    if (channel->tx_stream != NULL
            && channel->tx_stream->data == channel) {

        channel->tx_stream->data = NULL;
    }

    if (channel->rx_stream != NULL) {

        guac_user_free_stream(
            channel->user,
            channel->rx_stream
        );

        channel->rx_stream = NULL;
    }

    if (channel->curl != NULL) {

        curl_easy_cleanup(channel->curl);
        channel->curl = NULL;
    }

    pthread_mutex_destroy(
        &channel->mutex
    );

    free(channel);
}


/*
 * End the Guacamole receive pipe for an MJPEG stream exactly once.
 */
static void espkvm_mjpeg_end_rx(
        espkvm_mjpeg_stream* stream) {

    int send_end = 0;

    pthread_mutex_lock(&stream->mutex);

    if (!stream->rx_ended) {
        stream->rx_ended = 1;
        send_end = 1;
    }

    pthread_mutex_unlock(&stream->mutex);

    if (send_end && stream->rx_stream != NULL) {

        guac_protocol_send_end(
            stream->user->socket,
            stream->rx_stream
        );

        guac_socket_flush(
            stream->user->socket
        );
    }
}


/*
 * Destroy one per-user MJPEG /stream transport.
 *
 * The actual HTTP worker added later must observe stream->stop and abort its
 * CURL transfer before this function joins the worker thread.
 */
static void espkvm_mjpeg_destroy(
        espkvm_mjpeg_stream* stream) {

    if (stream == NULL)
        return;

    pthread_mutex_lock(&stream->mutex);
    stream->stop = 1;
    pthread_mutex_unlock(&stream->mutex);

    if (stream->thread_started) {

        pthread_join(
            stream->thread,
            NULL
        );

        stream->thread_started = 0;
    }

    espkvm_mjpeg_end_rx(stream);

    if (stream->tx_stream != NULL
            && stream->tx_stream->data == stream) {

        stream->tx_stream->data = NULL;
    }

    if (stream->rx_stream != NULL) {

        guac_user_free_stream(
            stream->user,
            stream->rx_stream
        );

        stream->rx_stream = NULL;
    }

    if (stream->curl != NULL) {

        curl_easy_cleanup(stream->curl);
        stream->curl = NULL;
    }

    pthread_mutex_destroy(
        &stream->mutex
    );

    free(stream);
}


#define ESPKVM_GUAC_BLOB_BYTES 6048U
#define ESPKVM_MJPEG_MAX_FRAME (32u * 1024u * 1024u)
#define ESPKVM_MJPEG_HEADER_LIMIT (64u * 1024u)


typedef struct espkvm_mjpeg_parser {

    espkvm_mjpeg_stream* stream;

    unsigned char* data;
    size_t length;
    size_t capacity;

} espkvm_mjpeg_parser;


/*
 * Return whether an MJPEG worker has been asked to stop.
 */
static int espkvm_mjpeg_should_stop(
        espkvm_mjpeg_stream* stream) {

    int stop;

    pthread_mutex_lock(&stream->mutex);
    stop = stream->stop;
    pthread_mutex_unlock(&stream->mutex);

    return stop;
}


/*
 * Find a short ASCII sequence inside arbitrary binary data.
 */
static const unsigned char* espkvm_mjpeg_find(
        const unsigned char* data,
        size_t length,
        const char* needle,
        size_t needle_length) {

    size_t i;

    if (needle_length == 0 || length < needle_length)
        return NULL;

    for (i = 0;
            i + needle_length <= length;
            i++) {

        if (memcmp(
                data + i,
                needle,
                needle_length) == 0) {

            return data + i;
        }
    }

    return NULL;
}


/*
 * Send exactly one JPEG frame through Guacamole.
 *
 * The browser side receives:
 *
 *   uint32 big-endian JPEG length
 *   JPEG bytes
 */
static int espkvm_mjpeg_send_frame(
        espkvm_mjpeg_stream* stream,
        const unsigned char* data,
        size_t length) {

    unsigned char header[4];
    size_t offset = 0;

    if (length == 0
            || length > ESPKVM_MJPEG_MAX_FRAME) {

        return 1;
    }

    header[0] =
        (unsigned char) ((length >> 24) & 0xff);

    header[1] =
        (unsigned char) ((length >> 16) & 0xff);

    header[2] =
        (unsigned char) ((length >> 8) & 0xff);

    header[3] =
        (unsigned char) (length & 0xff);

    if (guac_protocol_send_blob(
            stream->user->socket,
            stream->rx_stream,
            header,
            4) != 0) {

        return 1;
    }

    while (offset < length) {

        size_t remaining =
            length - offset;

        size_t chunk =
            remaining > ESPKVM_GUAC_BLOB_BYTES
                ? ESPKVM_GUAC_BLOB_BYTES
                : remaining;

        if (guac_protocol_send_blob(
                stream->user->socket,
                stream->rx_stream,
                data + offset,
                (int) chunk) != 0) {

            return 1;
        }

        offset += chunk;
    }

    guac_socket_flush(
        stream->user->socket
    );

    return 0;
}


/*
 * Parse ESPKVM's multipart/x-mixed-replace stream.
 *
 * ESPKVM supplies Content-Length for every JPEG, so there is no need to scan
 * JPEG data for multipart boundaries.
 */
static int espkvm_mjpeg_parse(
        espkvm_mjpeg_parser* parser) {

    while (parser->length > 0) {

        const unsigned char* header_end;
        const unsigned char* content_length_header;

        size_t header_length;
        size_t payload_offset;
        size_t content_length = 0;
        size_t i;

        header_end = espkvm_mjpeg_find(
            parser->data,
            parser->length,
            "\r\n\r\n",
            4
        );

        if (header_end == NULL) {

            if (parser->length
                    > ESPKVM_MJPEG_HEADER_LIMIT) {

                return 1;
            }

            return 0;
        }

        header_length =
            (size_t) (header_end - parser->data);

        content_length_header =
            espkvm_mjpeg_find(
                parser->data,
                header_length,
                "Content-Length:",
                strlen("Content-Length:")
            );

        if (content_length_header == NULL)
            return 1;

        i =
            (size_t) (
                content_length_header
                - parser->data
            )
            + strlen("Content-Length:");

        while (i < header_length
                && (parser->data[i] == ' '
                    || parser->data[i] == '\t')) {

            i++;
        }

        if (i >= header_length
                || parser->data[i] < '0'
                || parser->data[i] > '9') {

            return 1;
        }

        while (i < header_length
                && parser->data[i] >= '0'
                && parser->data[i] <= '9') {

            size_t digit =
                (size_t) (
                    parser->data[i] - '0'
                );

            if (content_length
                    > (
                        ESPKVM_MJPEG_MAX_FRAME
                        - digit
                    ) / 10u) {

                return 1;
            }

            content_length =
                content_length * 10u
                + digit;

            i++;
        }

        if (content_length == 0
                || content_length
                    > ESPKVM_MJPEG_MAX_FRAME) {

            return 1;
        }

        payload_offset =
            header_length + 4u;

        if (parser->length
                < payload_offset + content_length) {

            return 0;
        }

        if (espkvm_mjpeg_send_frame(
                parser->stream,
                parser->data + payload_offset,
                content_length) != 0) {

            return 1;
        }

        {
            size_t consumed =
                payload_offset
                + content_length;

            size_t remaining =
                parser->length
                - consumed;

            if (remaining > 0) {

                memmove(
                    parser->data,
                    parser->data + consumed,
                    remaining
                );
            }

            parser->length =
                remaining;
        }
    }

    return 0;
}


/*
 * libcurl body callback for /stream.
 */
static size_t espkvm_mjpeg_write_callback(
        char* ptr,
        size_t size,
        size_t nmemb,
        void* userdata) {

    espkvm_mjpeg_parser* parser =
        (espkvm_mjpeg_parser*) userdata;

    size_t bytes;
    size_t needed;

    unsigned char* resized;

    if (parser == NULL
            || parser->stream == NULL) {

        return 0;
    }

    if (espkvm_mjpeg_should_stop(
            parser->stream)) {

        return 0;
    }

    if (size != 0
            && nmemb > SIZE_MAX / size) {

        return 0;
    }

    bytes =
        size * nmemb;

    if (bytes == 0)
        return 0;

    if (parser->length
            > SIZE_MAX - bytes) {

        return 0;
    }

    needed =
        parser->length + bytes;

    if (needed
            > ESPKVM_MJPEG_MAX_FRAME
                + ESPKVM_MJPEG_HEADER_LIMIT) {

        return 0;
    }

    if (needed > parser->capacity) {

        size_t capacity =
            parser->capacity
                ? parser->capacity
                : 65536u;

        while (capacity < needed) {

            if (capacity
                    > (
                        ESPKVM_MJPEG_MAX_FRAME
                        + ESPKVM_MJPEG_HEADER_LIMIT
                    ) / 2u) {

                capacity =
                    ESPKVM_MJPEG_MAX_FRAME
                    + ESPKVM_MJPEG_HEADER_LIMIT;

                break;
            }

            capacity *= 2u;
        }

        resized =
            realloc(
                parser->data,
                capacity
            );

        if (resized == NULL)
            return 0;

        parser->data =
            resized;

        parser->capacity =
            capacity;
    }

    memcpy(
        parser->data + parser->length,
        ptr,
        bytes
    );

    parser->length += bytes;

    if (espkvm_mjpeg_parse(parser) != 0)
        return 0;

    return bytes;
}


/*
 * libcurl progress callback.
 *
 * This gives espkvm_mjpeg_destroy() a way to interrupt a long-lived
 * curl_easy_perform() without using the same CURL handle from two threads.
 */
static int espkvm_mjpeg_progress_callback(
        void* userdata,
        curl_off_t dltotal,
        curl_off_t dlnow,
        curl_off_t ultotal,
        curl_off_t ulnow) {

    espkvm_mjpeg_stream* stream =
        (espkvm_mjpeg_stream*) userdata;

    (void) dltotal;
    (void) dlnow;
    (void) ultotal;
    (void) ulnow;

    return espkvm_mjpeg_should_stop(
        stream
    );
}


/*
 * Long-lived worker for ESPKVM GET /stream.
 */
static void* espkvm_mjpeg_worker(
        void* opaque) {

    espkvm_mjpeg_stream* stream =
        (espkvm_mjpeg_stream*) opaque;

    espkvm_mjpeg_parser parser;

    CURLcode result;
    long status = 0;

    memset(
        &parser,
        0,
        sizeof(parser)
    );

    parser.stream =
        stream;

    curl_easy_setopt(
        stream->curl,
        CURLOPT_TIMEOUT,
        0L
    );

    curl_easy_setopt(
        stream->curl,
        CURLOPT_WRITEFUNCTION,
        espkvm_mjpeg_write_callback
    );

    curl_easy_setopt(
        stream->curl,
        CURLOPT_WRITEDATA,
        &parser
    );

    curl_easy_setopt(
        stream->curl,
        CURLOPT_NOPROGRESS,
        0L
    );

    curl_easy_setopt(
        stream->curl,
        CURLOPT_XFERINFOFUNCTION,
        espkvm_mjpeg_progress_callback
    );

    curl_easy_setopt(
        stream->curl,
        CURLOPT_XFERINFODATA,
        stream
    );

    result =
        curl_easy_perform(
            stream->curl
        );

    (void) curl_easy_getinfo(
        stream->curl,
        CURLINFO_RESPONSE_CODE,
        &status
    );

    guac_client_log(
        stream->user->client,
        GUAC_LOG_INFO,
        "ESPKVM /stream worker ended: curl=%d (%s), HTTP=%ld, stop=%d.",
        (int) result,
        curl_easy_strerror(result),
        status,
        espkvm_mjpeg_should_stop(stream)
    );

    if (result != CURLE_OK
            && result != CURLE_ABORTED_BY_CALLBACK
            && !espkvm_mjpeg_should_stop(stream)) {

        guac_client_log(
            stream->user->client,
            GUAC_LOG_WARNING,
            "ESPKVM /stream failed: %s",
            curl_easy_strerror(result)
        );
    }

    if (status >= 400
            && !espkvm_mjpeg_should_stop(stream)) {

        guac_client_log(
            stream->user->client,
            GUAC_LOG_WARNING,
            "ESPKVM /stream returned HTTP %ld.",
            status
        );
    }

    free(parser.data);

    espkvm_mjpeg_end_rx(
        stream
    );

    return NULL;
}


static int espkvm_control_ws_blob_handler(
        guac_user* user,
        guac_stream* stream,
        void* data,
        int length) {

    espkvm_ws_channel* channel =
        (espkvm_ws_channel*) stream->data;

    const unsigned char* bytes =
        (const unsigned char*) data;

    size_t offset = 0;

    if (channel == NULL || length < 0)
        return 1;

    /*
     * ESPKVM's /ws commands are tiny (normally 1-8 bytes), but handle partial
     * libcurl writes correctly regardless.
     */
    while (offset < (size_t) length) {

        size_t sent = 0;

        CURLcode result;

        pthread_mutex_lock(
            &channel->mutex
        );

        if (channel->stop) {
            pthread_mutex_unlock(&channel->mutex);
            return 1;
        }

        result = curl_ws_send(
            channel->curl,
            bytes + offset,
            (size_t) length - offset,
            &sent,
            0,
            CURLWS_BINARY
        );

        pthread_mutex_unlock(
            &channel->mutex
        );

        if (result == CURLE_AGAIN) {
            usleep(1000);
            continue;
        }

        if (result != CURLE_OK) {

            guac_client_log(
                user->client,
                GUAC_LOG_ERROR,
                "ESPKVM /ws send failed: %s",
                curl_easy_strerror(result)
            );

            return 1;
        }

        if (sent == 0)
            return 1;

        offset += sent;
    }

    guac_protocol_send_ack(
        user->socket,
        stream,
        "OK",
        GUAC_PROTOCOL_STATUS_SUCCESS
    );

    guac_socket_flush(
        user->socket
    );

    return 0;
}


static int espkvm_control_ws_end_handler(
        guac_user* user,
        guac_stream* stream) {

    espkvm_ws_channel* channel =
        (espkvm_ws_channel*) stream->data;

    espkvm_user_data* user_data =
        (espkvm_user_data*) user->data;

    stream->data = NULL;

    if (user_data != NULL
            && user_data->control_ws == channel) {

        user_data->control_ws = NULL;
    }

    espkvm_ws_destroy(channel);

    return 0;
}


static int espkvm_control_ws_open(
        guac_user* user,
        guac_stream* tx_stream,
        const char* request_id) {

    espkvm_client_data* client_data =
        (espkvm_client_data*) user->client->data;

    espkvm_user_data* user_data =
        (espkvm_user_data*) user->data;

    espkvm_ws_channel* channel = NULL;

    struct curl_slist* cookies = NULL;
    struct curl_slist* cookie = NULL;

    CURLcode result;

    char* ws_url = NULL;

    char rx_name[96];

    size_t needed;

    const char* host_part;

    if (client_data == NULL || user_data == NULL)
        return 1;

    if (request_id == NULL || request_id[0] == '\0')
        return 1;

    /*
     * Only one control socket is needed by one ESPKVM console.
     */
    if (user_data->control_ws != NULL)
        return 1;

    channel = calloc(
        1,
        sizeof(espkvm_ws_channel)
    );

    if (channel == NULL)
        return 1;

    channel->user = user;
    channel->tx_stream = tx_stream;

    if (pthread_mutex_init(
            &channel->mutex,
            NULL) != 0) {

        free(channel);
        return 1;
    }

    channel->curl = curl_easy_init();

    if (channel->curl == NULL) {
        pthread_mutex_destroy(&channel->mutex);
        free(channel);
        return 1;
    }

    espkvm_prepare_curl(
        channel->curl,
        client_data->ignore_cert
    );

    /*
     * Copy the authenticated ESPKVM cookies to this independent WebSocket
     * handle. curl_easy_duphandle() deliberately does not copy cookies.
     */
    result = curl_easy_getinfo(
        client_data->curl,
        CURLINFO_COOKIELIST,
        &cookies
    );

    if (result != CURLE_OK)
        goto fail;

    for (cookie = cookies;
            cookie != NULL;
            cookie = cookie->next) {

        result = curl_easy_setopt(
            channel->curl,
            CURLOPT_COOKIELIST,
            cookie->data
        );

        if (result != CURLE_OK)
            goto fail;
    }

    /*
     * base_url is always https://... in this plugin.
     */
    if (strncmp(
            client_data->base_url,
            "https://",
            8) == 0) {

        host_part =
            client_data->base_url + 8;
    }
    else {
        host_part =
            client_data->base_url;
    }

    needed =
        strlen(host_part)
        + strlen("wss:///ws")
        + 1;

    ws_url = malloc(needed);

    if (ws_url == NULL)
        goto fail;

    snprintf(
        ws_url,
        needed,
        "wss://%s/ws",
        host_part
    );

    curl_easy_setopt(
        channel->curl,
        CURLOPT_URL,
        ws_url
    );

    /*
     * WebSocket connect-only mode:
     * curl_easy_perform() completes the HTTP Upgrade and then returns.
     */
    curl_easy_setopt(
        channel->curl,
        CURLOPT_CONNECT_ONLY,
        2L
    );

    result = curl_easy_perform(
        channel->curl
    );

    if (result != CURLE_OK) {

        guac_client_log(
            user->client,
            GUAC_LOG_ERROR,
            "Unable to open ESPKVM /ws: %s",
            curl_easy_strerror(result)
        );

        goto fail;
    }

    channel->rx_stream =
        guac_user_alloc_stream(user);

    if (channel->rx_stream == NULL)
        goto fail;

    if (snprintf(
            rx_name,
            sizeof(rx_name),
            "espkvm:control-ws:%s:rx",
            request_id)
            >= (int) sizeof(rx_name)) {

        goto fail;
    }

    /*
     * The arrival of this pipe tells the browser that WebSocket.open has
     * completed successfully.
     */
    if (guac_protocol_send_pipe(
            user->socket,
            channel->rx_stream,
            "application/octet-stream",
            rx_name) != 0) {

        goto fail;
    }

    guac_socket_flush(
        user->socket
    );

    tx_stream->data = channel;

    tx_stream->blob_handler =
        espkvm_control_ws_blob_handler;

    tx_stream->end_handler =
        espkvm_control_ws_end_handler;

    user_data->control_ws = channel;

    if (pthread_create(
            &channel->thread,
            NULL,
            espkvm_control_ws_receive_thread,
            channel) != 0) {

        user_data->control_ws = NULL;
        tx_stream->data = NULL;
        goto fail;
    }

    channel->thread_started = 1;

    curl_slist_free_all(cookies);

    free(ws_url);

    guac_client_log(
        user->client,
        GUAC_LOG_INFO,
        "ESPKVM /ws connected."
    );

    return 0;

fail:

    curl_slist_free_all(cookies);

    free(ws_url);

    if (channel != NULL) {

        if (channel->rx_stream != NULL) {

            guac_protocol_send_end(
                user->socket,
                channel->rx_stream
            );

            guac_user_free_stream(
                user,
                channel->rx_stream
            );

            channel->rx_stream = NULL;
        }

        if (channel->curl != NULL)
            curl_easy_cleanup(channel->curl);

        pthread_mutex_destroy(
            &channel->mutex
        );

        free(channel);
    }

    return 1;
}



#define ESPKVM_VIDEO_MAX_MESSAGE (32U * 1024U * 1024U)


static int espkvm_video_send_message(
        espkvm_ws_channel* channel,
        const unsigned char* data,
        size_t length) {

    unsigned char header[4];
    uint32_t message_length;
    size_t offset = 0;

    if (length > UINT32_MAX)
        return 1;

    message_length = (uint32_t) length;

    header[0] =
        (unsigned char) ((message_length >> 24) & 0xff);

    header[1] =
        (unsigned char) ((message_length >> 16) & 0xff);

    header[2] =
        (unsigned char) ((message_length >> 8) & 0xff);

    header[3] =
        (unsigned char) (message_length & 0xff);

    if (guac_protocol_send_blob(
            channel->user->socket,
            channel->rx_stream,
            header,
            sizeof(header)) != 0) {

        return 1;
    }

    while (offset < length) {

        size_t remaining =
            length - offset;

        size_t chunk =
            remaining > ESPKVM_GUAC_BLOB_BYTES
                ? ESPKVM_GUAC_BLOB_BYTES
                : remaining;

        if (guac_protocol_send_blob(
                channel->user->socket,
                channel->rx_stream,
                data + offset,
                (int) chunk) != 0) {

            return 1;
        }

        offset += chunk;
    }

    guac_socket_flush(
        channel->user->socket
    );

    return 0;
}


static int espkvm_video_append(
        unsigned char** message,
        size_t* message_length,
        size_t* message_capacity,
        const unsigned char* data,
        size_t length) {

    size_t needed;
    size_t new_capacity;
    unsigned char* new_message;

    if (length == 0)
        return 0;

    if (*message_length >
            ESPKVM_VIDEO_MAX_MESSAGE - length) {

        return 1;
    }

    needed =
        *message_length + length;

    if (needed >
            ESPKVM_VIDEO_MAX_MESSAGE) {

        return 1;
    }

    if (needed <= *message_capacity) {

        memcpy(
            *message + *message_length,
            data,
            length
        );

        *message_length = needed;

        return 0;
    }

    new_capacity =
        *message_capacity != 0
            ? *message_capacity
            : 65536U;

    while (new_capacity < needed) {

        if (new_capacity >
                ESPKVM_VIDEO_MAX_MESSAGE / 2U) {

            new_capacity =
                ESPKVM_VIDEO_MAX_MESSAGE;

            break;
        }

        new_capacity *= 2U;
    }

    new_message = realloc(
        *message,
        new_capacity
    );

    if (new_message == NULL)
        return 1;

    *message = new_message;
    *message_capacity = new_capacity;

    memcpy(
        *message + *message_length,
        data,
        length
    );

    *message_length = needed;

    return 0;
}


static void* espkvm_video_ws_receive_thread(
        void* opaque) {

    espkvm_ws_channel* channel =
        (espkvm_ws_channel*) opaque;

    unsigned char buffer[8192];

    unsigned char* message = NULL;
    size_t message_length = 0;
    size_t message_capacity = 0;

    while (!espkvm_ws_should_stop(channel)) {

        size_t received = 0;

        const struct curl_ws_frame* meta = NULL;

        CURLcode result;

        int flags = 0;
        curl_off_t bytesleft = 0;

        pthread_mutex_lock(
            &channel->mutex
        );

        if (channel->stop) {

            pthread_mutex_unlock(
                &channel->mutex
            );

            break;
        }

        result = curl_ws_recv(
            channel->curl,
            buffer,
            sizeof(buffer),
            &received,
            &meta
        );

        /*
         * Copy metadata while holding the mutex. libcurl documents the
         * metadata pointer as valid only until another WebSocket call.
         */
        if (result == CURLE_OK
                && meta != NULL) {

            flags = meta->flags;
            bytesleft = meta->bytesleft;
        }

        pthread_mutex_unlock(
            &channel->mutex
        );

        if (result == CURLE_AGAIN) {

            usleep(5000);
            continue;
        }

        if (result != CURLE_OK) {

            if (!espkvm_ws_should_stop(channel)) {

                guac_client_log(
                    channel->user->client,
                    GUAC_LOG_WARNING,
                    "ESPKVM /video receive failed: %s",
                    curl_easy_strerror(result)
                );
            }

            break;
        }

        if (flags & CURLWS_CLOSE)
            break;

        /*
         * Ping/Pong control frames may appear between fragments of one
         * WebSocket message. libcurl handles Pong replies automatically.
         */
        if (flags & (CURLWS_PING | CURLWS_PONG))
            continue;

        if (!(flags & CURLWS_BINARY))
            continue;

        if (received > 0) {

            if (espkvm_video_append(
                    &message,
                    &message_length,
                    &message_capacity,
                    buffer,
                    received) != 0) {

                guac_client_log(
                    channel->user->client,
                    GUAC_LOG_ERROR,
                    "ESPKVM /video message exceeded safety limit."
                );

                break;
            }
        }

        /*
         * bytesleft == 0 means this complete WebSocket FRAME has arrived.
         *
         * CURLWS_CONT means another WebSocket FRAME still belongs to the
         * same WebSocket MESSAGE.
         */
        if (bytesleft == 0
                && !(flags & CURLWS_CONT)) {

            if (message_length > 0) {

                if (espkvm_video_send_message(
                        channel,
                        message,
                        message_length) != 0) {

                    break;
                }
            }

            message_length = 0;
        }
    }

    free(message);

    espkvm_ws_end_rx(channel);

    return NULL;
}


static int espkvm_video_ws_blob_handler(
        guac_user* user,
        guac_stream* stream,
        void* data,
        int length) {

    espkvm_ws_channel* channel =
        (espkvm_ws_channel*) stream->data;

    const unsigned char* bytes =
        (const unsigned char*) data;

    size_t offset = 0;

    if (channel == NULL || length < 0)
        return 1;

    while (offset < (size_t) length) {

        size_t sent = 0;
        CURLcode result;

        pthread_mutex_lock(
            &channel->mutex
        );

        if (channel->stop) {

            pthread_mutex_unlock(
                &channel->mutex
            );

            return 1;
        }

        result = curl_ws_send(
            channel->curl,
            bytes + offset,
            (size_t) length - offset,
            &sent,
            0,
            CURLWS_BINARY
        );

        pthread_mutex_unlock(
            &channel->mutex
        );

        if (result == CURLE_AGAIN) {

            usleep(1000);
            continue;
        }

        if (result != CURLE_OK) {

            guac_client_log(
                user->client,
                GUAC_LOG_ERROR,
                "ESPKVM /video send failed: %s",
                curl_easy_strerror(result)
            );

            return 1;
        }

        if (sent == 0)
            return 1;

        offset += sent;
    }

    guac_protocol_send_ack(
        user->socket,
        stream,
        "OK",
        GUAC_PROTOCOL_STATUS_SUCCESS
    );

    guac_socket_flush(
        user->socket
    );

    return 0;
}


static int espkvm_video_ws_end_handler(
        guac_user* user,
        guac_stream* stream) {

    espkvm_ws_channel* channel =
        (espkvm_ws_channel*) stream->data;

    espkvm_user_data* user_data =
        (espkvm_user_data*) user->data;

    stream->data = NULL;

    if (user_data != NULL
            && user_data->video_ws == channel) {

        user_data->video_ws = NULL;
    }

    espkvm_ws_destroy(channel);

    return 0;
}


static int espkvm_video_ws_open(
        guac_user* user,
        guac_stream* tx_stream,
        const char* request_id) {

    espkvm_client_data* client_data =
        (espkvm_client_data*) user->client->data;

    espkvm_user_data* user_data =
        (espkvm_user_data*) user->data;

    espkvm_ws_channel* channel = NULL;

    struct curl_slist* cookies = NULL;
    struct curl_slist* cookie = NULL;

    CURLcode result;

    char* ws_url = NULL;
    char rx_name[96];

    size_t needed;
    const char* host_part;

    if (client_data == NULL
            || user_data == NULL) {

        return 1;
    }

    if (request_id == NULL
            || request_id[0] == '\0') {

        return 1;
    }

    if (user_data->video_ws != NULL)
        return 1;

    channel = calloc(
        1,
        sizeof(espkvm_ws_channel)
    );

    if (channel == NULL)
        return 1;

    channel->user = user;
    channel->tx_stream = tx_stream;

    if (pthread_mutex_init(
            &channel->mutex,
            NULL) != 0) {

        free(channel);
        return 1;
    }

    channel->curl =
        curl_easy_init();

    if (channel->curl == NULL) {

        pthread_mutex_destroy(
            &channel->mutex
        );

        free(channel);
        return 1;
    }

    espkvm_prepare_curl(
        channel->curl,
        client_data->ignore_cert
    );

    /*
     * Copy the already-authenticated ESPKVM session cookie.
     */
    result = curl_easy_getinfo(
        client_data->curl,
        CURLINFO_COOKIELIST,
        &cookies
    );

    if (result != CURLE_OK)
        goto fail;

    for (cookie = cookies;
            cookie != NULL;
            cookie = cookie->next) {

        result = curl_easy_setopt(
            channel->curl,
            CURLOPT_COOKIELIST,
            cookie->data
        );

        if (result != CURLE_OK)
            goto fail;
    }

    if (strncmp(
            client_data->base_url,
            "https://",
            8) == 0) {

        host_part =
            client_data->base_url + 8;
    }
    else {

        host_part =
            client_data->base_url;
    }

    needed =
        strlen(host_part)
        + strlen("wss:///video")
        + 1;

    ws_url = malloc(needed);

    if (ws_url == NULL)
        goto fail;

    snprintf(
        ws_url,
        needed,
        "wss://%s/video",
        host_part
    );

    curl_easy_setopt(
        channel->curl,
        CURLOPT_URL,
        ws_url
    );

    curl_easy_setopt(
        channel->curl,
        CURLOPT_CONNECT_ONLY,
        2L
    );

    result = curl_easy_perform(
        channel->curl
    );

    if (result != CURLE_OK) {

        guac_client_log(
            user->client,
            GUAC_LOG_ERROR,
            "Unable to open ESPKVM /video: %s",
            curl_easy_strerror(result)
        );

        goto fail;
    }

    channel->rx_stream =
        guac_user_alloc_stream(user);

    if (channel->rx_stream == NULL)
        goto fail;

    if (snprintf(
            rx_name,
            sizeof(rx_name),
            "espkvm:video-ws:%s:rx",
            request_id)
            >= (int) sizeof(rx_name)) {

        goto fail;
    }

    if (guac_protocol_send_pipe(
            user->socket,
            channel->rx_stream,
            "application/octet-stream",
            rx_name) != 0) {

        goto fail;
    }

    guac_socket_flush(
        user->socket
    );

    tx_stream->data = channel;

    tx_stream->blob_handler =
        espkvm_video_ws_blob_handler;

    tx_stream->end_handler =
        espkvm_video_ws_end_handler;

    user_data->video_ws = channel;

    if (pthread_create(
            &channel->thread,
            NULL,
            espkvm_video_ws_receive_thread,
            channel) != 0) {

        user_data->video_ws = NULL;
        tx_stream->data = NULL;

        goto fail;
    }

    channel->thread_started = 1;

    curl_slist_free_all(cookies);
    free(ws_url);

    guac_client_log(
        user->client,
        GUAC_LOG_INFO,
        "ESPKVM /video connected."
    );

    return 0;

fail:

    curl_slist_free_all(cookies);
    free(ws_url);

    if (channel != NULL) {

        if (channel->rx_stream != NULL) {

            guac_protocol_send_end(
                user->socket,
                channel->rx_stream
            );

            guac_user_free_stream(
                user,
                channel->rx_stream
            );

            channel->rx_stream = NULL;
        }

        if (channel->curl != NULL) {

            curl_easy_cleanup(
                channel->curl
            );
        }

        pthread_mutex_destroy(
            &channel->mutex
        );

        free(channel);
    }

    return 1;
}



static int espkvm_mjpeg_end_handler(
        guac_user* user,
        guac_stream* guac_stream) {

    espkvm_mjpeg_stream* stream =
        (espkvm_mjpeg_stream*) guac_stream->data;

    espkvm_user_data* user_data =
        (espkvm_user_data*) user->data;

    if (stream == NULL)
        return 0;

    guac_client_log(
        user->client,
        GUAC_LOG_INFO,
        "ESPKVM /stream TX pipe ended by browser."
    );

    guac_stream->data = NULL;

    if (user_data != NULL
            && user_data->mjpeg_stream == stream) {

        user_data->mjpeg_stream = NULL;
    }

    espkvm_mjpeg_destroy(stream);

    return 0;
}


static int espkvm_mjpeg_open(
        guac_user* user,
        guac_stream* tx_stream,
        const char* request_id) {

    espkvm_client_data* client_data =
        (espkvm_client_data*) user->client->data;

    espkvm_user_data* user_data =
        (espkvm_user_data*) user->data;

    espkvm_mjpeg_stream* stream = NULL;

    struct curl_slist* cookies = NULL;
    struct curl_slist* cookie = NULL;

    CURLcode result;

    char* stream_url = NULL;
    char rx_name[96];

    size_t needed;

    if (client_data == NULL
            || user_data == NULL) {

        return 1;
    }

    if (request_id == NULL
            || request_id[0] == '\0') {

        return 1;
    }

    if (user_data->mjpeg_stream != NULL)
        return 1;

    stream = calloc(
        1,
        sizeof(espkvm_mjpeg_stream)
    );

    if (stream == NULL)
        return 1;

    stream->user = user;
    stream->tx_stream = tx_stream;

    if (pthread_mutex_init(
            &stream->mutex,
            NULL) != 0) {

        free(stream);
        return 1;
    }

    stream->curl =
        curl_easy_init();

    if (stream->curl == NULL) {

        pthread_mutex_destroy(
            &stream->mutex
        );

        free(stream);
        return 1;
    }

    espkvm_prepare_curl(
        stream->curl,
        client_data->ignore_cert
    );

    /*
     * /stream is long-lived. Override the normal REST timeout.
     */
    curl_easy_setopt(
        stream->curl,
        CURLOPT_TIMEOUT,
        0L
    );

    /*
     * Multipart JPEG data must arrive unchanged.
     */
    curl_easy_setopt(
        stream->curl,
        CURLOPT_ACCEPT_ENCODING,
        "identity"
    );

    /*
     * Copy the authenticated ESPKVM session cookie.
     */
    result = curl_easy_getinfo(
        client_data->curl,
        CURLINFO_COOKIELIST,
        &cookies
    );

    if (result != CURLE_OK)
        goto fail;

    for (cookie = cookies;
            cookie != NULL;
            cookie = cookie->next) {

        result = curl_easy_setopt(
            stream->curl,
            CURLOPT_COOKIELIST,
            cookie->data
        );

        if (result != CURLE_OK)
            goto fail;
    }

    needed =
        strlen(client_data->base_url)
        + strlen("/stream")
        + 1;

    stream_url =
        malloc(needed);

    if (stream_url == NULL)
        goto fail;

    snprintf(
        stream_url,
        needed,
        "%s/stream",
        client_data->base_url
    );

    result = curl_easy_setopt(
        stream->curl,
        CURLOPT_URL,
        stream_url
    );

    if (result != CURLE_OK)
        goto fail;

    stream->rx_stream =
        guac_user_alloc_stream(user);

    if (stream->rx_stream == NULL)
        goto fail;

    if (snprintf(
            rx_name,
            sizeof(rx_name),
            "espkvm:mjpeg-stream:%s:rx",
            request_id)
            >= (int) sizeof(rx_name)) {

        goto fail;
    }

    if (guac_protocol_send_pipe(
            user->socket,
            stream->rx_stream,
            "application/octet-stream",
            rx_name) != 0) {

        goto fail;
    }

    guac_socket_flush(
        user->socket
    );

    tx_stream->data = stream;
    tx_stream->end_handler =
        espkvm_mjpeg_end_handler;

    user_data->mjpeg_stream =
        stream;

    if (pthread_create(
            &stream->thread,
            NULL,
            espkvm_mjpeg_worker,
            stream) != 0) {

        user_data->mjpeg_stream = NULL;
        tx_stream->data = NULL;

        goto fail;
    }

    stream->thread_started = 1;

    curl_slist_free_all(cookies);
    free(stream_url);

    guac_client_log(
        user->client,
        GUAC_LOG_INFO,
        "ESPKVM /stream connected."
    );

    return 0;

fail:

    curl_slist_free_all(cookies);
    free(stream_url);

    if (stream != NULL) {

        if (stream->rx_stream != NULL) {

            guac_protocol_send_end(
                user->socket,
                stream->rx_stream
            );

            guac_user_free_stream(
                user,
                stream->rx_stream
            );

            stream->rx_stream = NULL;
        }

        if (stream->curl != NULL) {

            curl_easy_cleanup(
                stream->curl
            );

            stream->curl = NULL;
        }

        pthread_mutex_destroy(
            &stream->mutex
        );

        free(stream);
    }

    return 1;
}


static int espkvm_user_leave_handler(
        guac_user* user) {

    espkvm_user_data* user_data =
        (espkvm_user_data*) user->data;

    if (user_data == NULL)
        return 0;

    if (user_data->control_ws != NULL) {

        espkvm_ws_destroy(
            user_data->control_ws
        );

        user_data->control_ws = NULL;
    }

    if (user_data->video_ws != NULL) {

        espkvm_ws_destroy(
            user_data->video_ws
        );

        user_data->video_ws = NULL;
    }

    if (user_data->mjpeg_stream != NULL) {

        espkvm_mjpeg_destroy(
            user_data->mjpeg_stream
        );

        user_data->mjpeg_stream = NULL;
    }

    free(user_data);
    user->data = NULL;

    return 0;
}


static int espkvm_free_handler(guac_client* client) {

    espkvm_client_data* data = client->data;

    if (data == NULL)
        return 0;

    if (data->curl != NULL)
        curl_easy_cleanup(data->curl);

    free(data->base_url);
    free(data);

    client->data = NULL;

    return 0;
}


/*
 * Temporary ESPKVM pipe round-trip test.
 *
 * Browser:
 *   espkvm:echo
 *
 * Server response:
 *   espkvm:echo:reply
 */
typedef struct espkvm_echo_stream_data {
    guac_stream* response;
} espkvm_echo_stream_data;

static int espkvm_echo_blob_handler(
        guac_user* user,
        guac_stream* stream,
        void* data,
        int length) {

    espkvm_echo_stream_data* echo =
        (espkvm_echo_stream_data*) stream->data;

    if (echo == NULL || echo->response == NULL)
        return 1;

    if (guac_protocol_send_blob(
            user->socket,
            echo->response,
            data,
            length) != 0)
        return 1;

    /*
     * Acknowledge successful processing of the incoming blob.
     */
    guac_protocol_send_ack(
        user->socket,
        stream,
        "OK",
        GUAC_PROTOCOL_STATUS_SUCCESS
    );

    guac_socket_flush(user->socket);

    return 0;
}

static int espkvm_echo_end_handler(
        guac_user* user,
        guac_stream* stream) {

    espkvm_echo_stream_data* echo =
        (espkvm_echo_stream_data*) stream->data;

    if (echo != NULL) {

        if (echo->response != NULL) {

            guac_protocol_send_end(
                user->socket,
                echo->response
            );

            guac_socket_flush(user->socket);

            guac_user_free_stream(
                user,
                echo->response
            );
        }

        free(echo);
        stream->data = NULL;
    }

    return 0;
}


/*
 * Returns the current authenticated ESPKVM session state through a
 * server-initiated Guacamole pipe.
 */
static int espkvm_session_test_end_handler(
        guac_user* user,
        guac_stream* stream) {

    guac_client* client = user->client;
    espkvm_client_data* data =
        (espkvm_client_data*) client->data;

    guac_stream* response;

    int required = 0;
    int authenticated = 0;

    char json[128];
    int length;

    (void) stream;

    if (data == NULL)
        return 1;

    if (espkvm_get_session(
            client,
            data,
            &required,
            &authenticated) != 0) {

        guac_client_log(
            client,
            GUAC_LOG_ERROR,
            "ESPKVM session test failed."
        );

        return 1;
    }

    length = snprintf(
        json,
        sizeof(json),
        "{\"required\":%s,\"authenticated\":%s}",
        required ? "true" : "false",
        authenticated ? "true" : "false"
    );

    if (length < 0 || (size_t) length >= sizeof(json))
        return 1;

    response = guac_user_alloc_stream(user);

    if (response == NULL)
        return 1;

    if (guac_protocol_send_pipe(
            user->socket,
            response,
            "application/json",
            "espkvm:session-test:reply") != 0) {

        guac_user_free_stream(user, response);
        return 1;
    }

    if (guac_protocol_send_blob(
            user->socket,
            response,
            json,
            length) != 0) {

        guac_user_free_stream(user, response);
        return 1;
    }

    guac_protocol_send_end(
        user->socket,
        response
    );

    guac_socket_flush(user->socket);

    guac_user_free_stream(
        user,
        response
    );

    return 0;
}


/*
 * Temporary generic HTTP GET transport.
 *
 * The browser opens:
 *
 *   espkvm:http:<request-id>
 *
 * and sends an ESPKVM API path as UTF-8 text.
 *
 * The reply is returned through:
 *
 *   espkvm:http:<request-id>:reply
 *
 * as a JSON envelope containing the HTTP status, content type and body.
 *
 * This stage intentionally handles text-based GET /api/... requests only.
 * Binary and streaming transports are added separately.
 */
typedef struct espkvm_http_get_stream_data {

    char request_id[32];

    char* path;
    size_t path_length;

} espkvm_http_get_stream_data;


static int espkvm_http_send_reply(
        guac_user* user,
        const char* request_id,
        long status,
        const char* content_type,
        const char* body) {

    guac_stream* response;
    json_object* envelope;

    const char* json;
    char reply_name[96];

    int written;

    written = snprintf(
        reply_name,
        sizeof(reply_name),
        "espkvm:http:%s:reply",
        request_id
    );

    if (written < 0 || (size_t) written >= sizeof(reply_name))
        return 1;

    envelope = json_object_new_object();

    if (envelope == NULL)
        return 1;

    json_object_object_add(
        envelope,
        "status",
        json_object_new_int64(status)
    );

    json_object_object_add(
        envelope,
        "contentType",
        json_object_new_string(
            content_type != NULL ? content_type : ""
        )
    );

    json_object_object_add(
        envelope,
        "body",
        json_object_new_string(
            body != NULL ? body : ""
        )
    );

    json = json_object_to_json_string_ext(
        envelope,
        JSON_C_TO_STRING_PLAIN
    );

    if (json == NULL) {
        json_object_put(envelope);
        return 1;
    }

    response = guac_user_alloc_stream(user);

    if (response == NULL) {
        json_object_put(envelope);
        return 1;
    }

    if (guac_protocol_send_pipe(
            user->socket,
            response,
            "application/json",
            reply_name) != 0) {

        guac_user_free_stream(user, response);
        json_object_put(envelope);
        return 1;
    }

    /*
     * Send large responses in reasonably-sized Guacamole blobs.
     * The ESPKVM web console itself is currently around 285 KiB.
     */
    {
        size_t json_length = strlen(json);
        size_t offset = 0;

        while (offset < json_length) {

            size_t remaining = json_length - offset;
            int chunk_length = remaining > 8192
                ? 8192
                : (int) remaining;

            if (guac_protocol_send_blob(
                    user->socket,
                    response,
                    json + offset,
                    chunk_length) != 0) {

                guac_user_free_stream(user, response);
                json_object_put(envelope);
                return 1;
            }

            offset += (size_t) chunk_length;
        }
    }

    guac_protocol_send_end(
        user->socket,
        response
    );

    guac_socket_flush(user->socket);

    guac_user_free_stream(
        user,
        response
    );

    json_object_put(envelope);

    return 0;
}


static int espkvm_http_get_blob_handler(
        guac_user* user,
        guac_stream* stream,
        void* data,
        int length) {

    espkvm_http_get_stream_data* request =
        (espkvm_http_get_stream_data*) stream->data;

    char* new_path;

    if (request == NULL || length < 0)
        return 1;

    /*
     * API paths should be tiny. Keep an explicit upper bound so malformed
     * browser input cannot grow this buffer indefinitely.
     */
    if (request->path_length + (size_t) length > 4096)
        return 1;

    new_path = realloc(
        request->path,
        request->path_length + (size_t) length + 1
    );

    if (new_path == NULL)
        return 1;

    request->path = new_path;

    memcpy(
        request->path + request->path_length,
        data,
        (size_t) length
    );

    request->path_length += (size_t) length;
    request->path[request->path_length] = '\0';

    guac_protocol_send_ack(
        user->socket,
        stream,
        "OK",
        GUAC_PROTOCOL_STATUS_SUCCESS
    );

    guac_socket_flush(user->socket);

    return 0;
}


static int espkvm_http_get_end_handler(
        guac_user* user,
        guac_stream* stream) {

    espkvm_http_get_stream_data* request =
        (espkvm_http_get_stream_data*) stream->data;

    guac_client* client = user->client;

    espkvm_client_data* client_data =
        (espkvm_client_data*) client->data;

    espkvm_response response = { NULL, 0 };

    CURLcode result;

    long http_code = 0;

    char* content_type = NULL;
    char* url = NULL;

    size_t url_length;

    int return_value = 0;

    if (request == NULL || client_data == NULL)
        return 1;

    /*
     * Only the ESPKVM API namespace is accepted by this transport.
     * The target host always comes from the Guacamole connection itself.
     */
    if (request->path == NULL
            || (strcmp(request->path, "/") != 0
                && strncmp(request->path, "/api/", 5) != 0)
            || strstr(request->path, "://") != NULL
            || strstr(request->path, "..") != NULL
            || strchr(request->path, '\r') != NULL
            || strchr(request->path, '\n') != NULL) {

        espkvm_http_send_reply(
            user,
            request->request_id,
            400,
            "text/plain",
            "Invalid ESPKVM API path."
        );

        goto cleanup;
    }

    url_length =
        strlen(client_data->base_url)
        + strlen(request->path)
        + 1;

    url = malloc(url_length);

    if (url == NULL) {
        return_value = 1;
        goto cleanup;
    }

    snprintf(
        url,
        url_length,
        "%s%s",
        client_data->base_url,
        request->path
    );

    /*
     * Explicitly return the CURL handle to GET mode.
     *
     * Authentication uses the same handle, and therefore the same in-memory
     * ESPKVM session cookie.
     */
    curl_easy_setopt(
        client_data->curl,
        CURLOPT_POST,
        0L
    );

    curl_easy_setopt(
        client_data->curl,
        CURLOPT_NOBODY,
        0L
    );

    curl_easy_setopt(
        client_data->curl,
        CURLOPT_UPLOAD,
        0L
    );

    curl_easy_setopt(
        client_data->curl,
        CURLOPT_CUSTOMREQUEST,
        NULL
    );

    curl_easy_setopt(
        client_data->curl,
        CURLOPT_POSTFIELDS,
        NULL
    );

    curl_easy_setopt(
        client_data->curl,
        CURLOPT_POSTFIELDSIZE,
        0L
    );

    curl_easy_setopt(
        client_data->curl,
        CURLOPT_HTTPHEADER,
        NULL
    );

    curl_easy_setopt(
        client_data->curl,
        CURLOPT_HTTPGET,
        1L
    );

    curl_easy_setopt(
        client_data->curl,
        CURLOPT_URL,
        url
    );

    curl_easy_setopt(
        client_data->curl,
        CURLOPT_WRITEFUNCTION,
        espkvm_write_callback
    );

    curl_easy_setopt(
        client_data->curl,
        CURLOPT_WRITEDATA,
        &response
    );

    result = curl_easy_perform(
        client_data->curl
    );

    if (result != CURLE_OK) {

        guac_client_log(
            client,
            GUAC_LOG_ERROR,
            "ESPKVM HTTP GET failed for %s: %s",
            request->path,
            curl_easy_strerror(result)
        );

        espkvm_http_send_reply(
            user,
            request->request_id,
            0,
            "text/plain",
            curl_easy_strerror(result)
        );

        goto cleanup;
    }

    curl_easy_getinfo(
        client_data->curl,
        CURLINFO_RESPONSE_CODE,
        &http_code
    );

    curl_easy_getinfo(
        client_data->curl,
        CURLINFO_CONTENT_TYPE,
        &content_type
    );

    guac_client_log(
        client,
        GUAC_LOG_DEBUG,
        "ESPKVM GET %s returned HTTP %ld.",
        request->path,
        http_code
    );

    if (espkvm_http_send_reply(
            user,
            request->request_id,
            http_code,
            content_type,
            response.data != NULL ? response.data : "") != 0) {

        return_value = 1;
    }

cleanup:

    free(url);
    free(response.data);

    free(request->path);
    free(request);

    stream->data = NULL;

    return return_value;
}


/*
 * Generic ESPKVM HTTP request transport.
 *
 * Browser pipe:
 *
 *   espkvm:http2:<request-id>
 *
 * Request body is JSON:
 *
 *   {
 *     "method": "GET|POST|PUT|DELETE",
 *     "path": "/api/...",
 *     "contentType": "application/json",
 *     "body": "..."
 *   }
 *
 * Reply:
 *
 *   espkvm:http:<request-id>:reply
 *
 * The existing reply envelope is reused.
 */
typedef struct espkvm_http_request_stream_data {

    char request_id[32];

    char* json;
    size_t json_length;

} espkvm_http_request_stream_data;


static int espkvm_http_request_blob_handler(
        guac_user* user,
        guac_stream* stream,
        void* data,
        int length) {

    espkvm_http_request_stream_data* request =
        (espkvm_http_request_stream_data*) stream->data;

    char* new_json;

    if (request == NULL || length < 0)
        return 1;

    /*
     * Text API requests are intentionally bounded.
     * Large binary uploads will use their own streaming transport later.
     */
    if (request->json_length + (size_t) length > 1024 * 1024)
        return 1;

    new_json = realloc(
        request->json,
        request->json_length + (size_t) length + 1
    );

    if (new_json == NULL)
        return 1;

    request->json = new_json;

    memcpy(
        request->json + request->json_length,
        data,
        (size_t) length
    );

    request->json_length += (size_t) length;
    request->json[request->json_length] = '\0';

    guac_protocol_send_ack(
        user->socket,
        stream,
        "OK",
        GUAC_PROTOCOL_STATUS_SUCCESS
    );

    guac_socket_flush(user->socket);

    return 0;
}


/*
 * Fetch the ESPKVM device log using a separate authenticated CURL handle.
 *
 * Temporary diagnostics for the H.264 -> MJPEG failure.
 */
static void espkvm_log_device_video_diagnostics(
        guac_client* client,
        espkvm_client_data* client_data) {

    CURL* curl = NULL;

    struct curl_slist* cookies = NULL;
    struct curl_slist* cookie = NULL;

    espkvm_response response = { NULL, 0 };

    CURLcode result;
    long http_code = 0;

    char* url = NULL;
    size_t url_length;

    int matches = 0;

    if (client == NULL || client_data == NULL)
        return;

    curl = curl_easy_init();

    if (curl == NULL)
        return;

    espkvm_prepare_curl(
        curl,
        client_data->ignore_cert
    );

    curl_easy_setopt(
        curl,
        CURLOPT_TIMEOUT,
        5L
    );

    result = curl_easy_getinfo(
        client_data->curl,
        CURLINFO_COOKIELIST,
        &cookies
    );

    if (result != CURLE_OK)
        goto cleanup;

    for (cookie = cookies;
            cookie != NULL;
            cookie = cookie->next) {

        result = curl_easy_setopt(
            curl,
            CURLOPT_COOKIELIST,
            cookie->data
        );

        if (result != CURLE_OK)
            goto cleanup;
    }

    url_length =
        strlen(client_data->base_url)
        + strlen("/api/v1/system/log")
        + 1;

    url = malloc(url_length);

    if (url == NULL)
        goto cleanup;

    snprintf(
        url,
        url_length,
        "%s/api/v1/system/log",
        client_data->base_url
    );

    curl_easy_setopt(
        curl,
        CURLOPT_URL,
        url
    );

    curl_easy_setopt(
        curl,
        CURLOPT_HTTPGET,
        1L
    );

    curl_easy_setopt(
        curl,
        CURLOPT_WRITEFUNCTION,
        espkvm_write_callback
    );

    curl_easy_setopt(
        curl,
        CURLOPT_WRITEDATA,
        &response
    );

    result = curl_easy_perform(curl);

    if (result != CURLE_OK) {

        guac_client_log(
            client,
            GUAC_LOG_WARNING,
            "ESPKVM diagnostic log fetch failed: %s",
            curl_easy_strerror(result)
        );

        goto cleanup;
    }

    curl_easy_getinfo(
        curl,
        CURLINFO_RESPONSE_CODE,
        &http_code
    );

    if (http_code != 200) {

        guac_client_log(
            client,
            GUAC_LOG_WARNING,
            "ESPKVM diagnostic log returned HTTP %ld.",
            http_code
        );

        goto cleanup;
    }

    if (response.data != NULL) {

        char* line = response.data;

        while (*line != '\0'
                && matches < 60) {

            char* newline =
                strchr(line, '\n');

            if (newline != NULL)
                *newline = '\0';

            if (strstr(line, "jpeg") != NULL
                    || strstr(line, "JPEG") != NULL
                    || strstr(line, "codec") != NULL
                    || strstr(line, "h264") != NULL
                    || strstr(line, "H.264") != NULL
                    || strstr(line, "memory") != NULL
                    || strstr(line, "PSRAM") != NULL
                    || strstr(line, "NO_MEM") != NULL
                    || strstr(line, "watchdog") != NULL
                    || strstr(line, "panic") != NULL) {

                guac_client_log(
                    client,
                    GUAC_LOG_INFO,
                    "ESPKVM DEVICE LOG: %s",
                    line
                );

                matches++;
            }

            if (newline == NULL)
                break;

            line = newline + 1;
        }
    }

cleanup:

    curl_slist_free_all(cookies);
    free(url);
    free(response.data);

    if (curl != NULL)
        curl_easy_cleanup(curl);
}


static int espkvm_http_request_end_handler(
        guac_user* user,
        guac_stream* stream) {

    espkvm_http_request_stream_data* request =
        (espkvm_http_request_stream_data*) stream->data;

    guac_client* client = user->client;

    espkvm_client_data* client_data =
        (espkvm_client_data*) client->data;

    espkvm_response response = { NULL, 0 };

    json_object* root = NULL;
    json_object* method_obj = NULL;
    json_object* path_obj = NULL;
    json_object* body_obj = NULL;
    json_object* content_type_obj = NULL;

    const char* method;
    const char* path;
    const char* body = "";
    const char* request_content_type = "";

    char* url = NULL;
    char* response_content_type = NULL;

    struct curl_slist* headers = NULL;

    CURLcode result;
    long http_code = 0;

    size_t url_length;

    int return_value = 0;
    int mutating = 0;

    if (request == NULL || client_data == NULL)
        return 1;

    if (request->json == NULL) {

        espkvm_http_send_reply(
            user,
            request->request_id,
            400,
            "text/plain",
            "Missing HTTP request."
        );

        goto cleanup;
    }

    root = json_tokener_parse(request->json);

    if (root == NULL) {

        espkvm_http_send_reply(
            user,
            request->request_id,
            400,
            "text/plain",
            "Invalid HTTP request JSON."
        );

        goto cleanup;
    }

    if (!json_object_object_get_ex(
            root,
            "method",
            &method_obj)
            || !json_object_object_get_ex(
                root,
                "path",
                &path_obj)) {

        espkvm_http_send_reply(
            user,
            request->request_id,
            400,
            "text/plain",
            "HTTP method or path missing."
        );

        goto cleanup;
    }

    method = json_object_get_string(method_obj);
    path = json_object_get_string(path_obj);

    if (method == NULL || path == NULL)
        goto bad_request;

    if (strcmp(method, "GET") != 0
            && strcmp(method, "POST") != 0
            && strcmp(method, "PUT") != 0
            && strcmp(method, "DELETE") != 0)
        goto bad_request;

    /*
     * This transport may only reach ESPKVM's own API namespace.
     * It can never become an arbitrary HTTP proxy.
     */
    if (strncmp(path, "/api/", 5) != 0
            || strstr(path, "://") != NULL
            || strstr(path, "..") != NULL
            || strchr(path, '\r') != NULL
            || strchr(path, '\n') != NULL)
        goto bad_request;

    if (json_object_object_get_ex(
            root,
            "body",
            &body_obj)
            && !json_object_is_type(
                body_obj,
                json_type_null)) {

        body = json_object_get_string(body_obj);

        if (body == NULL)
            body = "";
    }

    if (json_object_object_get_ex(
            root,
            "contentType",
            &content_type_obj)
            && !json_object_is_type(
                content_type_obj,
                json_type_null)) {

        request_content_type =
            json_object_get_string(content_type_obj);

        if (request_content_type == NULL)
            request_content_type = "";
    }

    mutating =
        strcmp(method, "POST") == 0
        || strcmp(method, "PUT") == 0
        || strcmp(method, "DELETE") == 0;

    url_length =
        strlen(client_data->base_url)
        + strlen(path)
        + 1;

    url = malloc(url_length);

    if (url == NULL) {
        return_value = 1;
        goto cleanup;
    }

    snprintf(
        url,
        url_length,
        "%s%s",
        client_data->base_url,
        path
    );

    /*
     * Clear request-specific libcurl state left by previous requests.
     */
    curl_easy_setopt(
        client_data->curl,
        CURLOPT_POST,
        0L
    );

    curl_easy_setopt(
        client_data->curl,
        CURLOPT_NOBODY,
        0L
    );

    curl_easy_setopt(
        client_data->curl,
        CURLOPT_UPLOAD,
        0L
    );

    curl_easy_setopt(
        client_data->curl,
        CURLOPT_CUSTOMREQUEST,
        NULL
    );

    curl_easy_setopt(
        client_data->curl,
        CURLOPT_POSTFIELDS,
        NULL
    );

    curl_easy_setopt(
        client_data->curl,
        CURLOPT_POSTFIELDSIZE,
        0L
    );

    curl_easy_setopt(
        client_data->curl,
        CURLOPT_HTTPHEADER,
        NULL
    );

    curl_easy_setopt(
        client_data->curl,
        CURLOPT_URL,
        url
    );

    /*
     * ESPKVM requires this anti-CSRF header on requests which change state.
     */
    if (mutating) {

        headers = curl_slist_append(
            headers,
            "X-ESP-KVM: 1"
        );

        if (headers == NULL) {
            return_value = 1;
            goto cleanup;
        }
    }

    if (request_content_type[0] != '\0') {

        char header[512];

        int written = snprintf(
            header,
            sizeof(header),
            "Content-Type: %s",
            request_content_type
        );

        if (written < 0
                || (size_t) written >= sizeof(header)) {
            goto bad_request;
        }

        headers = curl_slist_append(
            headers,
            header
        );

        if (headers == NULL) {
            return_value = 1;
            goto cleanup;
        }
    }

    if (headers != NULL) {

        curl_easy_setopt(
            client_data->curl,
            CURLOPT_HTTPHEADER,
            headers
        );
    }

    if (strcmp(method, "GET") == 0) {

        curl_easy_setopt(
            client_data->curl,
            CURLOPT_HTTPGET,
            1L
        );
    }

    else {

        /*
         * POSTFIELDS is also valid for PUT/DELETE when CUSTOMREQUEST
         * explicitly selects the method.
         */
        curl_easy_setopt(
            client_data->curl,
            CURLOPT_POSTFIELDS,
            body
        );

        curl_easy_setopt(
            client_data->curl,
            CURLOPT_POSTFIELDSIZE,
            (long) strlen(body)
        );

        if (strcmp(method, "POST") == 0) {

            curl_easy_setopt(
                client_data->curl,
                CURLOPT_POST,
                1L
            );
        }

        else {

            curl_easy_setopt(
                client_data->curl,
                CURLOPT_CUSTOMREQUEST,
                method
            );
        }
    }

    if (strcmp(method, "PUT") == 0
            && strcmp(path, "/api/v1/settings") == 0) {

        guac_client_log(
            client,
            GUAC_LOG_INFO,
            "ESPKVM SETTINGS -> PUT /api/v1/settings body=%s",
            body
        );
    }

    /*
     * Temporary diagnostics for codec/settings changes only.
     * Never log authentication requests or cookies.
     */
    if (strncmp(
            path,
            "/api/v1/video",
            strlen("/api/v1/video")) == 0) {

        guac_client_log(
            client,
            GUAC_LOG_INFO,
            "ESPKVM VIDEO HTTP -> %s %s body=%s",
            method,
            path,
            body
        );
    }

    curl_easy_setopt(
        client_data->curl,
        CURLOPT_WRITEFUNCTION,
        espkvm_write_callback
    );

    curl_easy_setopt(
        client_data->curl,
        CURLOPT_WRITEDATA,
        &response
    );

    result = curl_easy_perform(
        client_data->curl
    );

    if (result != CURLE_OK) {

        guac_client_log(
            client,
            GUAC_LOG_ERROR,
            "ESPKVM %s %s failed: %s",
            method,
            path,
            curl_easy_strerror(result)
        );

        espkvm_http_send_reply(
            user,
            request->request_id,
            0,
            "text/plain",
            curl_easy_strerror(result)
        );

        goto cleanup;
    }

    curl_easy_getinfo(
        client_data->curl,
        CURLINFO_RESPONSE_CODE,
        &http_code
    );

    curl_easy_getinfo(
        client_data->curl,
        CURLINFO_CONTENT_TYPE,
        &response_content_type
    );

    guac_client_log(
        client,
        GUAC_LOG_DEBUG,
        "ESPKVM %s %s returned HTTP %ld.",
        method,
        path,
        http_code
    );

    if (strcmp(method, "PUT") == 0
            && strcmp(path, "/api/v1/settings") == 0) {

        guac_client_log(
            client,
            GUAC_LOG_INFO,
            "ESPKVM SETTINGS <- HTTP %ld body=%s",
            http_code,
            response.data != NULL
                ? response.data
                : ""
        );
    }


    if (strcmp(
            path,
            "/api/v1/video/status") == 0) {

        espkvm_user_data* user_data =
            (espkvm_user_data*) user->data;

        const int codec_none =
            response.data != NULL
            && strstr(
                response.data,
                "\"codec\":\"none\""
            ) != NULL;

        guac_client_log(
            client,
            GUAC_LOG_INFO,
            "ESPKVM VIDEO STATUS <- HTTP %ld body=%s",
            http_code,
            response.data != NULL
                ? response.data
                : ""
        );

        if (user_data != NULL) {

            if (codec_none
                    && !user_data->video_none_diagnostics_done) {

                user_data->video_none_diagnostics_done = 1;

                guac_client_log(
                    client,
                    GUAC_LOG_WARNING,
                    "ESPKVM codec became none; capturing device diagnostics."
                );

                espkvm_log_device_video_diagnostics(
                    client,
                    client_data
                );
            }

            else if (!codec_none) {

                user_data->video_none_diagnostics_done = 0;
            }
        }
    }


    if (strcmp(
            path,
            "/api/v1/video/status") == 0) {

        guac_client_log(
            client,
            GUAC_LOG_INFO,
            "ESPKVM VIDEO STATUS <- HTTP %ld body=%s",
            http_code,
            response.data != NULL
                ? response.data
                : ""
        );
    }


    if (espkvm_http_send_reply(
            user,
            request->request_id,
            http_code,
            response_content_type,
            response.data != NULL
                ? response.data
                : "") != 0) {

        return_value = 1;
    }

    goto cleanup;

bad_request:

    espkvm_http_send_reply(
        user,
        request->request_id,
        400,
        "text/plain",
        "Invalid ESPKVM HTTP request."
    );

cleanup:

    /*
     * Never leave temporary headers/body pointers attached to the shared
     * authenticated CURL handle.
     */
    if (client_data != NULL && client_data->curl != NULL) {

        curl_easy_setopt(
            client_data->curl,
            CURLOPT_HTTPHEADER,
            NULL
        );

        curl_easy_setopt(
            client_data->curl,
            CURLOPT_POSTFIELDS,
            NULL
        );

        curl_easy_setopt(
            client_data->curl,
            CURLOPT_POSTFIELDSIZE,
            0L
        );
    }

    curl_slist_free_all(headers);

    free(url);
    free(response.data);

    if (root != NULL)
        json_object_put(root);

    free(request->json);
    free(request);

    stream->data = NULL;

    return return_value;
}

static int espkvm_pipe_handler(
        guac_user* user,
        guac_stream* stream,
        char* mimetype,
        char* name) {

    espkvm_echo_stream_data* echo;
    guac_stream* response;

    (void) mimetype;

    if (name == NULL)
        return 0;

    /*
     * ESPKVM native MJPEG /stream transport.
     *
     * Browser pipe:
     *
     *   espkvm:mjpeg-stream:<request-id>
     */
    if (strncmp(
            name,
            "espkvm:mjpeg-stream:",
            20) == 0) {

        if (espkvm_mjpeg_open(
                user,
                stream,
                name + 20) != 0) {

            guac_client_log(
                user->client,
                GUAC_LOG_WARNING,
                "ESPKVM /stream unavailable; keeping Guacamole connection alive."
            );
        }

        return 0;
    }

    /*
     * ESPKVM video WebSocket.
     *
     * Browser pipe:
     *
     *   espkvm:video-ws:<request-id>
     */
    if (strncmp(name, "espkvm:video-ws:", 16) == 0) {

        if (espkvm_video_ws_open(
                user,
                stream,
                name + 16) != 0) {

            guac_client_log(
                user->client,
                GUAC_LOG_WARNING,
                "ESPKVM /video unavailable; keeping Guacamole connection alive."
            );
        }

        return 0;
    }

    /*
     * ESPKVM keyboard/mouse/control WebSocket.
     *
     * Browser pipe:
     *
     *   espkvm:control-ws:<request-id>
     */
    if (strncmp(name, "espkvm:control-ws:", 18) == 0) {

        if (espkvm_control_ws_open(
                user,
                stream,
                name + 18) != 0) {

            guac_client_log(
                user->client,
                GUAC_LOG_WARNING,
                "ESPKVM /ws unavailable; keeping Guacamole connection alive."
            );
        }

        return 0;
    }

    /*
     * Generic ESPKVM HTTP request.
     *
     * Pipe:
     *
     *   espkvm:http2:<numeric-request-id>
     */
    if (strncmp(name, "espkvm:http2:", 13) == 0) {

        const char* request_id = name + 13;
        const char* cursor;

        espkvm_http_request_stream_data* request;

        size_t id_length = strlen(request_id);

        if (id_length == 0 || id_length >= 32)
            return 1;

        for (cursor = request_id; *cursor != '\0'; cursor++) {

            if (*cursor < '0' || *cursor > '9')
                return 1;
        }

        request = calloc(
            1,
            sizeof(espkvm_http_request_stream_data)
        );

        if (request == NULL)
            return 1;

        memcpy(
            request->request_id,
            request_id,
            id_length + 1
        );

        stream->data = request;
        stream->blob_handler =
            espkvm_http_request_blob_handler;

        stream->end_handler =
            espkvm_http_request_end_handler;

        return 0;
    }

    /*
     * Generic text-based ESPKVM HTTP GET request.
     *
     * Pipe name:
     *
     *   espkvm:http:<numeric-request-id>
     */
    if (strncmp(name, "espkvm:http:", 12) == 0) {

        const char* request_id = name + 12;
        const char* cursor;

        espkvm_http_get_stream_data* request;

        size_t id_length = strlen(request_id);

        if (id_length == 0 || id_length >= 32)
            return 1;

        /*
         * Keep request IDs deliberately simple.
         */
        for (cursor = request_id; *cursor != '\0'; cursor++) {
            if (*cursor < '0' || *cursor > '9')
                return 1;
        }

        request = calloc(
            1,
            sizeof(espkvm_http_get_stream_data)
        );

        if (request == NULL)
            return 1;

        memcpy(
            request->request_id,
            request_id,
            id_length + 1
        );

        stream->data = request;
        stream->blob_handler = espkvm_http_get_blob_handler;
        stream->end_handler = espkvm_http_get_end_handler;

        return 0;
    }

    /*
     * Real ESPKVM API round-trip test.
     */
    if (strcmp(name, "espkvm:session-test") == 0) {
        stream->end_handler = espkvm_session_test_end_handler;
        return 0;
    }

    /*
     * Temporary echo round-trip test.
     */
    if (strcmp(name, "espkvm:echo") != 0)
        return 0;

    response = guac_user_alloc_stream(user);

    if (response == NULL)
        return 1;

    echo = calloc(1, sizeof(espkvm_echo_stream_data));

    if (echo == NULL) {
        guac_user_free_stream(user, response);
        return 1;
    }

    echo->response = response;

    stream->data = echo;
    stream->blob_handler = espkvm_echo_blob_handler;
    stream->end_handler = espkvm_echo_end_handler;

    if (guac_protocol_send_pipe(
            user->socket,
            response,
            "text/plain",
            "espkvm:echo:reply") != 0) {

        stream->data = NULL;

        guac_user_free_stream(
            user,
            response
        );

        free(echo);

        return 1;
    }

    guac_socket_flush(user->socket);

    return 0;
}

static int espkvm_join_handler(
        guac_user* user,
        int argc,
        char** argv) {

    guac_client* client = user->client;
    guac_socket* socket = user->socket;

    espkvm_client_data* data;

    const char* hostname;
    const char* port;
    const char* username;
    const char* password;
    const char* ignore_cert_string;

    int ignore_cert;

    if (argc != 5) {

        guac_client_log(
            client,
            GUAC_LOG_ERROR,
            "Invalid number of ESPKVM connection arguments."
        );

        return 1;
    }

    hostname = argv[0];
    port = argv[1];
    username = argv[2];
    password = argv[3];
    ignore_cert_string = argv[4];

    ignore_cert = espkvm_is_true(ignore_cert_string);

    /*
     * Only the owner establishes the ESPKVM connection.
     * Additional Guacamole users share the same client connection.
     */
    if (user->owner && client->data == NULL) {

        data = calloc(1, sizeof(espkvm_client_data));

        if (data == NULL)
            return 1;

        data->base_url = espkvm_build_base_url(
            hostname,
            port
        );

        if (data->base_url == NULL) {

            free(data);

            guac_client_log(
                client,
                GUAC_LOG_ERROR,
                "Invalid ESPKVM hostname or port."
            );

            return 1;
        }

        data->ignore_cert = ignore_cert;

        data->curl = curl_easy_init();

        if (data->curl == NULL) {

            free(data->base_url);
            free(data);

            guac_client_log(
                client,
                GUAC_LOG_ERROR,
                "Unable to initialize libcurl."
            );

            return 1;
        }

        espkvm_prepare_curl(
            data->curl,
            ignore_cert
        );

        if (espkvm_authenticate(
                client,
                data,
                username,
                password) != 0) {

            curl_easy_cleanup(data->curl);
            free(data->base_url);
            free(data);

            return 1;
        }

        client->data = data;

        guac_client_log(
            client,
            GUAC_LOG_INFO,
            "Authenticated with ESPKVM at %s.",
            hostname
        );
    }

    if (client->data == NULL)
        return 1;

    if (user->data == NULL) {

        user->data = calloc(
            1,
            sizeof(espkvm_user_data)
        );

        if (user->data == NULL)
            return 1;
    }

    user->leave_handler =
        espkvm_user_leave_handler;

    /*
     * Accept ESPKVM-specific browser pipes for this Guacamole user.
     */
    user->pipe_handler = espkvm_pipe_handler;

    /*
     * Temporary display.
     *
     * Actual ESPKVM video will replace this in the next stage.
     */
    guac_protocol_send_size(
        socket,
        GUAC_DEFAULT_LAYER,
        1280,
        720
    );

    guac_protocol_send_rect(
        socket,
        GUAC_DEFAULT_LAYER,
        0,
        0,
        1280,
        720
    );

    guac_protocol_send_cfill(
        socket,
        GUAC_COMP_OVER,
        GUAC_DEFAULT_LAYER,
        0x10,
        0x10,
        0x10,
        0xFF
    );

    guac_protocol_send_sync(
        socket,
        client->last_sent_timestamp,
        0
    );

    guac_socket_flush(socket);

    return 0;
}

int guac_client_init(guac_client* client) {

    CURLcode result;

    result = curl_global_init(CURL_GLOBAL_ALL);

    if (result != CURLE_OK)
        return 1;

    client->args = ESPKVM_ARGS;
    client->join_handler = espkvm_join_handler;
    client->free_handler = espkvm_free_handler;

    return 0;
}
