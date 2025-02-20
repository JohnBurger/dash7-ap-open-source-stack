/* * OSS-7 - An opensource implementation of the DASH7 Alliance Protocol for ultra
 * lowpower wireless sensor communication
 *
 * Copyright 2019 CORTUS, Aloxy
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * @author philippe.nunes@cortus.com
 * @author liam.oorts@aloxy.io
 * @author glenn.ergeerts@aloxy.io
 */

#include <stdlib.h>
#include "stdbool.h"
#include "string.h"
#include "types.h"
#include "math.h"

#include "debug.h"
#include "log.h"
#include "scheduler.h"
#include "timer.h"

#include "hwradio.h"
#include "hwdebug.h"
#include "phy.h"

#include "crc.h"
#include "pn9.h"
#include "fec.h"

#include "packet_queue.h"
#include "MODULE_D7AP_defs.h"
#include "d7ap_fs.h"

#if defined(FRAMEWORK_LOG_ENABLED) && defined(MODULE_D7AP_PHY_LOG_ENABLED)
#define DPRINT(...) log_print_stack_string(LOG_STACK_PHY, __VA_ARGS__)
#define DPRINT_DATA(...) log_print_data(__VA_ARGS__)
#else
#define DPRINT(...)
#define DPRINT_PACKET(...)
#define DPRINT_DATA(...)
#endif

#if PLATFORM_NUM_DEBUGPINS >= 2
    #define DEBUG_TX_START() hw_debug_set(0);
    #define DEBUG_TX_END() hw_debug_clr(0);
    #define DEBUG_RX_START() hw_debug_set(1);
    #define DEBUG_RX_END() hw_debug_clr(1);
#else
    #define DEBUG_TX_START()
    #define DEBUG_TX_END()
    #define DEBUG_RX_START()
    #define DEBUG_RX_END()
#endif

// modulation settings

// low rate
#define BITRATE_L 9600 // bps
#define FDEV_L    4800 // Hz
// Carson's rule: 2 x fm + 2 x fd  = 9.600 + 2 x 4.800 = 19.2 kHz
// assuming 1 ppm crystals gives max error of: 2 * 1 ppm * 868 = 1.736 kHz
// => BW > 19.2 + 1.736 kHz => > 20.936 kHZ. 
// This results in 10.468 kHz on a single sideband.
// #define RXBW_L    10468 //Hz

// normal rate
#define BITRATE_N 55555 // bps
#define FDEV_N    50000 // Hz
// Carson's rule: 2 x fm + 2 x fd  = 55.555 + 2 x 50 = 155.555 kHz
// assuming 1 ppm crystals gives max error of: 2 * 1 ppm * 868 = 1.736 kHz
// => BW > 155.555 + 1.736 => 157.291 kHz. 
// This results in 78.646 kHz on a single sideband.
// #define RXBW_N   78646 //Hz

// high rate
#define BITRATE_H 166667 // bps
#define FDEV_H     41667 // Hz
// Carson's rule: 2 x fm + 2 x fd  = 166.667 + 2 x 41.667 = 250 kHz
// assuming 1 ppm crystals gives max error of: 2 * 1 ppm * 868 = 1.736 kHz
// => BW > 250 + 1.736 kHz => 251.736 kHz.
// This results in 125.868 kHz on a single sideband.
// #define RXBW_H    125868 //Hz

#define LORA_T_SYMBOL_SF9_MS 4.096 // based on SF9 and 125k BW
#define LORA_T_PREAMBE_SF9_MS (8 + 4.25) * LORA_T_SYMBOL_SF9_MS // assuming 8 symbols for now


typedef enum {
  STATE_IDLE,
  STATE_TX,
  STATE_RX,
  STATE_BG_SCAN,
  STATE_CONT_TX,
  STATE_CONT_RX
} state_t;

static hwradio_init_args_t init_args;

static tx_packet_callback_t transmitted_callback;
static rx_packet_callback_t received_callback;

static state_t state = STATE_IDLE;
static hw_radio_packet_t *current_packet;
static bool should_rx_after_tx_completed = false;
static syncword_class_t current_syncword_class = PHY_SYNCWORD_CLASS0;
static phy_rx_config_t pending_rx_cfg;

static channel_id_t default_channel_id = {
  .channel_header.ch_coding = PHY_CODING_PN9,
  .channel_header.ch_class = PHY_CLASS_NORMAL_RATE,
  .channel_header.ch_freq_band = PHY_BAND_868,
  .center_freq_index = 0
};

#define EMPTY_CHANNEL_ID { .channel_header_raw = 0xFF, .center_freq_index = 0xFF }

static channel_id_t current_channel_id = EMPTY_CHANNEL_ID;

