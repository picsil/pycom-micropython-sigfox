/*
 * Copyright (c) 2020, Pycom Limited.
 *
 * This software is licensed under the GNU GPL version 3 or any
 * later version, with permitted additional terms. For more information
 * see the Pycom Licence v1.0 document supplied with this file, or
 * available at https://www.pycom.io/opensource/licensing
 */

#include <stdint.h>
#include <string.h>

#include "py/mpconfig.h"
#include "py/obj.h"
#include "py/mphal.h"
#include "readline.h"
#include "telnet.h"
#include "serverstask.h"

#include "esp_heap_caps.h"
#include "sdkconfig.h"
#include "esp_system.h"
#include "esp_spi_flash.h"
#include "nvs_flash.h"

#include "modnetwork.h"
//#include "modwlan.h"
#include "modusocket.h"
//#include "debug.h"
#include "utils/interrupt_char.h"
#include "genhdr/mpversion.h"

#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/******************************************************************************
 DEFINE PRIVATE CONSTANTS
 ******************************************************************************/
#define TELNET_PORT                         23
// rxRindex and rxWindex must be uint8_t and TELNET_RX_BUFFER_SIZE == 256
#define TELNET_RX_BUFFER_SIZE               256
#define TELNET_MAX_CLIENTS                  1
#define TELNET_TX_RETRIES_MAX               50
#define TELNET_WAIT_TIME_MS                 2
#define TELNET_LOGIN_RETRIES_MAX            3
#define TELNET_CYCLE_TIME_MS                (SERVERS_CYCLE_TIME_MS * 2)

#define SE 240
#define AYT 246
#define IAC 255
#define SB 250
#define WILL 251
#define WONT 252
#define DO 253
#define DONT 254
#define TRANSMIT_BINARY 0
#define ECHO 1
#define SUPPRESS_GO_AHEAD 3
#define LINEMODE 34
#define MODE 1
#define EDIT 1

/******************************************************************************
 DEFINE PRIVATE TYPES
 ******************************************************************************/
typedef enum {
    E_TELNET_RESULT_OK = 0,
    E_TELNET_RESULT_AGAIN,
    E_TELNET_RESULT_FAILED
} telnet_result_t;

typedef enum {
    E_TELNET_STE_DISABLED = 0,
    E_TELNET_STE_START,
    E_TELNET_STE_LISTEN,
    E_TELNET_STE_CONNECTED,
    E_TELNET_STE_LOGGED_IN
} telnet_state_t;

typedef enum {
    E_TELNET_STE_SUB_WELCOME,
    E_TELNET_STE_SUB_SND_USER_OPTIONS,
    E_TELNET_STE_SUB_REQ_USER,
    E_TELNET_STE_SUB_GET_USER,
    E_TELNET_STE_SUB_REQ_PASSWORD,
    E_TELNET_STE_SUB_SND_PASSWORD_OPTIONS,
    E_TELNET_STE_SUB_GET_PASSWORD,
    E_TELNET_STE_SUB_INVALID_LOGIN,
    E_TELNET_STE_SUB_SND_REPL_OPTIONS,
    E_TELNET_STE_SUB_LOGIN_SUCCESS
} telnet_connected_substate_t;

typedef union {
    telnet_connected_substate_t connected;
} telnet_substate_t;

typedef struct {
    uint8_t             *rxBuffer;
    uint32_t            timeout;
    telnet_state_t      state;
    telnet_substate_t   substate;
    int32_t             sd;
    int32_t             n_sd;

    // rxRindex and rxWindex must be uint8_t and TELNET_RX_BUFFER_SIZE == 256
    uint8_t             rxWindex;
    uint8_t             rxRindex;

    // used to store incoming chars in cases the reception needs to be
    // completed later
    uint8_t             rxIncompleteLen;

    uint8_t             txRetries;
    uint8_t             loginRetries;
    bool                enabled;
    bool                credentialsValid;
    bool                binary_mode;
} telnet_data_t;

