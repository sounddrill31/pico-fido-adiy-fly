/*
 * This file is part of the Pico FIDO distribution (https://github.com/polhenarejos/pico-fido).
 * Copyright (c) 2022 Pol Henarejos.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "fido.h"
#include "hsm.h"
#include "apdu.h"
#include "files.h"
#include "random.h"
#include "version.h"
#include "asn1.h"
#include "hid/ctap_hid.h"

#define FIXED_SIZE          16
#define KEY_SIZE            16
#define UID_SIZE            6
#define KEY_SIZE_OATH       20
#define ACC_CODE_SIZE       6

#define CONFIG1_VALID       0x01
#define CONFIG2_VALID       0x02
#define CONFIG1_TOUCH       0x04
#define CONFIG2_TOUCH       0x08
#define CONFIG_LED_INV      0x10
#define CONFIG_STATUS_MASK  0x1f

/* EXT Flags */
#define SERIAL_BTN_VISIBLE  0x01    // Serial number visible at startup (button press)
#define SERIAL_USB_VISIBLE  0x02    // Serial number visible in USB iSerial field
#define SERIAL_API_VISIBLE  0x04    // Serial number visible via API call
#define USE_NUMERIC_KEYPAD  0x08    // Use numeric keypad for digits
#define FAST_TRIG           0x10    // Use fast trig if only cfg1 set
#define ALLOW_UPDATE        0x20    // Allow update of existing configuration (selected flags + access code)
#define DORMANT             0x40    // Dormant config (woken up, flag removed, requires update flag)
#define LED_INV             0x80    // LED idle state is off rather than on

/* TKT Flags */
#define TAB_FIRST       0x01    // Send TAB before first part
#define APPEND_TAB1     0x02    // Send TAB after first part
#define APPEND_TAB2     0x04    // Send TAB after second part
#define APPEND_DELAY1   0x08    // Add 0.5s delay after first part
#define APPEND_DELAY2   0x10    // Add 0.5s delay after second part
#define APPEND_CR       0x20    // Append CR as final character
#define OATH_HOTP       0x40    // OATH HOTP mode
#define CHAL_RESP       0x40    // Challenge-response enabled (both must be set)
#define PROTECT_CFG2    0x80    // Block update of config 2 unless config 2 is configured and has this bit set

/* CFG Flags */
#define SEND_REF            0x01    // Send reference string (0..F) before data
#define PACING_10MS         0x04    // Add 10ms intra-key pacing
#define PACING_20MS         0x08    // Add 20ms intra-key pacing
#define STATIC_TICKET       0x20    // Static ticket generation
// Static
#define SHORT_TICKET        0x02    // Send truncated ticket (half length)
#define STRONG_PW1          0x10    // Strong password policy flag #1 (mixed case)
#define STRONG_PW2          0x40    // Strong password policy flag #2 (subtitute 0..7 to digits)
#define MAN_UPDATE          0x80    // Allow manual (local) update of static OTP
// Challenge (no keyboard)
#define HMAC_LT64           0x04    // Set when HMAC message is less than 64 bytes
#define CHAL_BTN_TRIG       0x08    // Challenge-response operation requires button press
#define CHAL_YUBICO         0x20    // Challenge-response enabled - Yubico OTP mode
#define CHAL_HMAC           0x22    // Challenge-response enabled - HMAC-SHA1
// OATH
#define OATH_HOTP8          0x02    // Generate 8 digits HOTP rather than 6 digits
#define OATH_FIXED_MODHEX1  0x10    // First byte in fixed part sent as modhex
#define OATH_FIXED_MODHEX2  0x40    // First two bytes in fixed part sent as modhex
#define OATH_FIXED_MODHEX   0x50    // Fixed part sent as modhex
#define OATH_FIXED_MASK     0x50    // Mask to get out fixed flags

static uint8_t config_seq = { 1 };

typedef struct otp_config {
    uint8_t fixed_data[FIXED_SIZE];
    uint8_t uid[UID_SIZE];
    uint8_t aes_key[KEY_SIZE];
    uint8_t acc_code[ACC_CODE_SIZE];
    uint8_t fixed_size;
    uint8_t ext_flags;
    uint8_t tkt_flags;
    uint8_t cfg_flags;
    uint8_t rfu[2];
    uint16_t crc;
} __attribute__((packed)) otp_config_t;