static uint32_t rx_bw_lo_rate;
static uint32_t rx_bw_normal_rate;
static uint32_t rx_bw_hi_rate;
static bool rx_bw_changed = false;

static uint8_t gain_offset = 0;

/*
 * FSK packet handler structure
 */
typedef struct
{
    uint8_t Size;
    uint8_t NbBytes;
    uint8_t FifoThresh;
}FskPacketHandler_t;

FskPacketHandler_t FskPacketHandler;

/*
 * Background advertising packet handler structure
 */
typedef struct
{
    uint8_t dll_header[BACKGROUND_DLL_HEADER_LENGTH];
    uint8_t packet[24];  // 6 bytes preamble (PREAMBLE_HI_RATE_CLASS) + 2 bytes SYNC word + 16 bytes max for a background frame FEC encoded
    uint8_t *packet_payload;
    uint8_t packet_size;
    uint16_t eta;
    uint16_t tx_duration;
    timer_tick_t stop_time;
}bg_adv_t;

bg_adv_t bg_adv;

typedef struct
{
    uint16_t encoded_length;
    uint8_t encoded_packet[PREAMBLE_HI_RATE_CLASS + 2 + (PACKET_MAX_SIZE + 1)*2]; // include space for preamble and syncword
    uint16_t transmitted_index;
    bool bg_adv;
}fg_frame_t;

fg_frame_t fg_frame;

const uint16_t sync_word_value[2][4] = {
    { 0xE6D0, 0x0000, 0xF498, 0xE6D0 },
    { 0x0B67, 0x0000, 0x192F, 0x0B67 }
};

/*
 * In background scan, the device has a period of To to successfully detect the sync word
 * To is at least equal to the duration of one background frame plus the duration
 * of the maximum preamble length for the used channel class.
 */
#define To_CLASS_LO_RATE 22 // (12(FEC encode payload) + 2 (SYNC) + 8 (Max preamble)) / 1 byte/tick
#define To_CLASS_NORMAL_RATE 4 // (12(FEC encode payload) + 2 (SYNC) + 8 (Max preamble)) / 6 bytes/tick
#define To_CLASS_HI_RATE 2 // (12(FEC encode payload) + 2 (SYNC) + 16 (Max preamble)) / 20 bytes/tick

const uint8_t bg_timeout[4] = {
    To_CLASS_LO_RATE,
    0, // RFU
    To_CLASS_NORMAL_RATE,
    To_CLASS_HI_RATE
};

uint16_t end_time;
/*!
 * D7A timer used to expire the continuous TX
 */
static timer_event continuous_tx_expiration_timer;

static void fill_in_fifo(uint8_t remaining_bytes_len);

static hw_radio_packet_t* alloc_new_packet(uint16_t length)
{
    // note we don't use length because in the current implementation the packets in the queue are of
    // fixed (maximum) size
    return &(packet_queue_alloc_packet()->hw_radio_packet);
}

static void release_packet(hw_radio_packet_t* hw_radio_packet)
{
    packet_queue_free_packet(packet_queue_find_packet(hw_radio_packet));
}

void phy_switch_to_standby_mode()
{
    hw_radio_set_opmode(HW_STATE_STANDBY);
    state = STATE_IDLE;
}

void phy_switch_to_sleep_mode()
{
    hw_radio_set_idle();
    state = STATE_IDLE;
}

static void packet_transmitted(timer_tick_t timestamp)
{
    assert(state == STATE_TX || state == STATE_CONT_TX);

    current_packet->tx_meta.timestamp = timestamp;
    DPRINT("Transmitted packet @ %i with length = %i", current_packet->tx_meta.timestamp, current_packet->length);

    phy_switch_to_standby_mode();

    transmitted_callback(packet_queue_find_packet(current_packet));
}