/******************************************************************************
 DECLARE PRIVATE DATA
 ******************************************************************************/
static telnet_data_t telnet_data;
static const char* telnet_welcome_msg       = "MicroPython " MICROPY_GIT_TAG " on " MICROPY_BUILD_DATE "; " MICROPY_HW_BOARD_NAME " with " MICROPY_HW_MCU_NAME "\r\n";
static const char* telnet_request_user      = "Login as: ";
static const char* telnet_request_password  = "Password: ";
static const char* telnet_invalid_login    = "\r\nInvalid credentials, try again.\r\n";
static const char* telnet_login_success    = "\r\nLogin succeeded!\r\nType \"help()\" for more information.\r\n";
static const uint8_t telnet_options_user[]  = { IAC, WONT, ECHO, IAC, WONT, SUPPRESS_GO_AHEAD, IAC, WILL, LINEMODE };
static const uint8_t telnet_options_pass[]  = { IAC, WILL, ECHO, IAC, WONT, SUPPRESS_GO_AHEAD, IAC, WILL, LINEMODE };
static const uint8_t telnet_options_repl[]  = { IAC, WILL, ECHO, IAC, WILL, SUPPRESS_GO_AHEAD, IAC, WONT, LINEMODE };

/******************************************************************************
 DECLARE PRIVATE FUNCTIONS
 ******************************************************************************/
static void telnet_wait_for_enabled (void);
static bool telnet_create_socket (void);
static void telnet_wait_for_connection (void);
static void telnet_send_and_proceed (void *data, int32_t Len, telnet_connected_substate_t next_state);
static telnet_result_t telnet_send_non_blocking (void *data, int32_t Len);
static telnet_result_t telnet_recv_text_non_blocking (void *buff, int32_t Maxlen, int32_t *rxLen);
static void telnet_process (void);
static int telnet_process_credential (char *credential, int32_t rxLen);
static void telnet_parse_input (uint8_t *str, int32_t *len);
static bool telnet_send_with_retries (int32_t sd, const void *pBuf, int32_t len);
static void telnet_reset_buffer (void);

/******************************************************************************
 DEFINE PUBLIC FUNCTIONS
 ******************************************************************************/
void telnet_init (void) {
    // allocate memory for the receive buffer (from the RTOS heap)
    telnet_data.rxBuffer = malloc(TELNET_RX_BUFFER_SIZE);
    telnet_data.state = E_TELNET_STE_DISABLED;
}