static const size_t otp_config_size = sizeof(otp_config_t);
uint16_t otp_status();

int otp_process_apdu();
int otp_unload();

const uint8_t otp_aid[] = {
    7,
    0xa0, 0x00, 0x00, 0x05, 0x27, 0x20, 0x01
};

app_t *otp_select(app_t *a, const uint8_t *aid, uint8_t aid_len) {
    if (!memcmp(aid, otp_aid + 1, MIN(aid_len, otp_aid[0]))) {
        a->aid = otp_aid;
        a->process_apdu = otp_process_apdu;
        a->unload = otp_unload;
        if (file_has_data(search_dynamic_file(EF_OTP_SLOT1)) ||
            file_has_data(search_dynamic_file(EF_OTP_SLOT2))) {
            config_seq = 1;
        }
        else {
            config_seq = 0;
        }
        otp_status();
        apdu.ne = res_APDU_size;
        return a;
    }
    return NULL;
}

extern int calculate_oath(uint8_t truncate,
                          const uint8_t *key,
                          size_t key_len,
                          const uint8_t *chal,
                          size_t chal_len);
int otp_button_pressed(uint8_t slot) {
#ifndef ENABLE_EMULATION
    printf("CB PRESSED slot %d\n", slot);
    file_t *ef = search_dynamic_file(slot == 1 ? EF_OTP_SLOT1 : EF_OTP_SLOT2);
    const uint8_t *data = file_get_data(ef);
    otp_config_t *otp_config = (otp_config_t *)data;
    if (otp_config->tkt_flags & OATH_HOTP) {
        uint8_t tmp_key[KEY_SIZE + 2];
        tmp_key[0] = 0x01;
        memcpy(tmp_key + 2, otp_config->aes_key, KEY_SIZE);
        uint64_t imf = 0;
        if (file_get_size(ef) == otp_config_size) {
            imf = ((otp_config->uid[4] << 8) | otp_config->uid[5]) << 4;
        }
        else {
            const uint8_t *p = data + otp_config_size;
            imf |= (uint64_t)*p++ << 56;
            imf |= (uint64_t)*p++ << 48;
            imf |= (uint64_t)*p++ << 40;
            imf |= (uint64_t)*p++ << 32;
            imf |= *p++ << 24;
            imf |= *p++ << 16;
            imf |= *p++ << 8;
            imf |= *p++;
        }
        uint8_t chal[8] = {imf >> 56, imf >> 48, imf >> 40, imf >> 32, imf >> 24, imf >> 16, imf >> 8, imf & 0xff};
        res_APDU_size = 0;
        int ret = calculate_oath(1, tmp_key, sizeof(tmp_key), chal, sizeof(chal));
        if (ret == CCID_OK) {
            uint32_t base = otp_config->cfg_flags & OATH_HOTP8 ? 1e8 : 1e6;
            uint32_t number = (res_APDU[2] << 24) | (res_APDU[3] << 16) | (res_APDU[4] << 8) | res_APDU[5];
            number %= base;
            char number_str[9];
            if (otp_config->cfg_flags & OATH_HOTP8) {
                sprintf(number_str, "%08lu", (long unsigned int)number);
                add_keyboard_buffer((const uint8_t *)number_str, 8, true);
            }
            else {
                sprintf(number_str, "%06lu", (long unsigned int)number);
                add_keyboard_buffer((const uint8_t *)number_str, 6, true);
            }
            imf++;
            uint8_t new_chal[8] = {imf >> 56, imf >> 48, imf >> 40, imf >> 32, imf >> 24, imf >> 16, imf >> 8, imf & 0xff};
            uint8_t new_otp_config[otp_config_size + sizeof(new_chal)];
            memcpy(new_otp_config, otp_config, otp_config_size);
            memcpy(new_otp_config + otp_config_size, new_chal, sizeof(new_chal));
            flash_write_data_to_file(ef, new_otp_config, sizeof(new_otp_config));
            low_flash_available();
        }
        if (otp_config->tkt_flags & APPEND_CR) {
            append_keyboard_buffer((const uint8_t *)"\r", 1);
        }
    }
    else if (otp_config->cfg_flags & SHORT_TICKET || otp_config->cfg_flags & STATIC_TICKET) {
        if (otp_config->cfg_flags & SHORT_TICKET) {
            otp_config->fixed_size /= 2;
        }
        add_keyboard_buffer(otp_config->fixed_data, otp_config->fixed_size, false);
        if (otp_config->tkt_flags & APPEND_CR) {
            append_keyboard_buffer((const uint8_t *)"\x28", 1);
        }
    }
    else {

    }
#endif
    return 0;
}