static void packet_received(hw_radio_packet_t* hw_radio_packet)
{
    assert(state == STATE_RX || state == STATE_BG_SCAN);
    // we are in interrupt context here, so mark packet for further processing,
    // schedule it and return
    DPRINT("packet received @ %i , RSSI = %d", hw_radio_packet->rx_meta.timestamp, hw_radio_packet->rx_meta.rssi);

    packet_t* packet = packet_queue_find_packet(hw_radio_packet);

    DPRINT("Rx packet before decoding <len = %d>", hw_radio_packet->length);
    DPRINT_DATA(hw_radio_packet->data, hw_radio_packet->length);

#ifndef HAL_RADIO_USE_HW_DC_FREE
    pn9_encode(hw_radio_packet->data, hw_radio_packet->length);

    DPRINT("Rx packet after pn9 encoding <len = %d>", hw_radio_packet->length);
    DPRINT_DATA(hw_radio_packet->data, hw_radio_packet->length);
#endif
#ifndef HAL_RADIO_USE_HW_FEC
    if (current_channel_id.channel_header.ch_coding == PHY_CODING_FEC_PN9)
        fec_decode_packet(hw_radio_packet->data, hw_radio_packet->length, hw_radio_packet->length);
#endif

    if (current_syncword_class == PHY_SYNCWORD_CLASS0)
    {
        packet->type = BACKGROUND_ADV;
        hw_radio_packet->length = BACKGROUND_FRAME_LENGTH;
    }
    else
        hw_radio_packet->length = hw_radio_packet->data[0] + 1;

    DPRINT("RX packet fully decoded <len = %d>", hw_radio_packet->length);
    DPRINT_DATA(hw_radio_packet->data, hw_radio_packet->length);

    packet->phy_config.rx.syncword_class = current_syncword_class;
    memcpy(&(packet->phy_config.rx.channel_id), &current_channel_id, sizeof(channel_id_t));

    if (state == STATE_RX)
    {
        // Restart the reception until upper layer decides to stop it
        hw_radio_set_opmode(HW_STATE_RX);
    }
    else
        phy_switch_to_standby_mode();

    received_callback(packet);
}

static void packet_header_received(uint8_t *data, uint8_t len)
{
    uint16_t packet_len;
    DPRINT("Packet Header received %i\n", len);
    DPRINT_DATA(data, len);

    assert(len == 4);

#ifndef HAL_RADIO_USE_HW_DC_FREE
    pn9_encode(data, len);
#endif

    if (current_channel_id.channel_header.ch_coding == PHY_CODING_FEC_PN9)
    {
#ifndef HAL_RADIO_USE_HW_FEC
        fec_decode_packet(data, len, len);
#endif
        DPRINT("RX packet header after decoding");
        DPRINT_DATA(data, len);

        packet_len = fec_calculated_decoded_length(data[0] + 1);
    }
    else
        packet_len = data[0] + 1 ;

    DPRINT("RX Packet Length: %i ", packet_len);
    // set PayloadLength to the length of the expected foreground frame
    hw_radio_set_payload_length(packet_len);
}

bool phy_radio_channel_ids_equal(const channel_id_t* a, const channel_id_t* b)
{
    //return memcmp(a,b, sizeof(channel_id_t)) == 0; //not working since channel_id_t not packed
    return (a->channel_header_raw == b->channel_header_raw) && (a->center_freq_index == b->center_freq_index);
}

uint16_t phy_calculate_tx_duration(phy_channel_class_t channel_class, phy_coding_t ch_coding, uint8_t packet_length, bool payload_only)
{
    double data_rate = 6.0; // Normal rate: 6.9 bytes/tick

    if (ch_coding == PHY_CODING_FEC_PN9)
        packet_length = fec_calculated_decoded_length(packet_length);

    if(!payload_only)
      packet_length += sizeof(uint16_t); // Sync word

#ifdef USE_SX127X
    if(channel_class == PHY_CLASS_LORA) {
        // based on http://www.semtech.com/images/datasheet/LoraDesignGuide_STD.pdf
        // only valid for explicit header, CR4/5, SF9 for now
        uint16_t payload_symbols = 8 + ceil(2*(packet_length+1)/9)*5;
        uint16_t packet_duration = LORA_T_PREAMBE_SF9_MS + payload_symbols * LORA_T_SYMBOL_SF9_MS;
        return packet_duration;
    }
#endif

    switch (channel_class)
    {
    case PHY_CLASS_LO_RATE:
        if(!payload_only)
          packet_length += PREAMBLE_LOW_RATE_CLASS;

        data_rate = 1.0; // Lo Rate 9.6 kbps: 1.2 bytes/tick
        break;
    case PHY_CLASS_NORMAL_RATE:
        if(!payload_only)
          packet_length += PREAMBLE_NORMAL_RATE_CLASS;

        data_rate = 6.0; // Normal Rate 55.555 kbps: 6.94 bytes/tick
        break;
    case PHY_CLASS_HI_RATE:
        if(!payload_only)
          packet_length += PREAMBLE_HI_RATE_CLASS;

        data_rate = 20.0; // High rate 166.667 kbps: 20.83 byte/tick
        break;
    }

    // TODO Add the power ramp-up/ramp-down symbols in the packet length?

    return ceil(packet_length / data_rate) + 1;
}

static void configure_eirp(eirp_t eirp)
{
    eirp -= gain_offset;
    DPRINT("Set Tx power: %d dBm including offset of %i\n", eirp, gain_offset);

    hw_radio_set_tx_power(eirp);
}