void telnet_run (void) {
    int32_t rxLen;
    switch (telnet_data.state) {
        case E_TELNET_STE_DISABLED:
            telnet_wait_for_enabled();
            break;
        case E_TELNET_STE_START:
            if (/*wlan_is_connected() && */ telnet_create_socket()) {
                telnet_data.state = E_TELNET_STE_LISTEN;
            }
            break;
        case E_TELNET_STE_LISTEN:
            telnet_wait_for_connection();
            break;
        case E_TELNET_STE_CONNECTED:
            switch (telnet_data.substate.connected) {
            case E_TELNET_STE_SUB_WELCOME:
                telnet_send_and_proceed((void *)telnet_welcome_msg, strlen(telnet_welcome_msg), E_TELNET_STE_SUB_SND_USER_OPTIONS);
                break;
            case E_TELNET_STE_SUB_SND_USER_OPTIONS:
                telnet_send_and_proceed((void *)telnet_options_user, sizeof(telnet_options_user), E_TELNET_STE_SUB_REQ_USER);
                break;
            case E_TELNET_STE_SUB_REQ_USER:
                // to catch any left over characters from the previous actions
                telnet_recv_text_non_blocking(telnet_data.rxBuffer, TELNET_RX_BUFFER_SIZE, &rxLen);
                telnet_send_and_proceed((void *)telnet_request_user, strlen(telnet_request_user), E_TELNET_STE_SUB_GET_USER);
                break;
            case E_TELNET_STE_SUB_GET_USER:
                if (E_TELNET_RESULT_OK == telnet_recv_text_non_blocking(telnet_data.rxBuffer + telnet_data.rxWindex,
                                                                        TELNET_RX_BUFFER_SIZE - telnet_data.rxWindex,
                                                                        &rxLen)) {
                    int result;
                    if ((result = telnet_process_credential (servers_user, rxLen))) {
                        telnet_data.credentialsValid = result > 0 ? true : false;
                        telnet_data.substate.connected = E_TELNET_STE_SUB_REQ_PASSWORD;
                    }
                }
                break;
            case E_TELNET_STE_SUB_REQ_PASSWORD:
                telnet_send_and_proceed((void *)telnet_request_password, strlen(telnet_request_password), E_TELNET_STE_SUB_SND_PASSWORD_OPTIONS);
                break;
            case E_TELNET_STE_SUB_SND_PASSWORD_OPTIONS:
                // to catch any left over characters from the previous actions
                telnet_recv_text_non_blocking(telnet_data.rxBuffer, TELNET_RX_BUFFER_SIZE, &rxLen);
                telnet_send_and_proceed((void *)telnet_options_pass, sizeof(telnet_options_pass), E_TELNET_STE_SUB_GET_PASSWORD);
                break;
            case E_TELNET_STE_SUB_GET_PASSWORD:
                if (E_TELNET_RESULT_OK == telnet_recv_text_non_blocking(telnet_data.rxBuffer + telnet_data.rxWindex,
                                                                        TELNET_RX_BUFFER_SIZE - telnet_data.rxWindex,
                                                                        &rxLen)) {
                    int result;
                    if ((result = telnet_process_credential (servers_pass, rxLen))) {
                        if ((telnet_data.credentialsValid = telnet_data.credentialsValid && (result > 0 ? true : false))) {
                            telnet_data.substate.connected = E_TELNET_STE_SUB_SND_REPL_OPTIONS;
                        }
                        else {
                            telnet_data.substate.connected = E_TELNET_STE_SUB_INVALID_LOGIN;
                        }
                    }
                }
                break;
            case E_TELNET_STE_SUB_INVALID_LOGIN:
                if (E_TELNET_RESULT_OK == telnet_send_non_blocking((void *)telnet_invalid_login, strlen(telnet_invalid_login))) {
                    telnet_data.credentialsValid = true;
                    if (++telnet_data.loginRetries >= TELNET_LOGIN_RETRIES_MAX) {
                        telnet_reset();
                    }
                    else {
                        telnet_data.substate.connected = E_TELNET_STE_SUB_SND_USER_OPTIONS;
                    }
                }
                break;
            case E_TELNET_STE_SUB_SND_REPL_OPTIONS:
                telnet_send_and_proceed((void *)telnet_options_repl, sizeof(telnet_options_repl), E_TELNET_STE_SUB_LOGIN_SUCCESS);
                break;
            case E_TELNET_STE_SUB_LOGIN_SUCCESS:
                if (E_TELNET_RESULT_OK == telnet_send_non_blocking((void *)telnet_login_success, strlen(telnet_login_success))) {
                    // clear the current line and force the prompt
                    telnet_reset_buffer();
                    telnet_data.state= E_TELNET_STE_LOGGED_IN;
                }
            default:
                break;
            }
            break;
        case E_TELNET_STE_LOGGED_IN:
            telnet_process();
            break;
        default:
            break;
    }

    if (telnet_data.state >= E_TELNET_STE_CONNECTED) {
        if (telnet_data.timeout++ > (servers_get_timeout() / TELNET_CYCLE_TIME_MS)) {
            telnet_reset();
        }
    }
}

void telnet_tx_strn (const char *str, int len) {
    if (telnet_data.n_sd > 0 && telnet_data.state == E_TELNET_STE_LOGGED_IN && len > 0) {
        telnet_send_with_retries(telnet_data.n_sd, str, len);
    }
}