void __attribute__((constructor)) otp_ctor() {
    register_app(otp_select);
    button_pressed_cb = otp_button_pressed;
}

int otp_unload() {
    return CCID_OK;
}

uint16_t otp_status() {
    res_APDU_size = 0;
    res_APDU[1] = PICO_FIDO_VERSION_MAJOR;
    res_APDU[2] = PICO_FIDO_VERSION_MINOR;
    res_APDU[3] = 0;
    res_APDU[4] = config_seq;
    res_APDU[5] = (CONFIG2_TOUCH | CONFIG1_TOUCH) |
                                (file_has_data(search_dynamic_file(EF_OTP_SLOT1)) ? CONFIG1_VALID :
                                 0x00) |
                                (file_has_data(search_dynamic_file(EF_OTP_SLOT2)) ? CONFIG2_VALID :
                                 0x00);
    res_APDU[6] = 0;
    return SW_OK();
}

int cmd_otp() {
    uint8_t p1 = P1(apdu), p2 = P2(apdu);
    if (p2 != 0x00) {
        return SW_INCORRECT_P1P2();
    }
    if (p1 == 0x01 || p1 == 0x03) { // Configure slot
        /*if (apdu.nc != otp_config_size + ACC_CODE_SIZE) {
            return SW_WRONG_LENGTH();
        }*/
        otp_config_t *odata = (otp_config_t *)apdu.data;
        if (odata->rfu[0] != 0 || odata->rfu[1] != 0) {
            return SW_WRONG_DATA();
        }
        file_t *ef = file_new(p1 == 0x01 ? EF_OTP_SLOT1 : EF_OTP_SLOT2);
        printf("has data %d\n",file_has_data(ef));
        if (file_has_data(ef)) {
            otp_config_t *otpc = (otp_config_t *) file_get_data(ef);
            if (memcmp(otpc->acc_code, apdu.data + otp_config_size, ACC_CODE_SIZE) != 0) {
                return SW_SECURITY_STATUS_NOT_SATISFIED();
            }
        }
        for (int c = 0; c < otp_config_size; c++) {
            if (apdu.data[c] != 0) {
                flash_write_data_to_file(ef, apdu.data, otp_config_size);
                low_flash_available();
                config_seq++;
                return otp_status();
            }
        }
        // Delete slot
        delete_file(ef);
        if (!file_has_data(search_dynamic_file(EF_OTP_SLOT1)) &&
            !file_has_data(search_dynamic_file(EF_OTP_SLOT2))) {
            config_seq = 0;
        }
        return otp_status();
    }
    else if (p1 == 0x10) {
#ifndef ENABLE_EMULATION
        pico_get_unique_board_id_string((char *) res_APDU, 4);
#endif
        res_APDU_size = 4;
    }
    return SW_OK();
}

#define INS_OTP             0x01
#define INS_DELETE          0x02
#define INS_SET_CODE        0x03
#define INS_RESET           0x04
#define INS_LIST            0xa1
#define INS_CALCULATE       0xa2
#define INS_VALIDATE        0xa3
#define INS_CALC_ALL        0xa4
#define INS_SEND_REMAINING  0xa5

static const cmd_t cmds[] = {
    { INS_OTP, cmd_otp },
    { 0x00, 0x0 }
};

int otp_process_apdu() {
    if (CLA(apdu) != 0x00) {
        return SW_CLA_NOT_SUPPORTED();
    }
    for (const cmd_t *cmd = cmds; cmd->ins != 0x00; cmd++) {
        if (cmd->ins == INS(apdu)) {
            int r = cmd->cmd_handler();
            return r;
        }
    }
    return SW_INS_NOT_SUPPORTED();
}