static void configure_channel(const channel_id_t* channel) {
    if(phy_radio_channel_ids_equal(&current_channel_id, channel) && !rx_bw_changed) {
        return;
    }

    rx_bw_changed = false;

    // configure modulation settings
    if(channel->channel_header.ch_class == PHY_CLASS_LO_RATE)
    {
        hw_radio_set_bitrate(BITRATE_L);
        if(channel->channel_header.ch_coding != PHY_CODING_CW)
            hw_radio_set_tx_fdev(FDEV_L);
        else
            hw_radio_set_tx_fdev(0);
        hw_radio_set_rx_bw_hz(rx_bw_lo_rate);
        hw_radio_set_preamble_size(PREAMBLE_LOW_RATE_CLASS * 8);
    }
    else if(channel->channel_header.ch_class == PHY_CLASS_NORMAL_RATE)
    {
        hw_radio_set_bitrate(BITRATE_N);
        if(channel->channel_header.ch_coding != PHY_CODING_CW)
            hw_radio_set_tx_fdev(FDEV_N);
        else
            hw_radio_set_tx_fdev(0);
        hw_radio_set_rx_bw_hz(rx_bw_normal_rate);
        hw_radio_set_preamble_size(PREAMBLE_NORMAL_RATE_CLASS * 8);
    }
    else if(channel->channel_header.ch_class == PHY_CLASS_HI_RATE)
    {
        hw_radio_set_bitrate(BITRATE_H);
        if(channel->channel_header.ch_coding != PHY_CODING_CW)
            hw_radio_set_tx_fdev(FDEV_H);
        else
            hw_radio_set_tx_fdev(0);
        hw_radio_set_rx_bw_hz(rx_bw_hi_rate);
        hw_radio_set_preamble_size(PREAMBLE_HI_RATE_CLASS * 8);
    }

    // TODO regopmode for LF?

    uint32_t center_freq = 433.06e6;
    if(channel->channel_header.ch_freq_band == PHY_BAND_868)
        center_freq = 863e6;
    else if(channel->channel_header.ch_freq_band == PHY_BAND_915)
        center_freq = 902e6;

    uint32_t channel_spacing_half = 100e3;
    if(channel->channel_header.ch_class == PHY_CLASS_LO_RATE)
        channel_spacing_half = 12500;

    center_freq += 25000 * channel->center_freq_index + channel_spacing_half;
    hw_radio_set_center_freq(center_freq);

    current_channel_id = *channel;
    DPRINT("set channel_header %i, channel_band %i, center_freq_index %i\n",
           current_channel_id.channel_header_raw,
           current_channel_id.channel_header.ch_freq_band,
           current_channel_id.center_freq_index);
}

static void configure_syncword(syncword_class_t syncword_class, const channel_id_t* channel)
{
    current_syncword_class = syncword_class;
    uint16_t sync_word = sync_word_value[syncword_class][channel->channel_header.ch_coding ];

    DPRINT("sync_word = %04x", sync_word);
    hw_radio_set_sync_word((uint8_t *)&sync_word, sizeof(uint16_t));
}

void continuous_tx_expiration()
{
    hw_radio_enable_refill(false);
    DPRINT("Continuous TX is now terminated");
}

void fact_settings_file_change_callback()
{
    uint8_t fact_settings[D7A_FILE_FACTORY_SETTINGS_SIZE];
    d7ap_fs_read_file(D7A_FILE_FACTORY_SETTINGS_FILE_ID, 0, fact_settings, D7A_FILE_FACTORY_SETTINGS_SIZE);

    gain_offset = (int8_t)fact_settings[0];
    rx_bw_lo_rate = __builtin_bswap32(*((uint32_t*)(fact_settings+1)));
    rx_bw_normal_rate = __builtin_bswap32(*((uint32_t*)(fact_settings+5)));
    rx_bw_hi_rate = __builtin_bswap32(*((uint32_t*)(fact_settings+9)));

    DPRINT("rx bw low rate is %i, normal rate is %i, high rate is %i\n", rx_bw_lo_rate, rx_bw_normal_rate, rx_bw_hi_rate);
    DPRINT("gain offset set to %i\n", gain_offset);

    rx_bw_changed = true;
}