bool telnet_rx_any (void) {
    return (telnet_data.n_sd > 0) ? (telnet_data.rxRindex != telnet_data.rxWindex &&
            telnet_data.state == E_TELNET_STE_LOGGED_IN) : false;
}

int telnet_rx_char (void) {
    int rx_char = -1;
    if (telnet_data.rxRindex != telnet_data.rxWindex) {
        // rxRindex must be uint8_t and TELNET_RX_BUFFER_SIZE == 256 so that it wraps around automatically
        rx_char = (int)telnet_data.rxBuffer[telnet_data.rxRindex++];
    }
    return rx_char;
}

void telnet_enable (void) {
    telnet_data.enabled = true;
}

void telnet_disable (void) {
    telnet_reset();
    telnet_data.enabled = false;
    telnet_data.state = E_TELNET_STE_DISABLED;
}

void telnet_reset (void) {
    // close the connection and start all over again
    servers_close_socket(&telnet_data.n_sd);
    servers_close_socket(&telnet_data.sd);
    telnet_data.state = E_TELNET_STE_START;
}

/******************************************************************************
 DEFINE PRIVATE FUNCTIONS
 ******************************************************************************/
static void telnet_wait_for_enabled (void) {
    // Init telnet's data
    telnet_data.n_sd = -1;
    telnet_data.sd   = -1;

    // Check if the telnet service has been enabled
    if (telnet_data.enabled) {
        telnet_data.state = E_TELNET_STE_START;
    }
}

static bool telnet_create_socket (void) {
    struct sockaddr_in sServerAddress;
    int32_t result;

    // open a socket for telnet
    telnet_data.sd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (telnet_data.sd > 0) {
        // add the socket to the network administration
        modusocket_socket_add(telnet_data.sd, false);

        // enable non-blocking mode
        uint32_t option = fcntl(telnet_data.sd, F_GETFL, 0);
        option |= O_NONBLOCK;
        fcntl(telnet_data.sd, F_SETFL, option);

        // enable address reusing
        option = 1;
        result = setsockopt(telnet_data.sd, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option));

        // bind the socket to a port number
        sServerAddress.sin_family = AF_INET;
        sServerAddress.sin_addr.s_addr = INADDR_ANY;
        sServerAddress.sin_len = sizeof(sServerAddress);
        sServerAddress.sin_port = htons(TELNET_PORT);

        result = bind(telnet_data.sd, (const struct sockaddr *)&sServerAddress, sizeof(sServerAddress));

        // start listening
        result |= listen (telnet_data.sd, TELNET_MAX_CLIENTS - 1);

        if (!result) {
            return true;
        }
        servers_close_socket(&telnet_data.sd);
    }

    return false;
}

static void telnet_wait_for_connection (void) {
    socklen_t  in_addrSize;
    struct sockaddr_in  sClientAddress;

    // accepts a connection from a TCP client, if there is any, otherwise returns EAGAIN
    telnet_data.n_sd = accept(telnet_data.sd, (struct sockaddr *)&sClientAddress, (socklen_t *)&in_addrSize);
    if (telnet_data.n_sd < 0 && errno == EAGAIN) {
        return;
    } else {
        if (telnet_data.n_sd <= 0) {
            // error
            telnet_reset();
            return;
        }

        // close the listening socket, we don't need it anymore
        servers_close_socket(&telnet_data.sd);

        // add the new socket to the network administration
        modusocket_socket_add(telnet_data.n_sd, false);

        // enable non-blocking mode
        uint32_t option = fcntl(telnet_data.n_sd, F_GETFL, 0);
        option |= O_NONBLOCK;
        fcntl(telnet_data.n_sd, F_SETFL, option);

        // client connected, so go on
        telnet_data.rxWindex = 0;
        telnet_data.rxRindex = 0;
        telnet_data.txRetries = 0;
        telnet_data.rxIncompleteLen = 0;

        telnet_data.state = E_TELNET_STE_CONNECTED;
        telnet_data.substate.connected = E_TELNET_STE_SUB_WELCOME;
        telnet_data.credentialsValid = true;
        telnet_data.loginRetries = 0;
        telnet_data.timeout = 0;
        telnet_data.binary_mode = false;
    }
}