error_t phy_init(void) {

    error_t ret = SUCCESS;

    init_args.alloc_packet_cb = alloc_new_packet;
    init_args.release_packet_cb = release_packet;
    init_args.rx_packet_cb = packet_received;
    init_args.tx_packet_cb = packet_transmitted;
    init_args.rx_packet_header_cb = packet_header_received;
    init_args.tx_refill_cb = fill_in_fifo;

    hw_radio_init(&init_args);

#ifdef HAL_RADIO_USE_HW_CRC
    hw_radio_set_crc_on(true);
#else
    hw_radio_set_crc_on(false);
#endif

#ifdef HAL_RADIO_USE_HW_DC_FREE
    hw_radio_set_dc_free(HW_DC_FREE_WHITENING);
#else
    hw_radio_set_dc_free(HW_DC_FREE_NONE);
#endif

    fact_settings_file_change_callback();

    fs_register_file_modified_callback(D7A_FILE_FACTORY_SETTINGS_FILE_ID, &fact_settings_file_change_callback);

    configure_syncword(PHY_SYNCWORD_CLASS0, &default_channel_id);
    configure_channel(&default_channel_id);
    configure_eirp(10);

    //netdev->event_callback = _event_cb;
    //sched_register_task(&isr_handler, netdev);

    //hw_radio_set_opmode(OPMODE_STANDBY); --> done by the netdev driver
    //while(hw_radio_get_opmode() != OPMODE_STANDBY) {}

    timer_init_event(&continuous_tx_expiration_timer, &continuous_tx_expiration);

    return ret;
}

error_t phy_start_rx(channel_id_t* channel, syncword_class_t syncword_class, rx_packet_callback_t rx_cb) {
    received_callback = rx_cb;
    // TODO error handling EINVAL, EOFF

    // if we are currently transmitting wait until TX completed before entering RX
    // we return now and go into RX when TX is completed
    if(state == STATE_TX)
    {
        should_rx_after_tx_completed = true;
        pending_rx_cfg.channel_id = *channel;
        pending_rx_cfg.syncword_class = syncword_class;
        return SUCCESS;
    }

    configure_channel(channel);
    configure_syncword(syncword_class, channel);

    // Unlimited Length packet format to set the Receive packet of arbitrary length
    hw_radio_set_payload_length(0x00); // unlimited length mode

    DPRINT("START FG scan @ %i", timer_get_counter_value());
    DEBUG_RX_START();

    state = STATE_RX;
    hw_radio_set_opmode(HW_STATE_RX);

    return SUCCESS;
}

error_t phy_start_energy_scan(channel_id_t* channel, rssi_valid_callback_t rssi_cb, int16_t scan_duration)
{
    // We should not initiate a RSSI measurement before TX is completed
    assert(state != STATE_TX);

    configure_channel(channel);
    //configure_syncword(syncword_class, channel);
    hw_radio_set_payload_length(0x00); // unlimited length mode

    // switch to RX since the RSSI measurement is done in RX mode
    state = STATE_RX;

    //FIXME support asynchronous RSSI and scan duration
    //uint8_t rssi_samples = scan_duration
    //hw_radio_set_rssi_smoothing(rssi_samples);

    int16_t rssi = hw_radio_get_rssi();
    rssi_cb(rssi);

    return SUCCESS;
}

error_t phy_stop_rx(){

    phy_switch_to_sleep_mode();
    return SUCCESS;
}

static uint16_t encode_packet(hw_radio_packet_t* packet, uint8_t* encoded_packet)
{
    uint16_t encoded_len = packet->length;
    memcpy(encoded_packet, packet->data, packet->length);

#ifndef HAL_RADIO_USE_HW_FEC
    if (current_channel_id.channel_header.ch_coding == PHY_CODING_FEC_PN9)
        encoded_len = fec_encode(encoded_packet, packet->length);
#endif

    DPRINT("AFTER FEC ENCODING TX len=%d", encoded_len);
    DPRINT_DATA(encoded_packet, encoded_len);

#ifndef HAL_RADIO_USE_HW_DC_FREE
    pn9_encode(encoded_packet, encoded_len);
#endif

    return encoded_len;
}

error_t phy_send_packet(hw_radio_packet_t* packet, phy_tx_config_t* config, tx_packet_callback_t tx_callback)
{
    assert(packet->length <= PACKET_MAX_SIZE);

    transmitted_callback = tx_callback;

    if(packet->length == 0)
        return ESIZE;

    current_packet = packet;

    if(state == STATE_RX)
    {
        pending_rx_cfg.channel_id = current_channel_id;
        pending_rx_cfg.syncword_class = current_syncword_class;
        should_rx_after_tx_completed = true;
        phy_switch_to_standby_mode();
    }

    configure_channel(&config->channel_id);
    configure_eirp(config->eirp);
    configure_syncword(config->syncword_class, &config->channel_id);

    state = STATE_TX;

    DPRINT("BEFORE ENCODING TX len=%i", packet->length);
    DPRINT_DATA(packet->data, packet->length);

    // Encode the packet if not supported by xcvr
    // uint8_t encoded_packet[(PACKET_MAX_SIZE + 1)*2]; // bufer sized for FEC encoding
    fg_frame.encoded_length = encode_packet(packet, fg_frame.encoded_packet);

    DPRINT("AFTER ENCODING TX len=%i\n", fg_frame.encoded_length);
    DPRINT_DATA(fg_frame.encoded_packet, fg_frame.encoded_length);

    DEBUG_RX_END();
    DEBUG_TX_START();

    DPRINT("start sending @ %i\n", timer_get_counter_value());

    hw_radio_send_payload(fg_frame.encoded_packet, fg_frame.encoded_length);

    return SUCCESS; // TODO other return codes
}

static uint8_t assemble_background_payload()
{
    uint16_t crc, swap_eta;
    uint8_t payload_len;

    /*
     * Build the next advertising frame.
     * In order to flood the channel with advertising frames without discontinuity,
     * the FIFO is refilled with the next frames within the same TX.
     * For that, the preamble and the sync word are explicitly inserted before each
     * subsequent advertising frame.
     */

    memcpy(bg_adv.packet_payload, bg_adv.dll_header, BACKGROUND_DLL_HEADER_LENGTH);

    // add ETA for background frames
    //DPRINT("eta %i", bg_adv.eta);
    swap_eta = __builtin_bswap16(bg_adv.eta);
    memcpy(&bg_adv.packet_payload[BACKGROUND_DLL_HEADER_LENGTH], &swap_eta, sizeof(uint16_t));

    // add CRC
    crc = __builtin_bswap16(crc_calculate(bg_adv.packet_payload, 4));
    memcpy(&bg_adv.packet_payload[BACKGROUND_DLL_HEADER_LENGTH + sizeof(uint16_t)], &crc, 2);

    if (current_channel_id.channel_header.ch_coding == PHY_CODING_FEC_PN9)
    {
        payload_len = fec_encode(bg_adv.packet_payload, BACKGROUND_FRAME_LENGTH);
        pn9_encode(bg_adv.packet_payload, payload_len);
    }
    else
    {
        //DPRINT("assemble payload %d", BACKGROUND_FRAME_LENGTH);
        //DPRINT_DATA(bg_adv.packet_payload, BACKGROUND_FRAME_LENGTH);
        pn9_encode(bg_adv.packet_payload, BACKGROUND_FRAME_LENGTH);
        payload_len = BACKGROUND_FRAME_LENGTH;
    }

    return payload_len;
}

/** \brief Send a packet using background advertising
 *
 * Start a background frame flooding until expiration of the advertising period, followed by transmission
 * of the foreground frame.
 * Each background frame contains the Estimated Time of Arrival of the D7ANP Request (ETA).
 * When no more advertising background frames can be fully transmitted before the start of D7ANP,
 * the last background frame is extended by padding preamble symbols after the end of the background
 * packet, in order to guarantee no silence period on the channel between D7AAdvP and D7ANP.
 */