static void telnet_send_and_proceed (void *data, int32_t Len, telnet_connected_substate_t next_state) {
    if (E_TELNET_RESULT_OK == telnet_send_non_blocking(data, Len)) {
        telnet_data.substate.connected = next_state;
    }
}

static telnet_result_t telnet_send_non_blocking (void *data, int32_t Len) {
    if (send(telnet_data.n_sd, data, Len, 0) > 0) {
        telnet_data.txRetries = 0;
        return E_TELNET_RESULT_OK;
    } else if ((TELNET_TX_RETRIES_MAX >= ++telnet_data.txRetries) && (errno == EAGAIN)) {
        return E_TELNET_RESULT_AGAIN;
    } else {
        // error
        telnet_reset();
        return E_TELNET_RESULT_FAILED;
    }
}

static telnet_result_t telnet_recv_text_non_blocking (void *buff, int32_t Maxlen, int32_t *rxLen) {
    *rxLen = recv(telnet_data.n_sd, buff, Maxlen, 0);
    // if there's data received, parse it
    if (*rxLen > 0) {
        telnet_data.timeout = 0;
        telnet_parse_input (buff, rxLen);
        if (*rxLen > 0) {
            return E_TELNET_RESULT_OK;
        }
    } else if (errno != EAGAIN) {
        // error
        telnet_reset();
        return E_TELNET_RESULT_FAILED;
    }
    return E_TELNET_RESULT_AGAIN;
}

static void telnet_process (void) {
    int32_t rxLen;
    int32_t maxLen = (telnet_data.rxWindex >= telnet_data.rxRindex) ? (TELNET_RX_BUFFER_SIZE - telnet_data.rxWindex) :
                                                                   ((telnet_data.rxRindex - telnet_data.rxWindex) - 1);
    // to avoid an overrrun
    maxLen = (telnet_data.rxRindex == 0) ? (maxLen - 1) : maxLen;

    if (maxLen > 0) {
        if (E_TELNET_RESULT_OK == telnet_recv_text_non_blocking(&telnet_data.rxBuffer[telnet_data.rxWindex], maxLen, &rxLen)) {
            // rxWindex must be uint8_t and TELNET_RX_BUFFER_SIZE == 256 so that it wraps around automatically
            telnet_data.rxWindex = telnet_data.rxWindex + rxLen;
        }
    }
}

static int telnet_process_credential (char *credential, int32_t rxLen) {
    telnet_data.rxWindex += rxLen;
    if (telnet_data.rxWindex >= SERVERS_USER_PASS_LEN_MAX) {
        telnet_data.rxWindex = SERVERS_USER_PASS_LEN_MAX;
    }

    uint8_t *p = telnet_data.rxBuffer + SERVERS_USER_PASS_LEN_MAX;
    // if a '\r' is found, or the length exceeds the max username length
    if ((p = memchr(telnet_data.rxBuffer, '\r', telnet_data.rxWindex)) || (telnet_data.rxWindex >= SERVERS_USER_PASS_LEN_MAX)) {
        uint8_t len = p - telnet_data.rxBuffer;

        telnet_data.rxWindex = 0;
        if ((len > 0) && (memcmp(credential, telnet_data.rxBuffer, MAX(len, strlen(credential))) == 0)) {
            return 1;
        }
        return -1;
    }
    return 0;
}

static uint8_t telnet_get_reply_verb(uint8_t verb) {
    if (verb < DO) {
        // translate a will into do and a won't into don't
        return verb + (DO - WILL);
    } else {
        // if not, translate a do into will and don't into won't
        return verb - (DO - WILL);
    }
}