error_t phy_send_packet_with_advertising(hw_radio_packet_t* packet, phy_tx_config_t* config,
                                         uint8_t dll_header_bg_frame[2], uint16_t eta, tx_packet_callback_t tx_callback)
{   
    transmitted_callback = tx_callback;
    DPRINT("Start the bg advertising for ad-hoc sync before transmitting the FG frame");

    configure_syncword(PHY_SYNCWORD_CLASS0, &config->channel_id);
    configure_channel(&config->channel_id);
    configure_eirp(config->eirp);

    current_packet = packet;

    // During the advertising flooding, use the infinite packet length mode
    hw_radio_set_payload_length(0x00); // unlimited length mode

    hw_radio_enable_refill(true);
    hw_radio_enable_preloading(true);

    // Prepare the subsequent background frames which include the preamble and the sync word
    uint8_t preamble_len = (current_channel_id.channel_header.ch_class ==  PHY_CLASS_HI_RATE ? PREAMBLE_HI_RATE_CLASS : PREAMBLE_LOW_RATE_CLASS);
    memset(bg_adv.packet, 0xAA, preamble_len); // preamble length is given in number of bytes
    uint16_t sync_word = __builtin_bswap16(sync_word_value[PHY_SYNCWORD_CLASS0][current_channel_id.channel_header.ch_coding]);
    memcpy(&bg_adv.packet[preamble_len], &sync_word, 2);

    if (current_channel_id.channel_header.ch_coding == PHY_CODING_FEC_PN9)
        bg_adv.packet_size = preamble_len + 2 + fec_calculated_decoded_length(BACKGROUND_FRAME_LENGTH);
    else
        bg_adv.packet_size = preamble_len + 2 + BACKGROUND_FRAME_LENGTH;

    bg_adv.packet_payload = bg_adv.packet + preamble_len + 2 ;

    // Backup the DLL header
    memcpy(bg_adv.dll_header, dll_header_bg_frame, BACKGROUND_DLL_HEADER_LENGTH);
    DPRINT("DLL header followed by ETA %i", eta);
    DPRINT_DATA(bg_adv.dll_header, BACKGROUND_DLL_HEADER_LENGTH);

    bg_adv.eta = eta;
    bg_adv.tx_duration = phy_calculate_tx_duration(current_channel_id.channel_header.ch_class,
                                                   current_channel_id.channel_header.ch_coding,
                                                   bg_adv.packet_size, false);

    // prepare the foreground frame, so we can transmit this immediately
    DPRINT("Original payload with ETA %i", eta);
    DPRINT_DATA(packet->data, packet->length);

    DPRINT("tx_duration_bg_frame %i", bg_adv.tx_duration);
    fg_frame.bg_adv = true;
    memset(fg_frame.encoded_packet, 0xAA, preamble_len);
    sync_word = __builtin_bswap16(sync_word_value[PHY_SYNCWORD_CLASS1][current_channel_id.channel_header.ch_coding]);
    memcpy(&fg_frame.encoded_packet[preamble_len], &sync_word, 2);
    fg_frame.encoded_length = encode_packet(packet, &fg_frame.encoded_packet[preamble_len + 2]);
    fg_frame.encoded_length += preamble_len + 2; // add preamble + syncword

    uint8_t payload_len;
    payload_len = assemble_background_payload();

    // For the first advertising frame, transmit directly the payload since the preamble and the sync word are directly managed by the xcv
    DPRINT("Transmit packet: %d", payload_len);
    DPRINT_DATA(bg_adv.packet_payload, payload_len);

    hw_radio_send_payload(bg_adv.packet_payload, payload_len); // in preloading mode

    // prepare the next advertising frame, insert the preamble and the SYNC word
    bg_adv.eta -= bg_adv.tx_duration; // the next ETA is the time remaining after the end transmission time of the D7AAdvP frame
    assemble_background_payload();

    // start Tx
    timer_tick_t start = timer_get_counter_value();
    bg_adv.stop_time = start + eta + bg_adv.tx_duration; // Tadv = Tsched + Ttx
    DPRINT("BG Tadv %i (start time @ %i stop time @ %i)", eta + bg_adv.tx_duration, start, bg_adv.stop_time);

    state = STATE_TX;
    DEBUG_RX_END();
    DEBUG_TX_START();
    hw_radio_set_opmode(HW_STATE_TX);

    return SUCCESS;
}


static void fill_in_fifo(uint8_t remaining_bytes_len)
{
    // To update the ETA, we take into account the number of remaining bytes still to be transmitted
    // Also, at the time we calculate ETA here we will only insert the previously crafted packet (containing the prev ETA) in the
    // FIFO. So we will need to take this into account as well.

    // TODO adapt how we calculate ETA. There is no reason to use current time for each ETA update, we can just use the BG frame duration
    // and a frame counter to determine this.

    if (fg_frame.bg_adv)
    {
        timer_tick_t current = timer_get_counter_value();
        // DPRINT("fill in fifo, bg adv, currently %d untill %d\n", current, bg_adv.stop_time);

        // calculate the time needed to flush the remaining bytes in the TX
        uint16_t flush_duration = phy_calculate_tx_duration(current_channel_id.channel_header.ch_class,
                                                            PHY_CODING_PN9, // override FEC, we need the time for the BG_THRESHOLD bytes in the fifo, regardless of coding
                                                            remaining_bytes_len, true); // don't take syncword and preamble into account

        if (bg_adv.stop_time > current + 2 * bg_adv.tx_duration + flush_duration)
            bg_adv.eta = (bg_adv.stop_time - current) - 2 * bg_adv.tx_duration - flush_duration; // ETA is updated according the real current time
        else
            //TODO avoid stop time being elapsed
            bg_adv.eta = 0;

        DPRINT("ts after tx %d, new ETA %d\n", current + bg_adv.tx_duration, bg_adv.eta);

        /*
         * When no more advertising background frames can be fully transmitted before
         * the start of D7ANP, the last background frame is extended by padding preamble
         * symbols after the end of the background packet, in order to guarantee no silence period.
         * The FIFO level allows to write enough padding preamble bytes without overflow
         */
        if(bg_adv.eta)
        {
            // Fill up the TX FIFO with the full packet including the preamble and the SYNC word
            hw_radio_send_payload(bg_adv.packet, bg_adv.packet_size);

            // Prepare the next frame
            assemble_background_payload();
        }
        else
        {
            // not enough time for sending another BG frame, send the FG frame,
            // prepend with preamble bytes if necessary
            uint16_t preamble_len = 0;
            uint8_t preamble[bg_adv.packet_size];

            preamble_len = (bg_adv.stop_time - current) * (bg_adv.packet_size / (float)bg_adv.tx_duration); // TODO instead of current we should use the timestamp
            DPRINT("ETA %d, packet size %d, tx_duration %d, current time %d\n", bg_adv.eta, bg_adv.packet_size, bg_adv.tx_duration, timer_get_counter_value());

            DPRINT("Add preamble_bytes: %d\n", preamble_len);
            memset(preamble, 0xAA, preamble_len);
            hw_radio_send_payload(preamble, preamble_len);

            bg_adv.eta = 0;
            fg_frame.bg_adv = false;
        }
    }
    else
    {
        // Disable the refill event since this is the last chunk of data to transmit
        if (state != STATE_CONT_TX) 
            hw_radio_enable_refill(false);
        hw_radio_send_payload(fg_frame.encoded_packet, fg_frame.encoded_length);
    }
}

error_t phy_start_background_scan(phy_rx_config_t* config, rx_packet_callback_t rx_cb)
{
    received_callback = rx_cb;
    uint8_t packet_len;

    //DPRINT("START BG scan @ %i", timer_get_counter_value());

    // We should not initiate a background scan before TX is completed
    assert(state != STATE_TX);

    state = STATE_BG_SCAN;

    configure_syncword(PHY_SYNCWORD_CLASS0, &config->channel_id);
    configure_channel(&config->channel_id);

    if (current_channel_id.channel_header.ch_coding == PHY_CODING_FEC_PN9)
        packet_len = fec_calculated_decoded_length(BACKGROUND_FRAME_LENGTH);
    else
        packet_len = BACKGROUND_FRAME_LENGTH;

    // set PayloadLength to the length of the expected Background frame (fixed length packet format is used)
    hw_radio_set_payload_length(packet_len);

    DEBUG_RX_START();

    int16_t rssi = hw_radio_get_rssi();
    if (rssi <= config->rssi_thr)
    {
        DPRINT("FAST RX termination RSSI %i below limit %i\n", rssi, config->rssi_thr);
        phy_switch_to_sleep_mode();
        // TODO choose standby mode to allow rapid channel cycling
        //phy_switch_to_standby_mode();
        DEBUG_RX_END();
        return FAIL;
    }

    DPRINT("rssi %i, waiting for BG frame\n", rssi);

    // the device has a period of To to successfully detect the sync word
    hw_radio_set_rx_timeout(bg_timeout[current_channel_id.channel_header.ch_class] + 40); //TO DO: OPTIMISE THIS TIMEOUT
    hw_radio_set_opmode(HW_STATE_RX);

    return SUCCESS;
}

void phy_continuous_tx(phy_tx_config_t const* tx_cfg, uint8_t time_period, tx_packet_callback_t tx_cb)
{
    transmitted_callback = tx_cb;
    DPRINT("Continuous tx\n");

    if(state == STATE_RX)
    {
        pending_rx_cfg.channel_id = current_channel_id;
        pending_rx_cfg.syncword_class = current_syncword_class;
        should_rx_after_tx_completed = true;
        phy_switch_to_standby_mode();
    }

    configure_channel(&tx_cfg->channel_id);
    configure_eirp(tx_cfg->eirp);
    configure_syncword(tx_cfg->syncword_class, &tx_cfg->channel_id);
    hw_radio_enable_refill(true);

    state = STATE_CONT_TX;
    if(time_period) {
        continuous_tx_expiration_timer.next_event = time_period * 1024;
        timer_add_event(&continuous_tx_expiration_timer);
    }

    fg_frame.bg_adv = false;
    if (current_channel_id.channel_header.ch_coding == PHY_CODING_FEC_PN9)
    {
        uint8_t payload_len = 32;
        fg_frame.encoded_packet[0] = payload_len;
        for (uint8_t i = 0; i < payload_len; i++)
            fg_frame.encoded_packet[i+1] = i;

        fg_frame.encoded_length = fec_encode(fg_frame.encoded_packet, payload_len);
        pn9_encode(fg_frame.encoded_packet, fg_frame.encoded_length);
    }
    else if (current_channel_id.channel_header.ch_coding == PHY_CODING_PN9)
    {
        uint8_t payload_len = 63;
        fg_frame.encoded_packet[0] = payload_len;
        for (uint8_t i = 0; i < payload_len; i++)
            fg_frame.encoded_packet[i+1] = 0xAA;

        pn9_encode(fg_frame.encoded_packet, payload_len);
        fg_frame.encoded_length = payload_len;
    } else {
        uint8_t payload_len = 0xFF;
        fg_frame.encoded_packet[0] = payload_len;
        for (uint8_t i = 1; i < payload_len; i++)
            fg_frame.encoded_packet[i+1] = i;
        fg_frame.encoded_length = payload_len;
    }
    hw_radio_send_payload(fg_frame.encoded_packet, fg_frame.encoded_length);
}