static int telnet_process_IAC (uint8_t **strR, uint8_t **strW, int32_t *len, uint32_t remaining) {
    if (remaining >= 2)  {
        switch (*((*strR) + 1)) {
            case IAC:
                // double IAC char (0xFF) means escaped 0xFF
                **strW = 0xFF;
                (*strW)++;
                (*strR) += 2;
                (*len)--;
                return 0;

            case AYT:
                // reply to the AYT with an echo of the IAC AYT
                telnet_tx_strn((char *) *strR, 2);
                (*strR) += 2;
                (*len) -= 2;
                return 0;
        }
    }

    if (remaining >= 3) {
        if (*((*strR) + 2) == TRANSMIT_BINARY) {
            uint8_t option = *((*strR) + 1);
            if (option == WILL) telnet_data.binary_mode = true;
            if (option == WONT) telnet_data.binary_mode = false;
            *((*strR) + 1) = telnet_get_reply_verb(option);
            telnet_tx_strn((char *) *strR, 3);
        }
        (*strR) += 3;
        (*len) -= 3;
        return 0;
    } else {
        // not enough characters to continue
        *len -= remaining;
        return remaining;
    }
    return 0;
}

static void telnet_parse_input (uint8_t *str, int32_t *len) {
    int32_t b_len = *len;
    uint8_t *b_str = str - telnet_data.rxIncompleteLen;

    *len += telnet_data.rxIncompleteLen;
    b_len = *len;

    for (uint8_t *_str = b_str; _str < b_str + b_len; ) {
        uint8_t ch = *_str;
        if (ch == IAC) {
            uint32_t remaining = b_len - (_str - b_str);
            telnet_data.rxIncompleteLen = telnet_process_IAC(&_str, &str, len, remaining);
            if (telnet_data.rxIncompleteLen > 0) {
                break;
            }
            continue;
        }

        if (telnet_data.binary_mode == true) {
            *str++ = *_str++;
            continue;
        }

        // in this case the server is not operating in binary mode
        if (ch > 127 || ch == 0 || (telnet_data.state == E_TELNET_STE_LOGGED_IN &&
            (ch == mp_interrupt_char || ch == CHAR_CTRL_F))) {
            if (ch == mp_interrupt_char) {
                mp_keyboard_interrupt();
            } else if (ch == CHAR_CTRL_F) {
                *str++ = CHAR_CTRL_D;
                mp_hal_reset_safe_and_boot(false);
                _str++;
                continue;
            }
            // skip this char
            (*len)--;
            _str++;
        } else {
            *str++ = *_str++;
        }
    }
}

static bool telnet_send_with_retries (int32_t sd, const void *pBuf, int32_t len) {
    int32_t retries = 0;
    uint32_t delay = TELNET_WAIT_TIME_MS;

    do {
        // make it blocking
        uint32_t option = fcntl(sd, F_GETFL, 0);
        option &= ~O_NONBLOCK;
        fcntl(sd, F_SETFL, option);
        if (send(sd, pBuf, len, 0) > 0) {
            // make it non-blocking again
            option |= O_NONBLOCK;
            fcntl(sd, F_SETFL, option);
            return true;
        } else if (EAGAIN != errno) {
            // make it non-blocking again
            option |= O_NONBLOCK;
            fcntl(sd, F_SETFL, option);
            return false;
        }
        // start with the default delay and increment it on each retry
        mp_hal_delay_ms(delay++);
    } while (++retries <= TELNET_TX_RETRIES_MAX);

    return false;
}

static void telnet_reset_buffer (void) {
    // erase any characters present in the current line
    memset (telnet_data.rxBuffer, '\b', TELNET_RX_BUFFER_SIZE / 2);
    telnet_data.rxWindex = TELNET_RX_BUFFER_SIZE / 2;
    // fake an "enter" key pressed to display the prompt
    telnet_data.rxBuffer[telnet_data.rxWindex++] = '\r';
}

