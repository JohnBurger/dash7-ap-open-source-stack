/*
 *  Created on: Nov 22, 2012
 *  Authors:
 * 		maarten.weyn@artesis.be
 *  	glenn.ergeerts@artesis.be
 *  	alexanderhoet@gmail.com
 */

#include <msp430.h>
#include <stdbool.h>
#include <stdint.h>

#include "../phy.h"
#include "cc430_phy.h"
#include "cc430_registers.h"

#include "rf1a.h"

/*
 * Variables
 */
extern RF_SETTINGS rfSettings;
extern InterruptHandler interrupt_table[34];

RadioState state;

uint8_t buffer[255];
uint8_t packetLength;
uint8_t remainingBytes;
uint8_t* bufferPosition;

uint8_t lqi;
int16_t rssi;

bool fec;
uint8_t channel_center_freq_index;
uint8_t channel_bandwidth_index;
uint8_t preamble_size;
uint16_t sync_word;

bool packetReceived;

/*
 * Phy implementation functions
 */
void phy_init(void)
{
	//Set radio state
	state = Idle;

	//Reset the radio core
	ResetRadioCore();

	//Write configuration
	WriteRfSettings(&rfSettings);
	WriteSinglePATable(PATABLE_VAL);
}

bool phy_tx(phy_tx_cfg* cfg)
{
	if(get_radiostate() != Idle)
		return false;

	//Set radio state
	state = Transmit;

	//Go to idle and flush the txfifo
	Strobe(RF_SIDLE);
	Strobe(RF_SFTX);

	//Set configuration
	if(!phy_translate_settings(cfg->spectrum_id, cfg->sync_word_class, &fec, &channel_center_freq_index, &channel_bandwidth_index, &preamble_size, &sync_word))
		return false;

	set_length_infinite(false);
	set_data_whitening(true);
	set_channel(channel_center_freq_index, channel_bandwidth_index);
	set_preamble_size(preamble_size);
	set_sync_word(sync_word);

	//Set buffer position
	bufferPosition = cfg->data;

	//Set packet length
	remainingBytes = cfg->length;
	WriteSingleReg(PKTLEN, cfg->length);

	//Write initial data to txfifo
	tx_data_isr();

	//Configure txfifo threshold
	WriteSingleReg(FIFOTHR, RADIO_FIFOTHR_FIFO_THR_17_48);

	//Enable interrupts
	RF1AIES = RFIFG_FLANK_TXBelowThresh | RFIFG_FLANK_TXUnderflow | RFIFG_FLANK_EndOfPacket;
	RF1AIFG = 0;
	RF1AIE  = RFIFG_FLAG_TXBelowThresh | RFIFG_FLAG_TXUnderflow | RFIFG_FLAG_EndOfPacket;

	//Start transmitting
	Strobe(RF_STX);

	return true;
}

bool phy_rx_start(phy_rx_cfg* cfg)
{
	if(get_radiostate() != Idle)
		return false;

	//Set radio state
	state = Receive;

	//Flush the txfifo
	Strobe(RF_SIDLE);
	Strobe(RF_SFRX);

	//Set configuration
	if(!phy_translate_settings(cfg->spectrum_id, cfg->sync_word_class, &fec, &channel_center_freq_index, &channel_bandwidth_index, &preamble_size, &sync_word))
		return false;

	set_length_infinite(false);
	set_data_whitening(true);
	set_channel(channel_center_freq_index, channel_bandwidth_index);
	set_preamble_size(preamble_size);
	set_sync_word(sync_word);
	set_timeout(cfg->timeout);

	//Reset packet received flag
	packetReceived = false;

	//Set buffer position
	bufferPosition = buffer;

	//Set packet length and configure txfifo threshold
	packetLength = cfg->length;
	remainingBytes = cfg->length;

	if(packetLength == 0)
	{
		WriteSingleReg(PKTLEN, 0xFF);
		WriteSingleReg(FIFOTHR, RADIO_FIFOTHR_FIFO_THR_61_4);
	} else {
		WriteSingleReg(PKTLEN, packetLength);
		WriteSingleReg(FIFOTHR, RADIO_FIFOTHR_FIFO_THR_17_48);
	}

	//Enable interrupts
	RF1AIES = RFIFG_FLANK_IOCFG0 | RFIFG_FLANK_RXFilled | RFIFG_FLANK_RXOverflow | RFIFG_FLANK_EndOfPacket;
	RF1AIFG = 0;
	RF1AIE  = RFIFG_FLAG_IOCFG0 | RFIFG_FLAG_RXFilled | RFIFG_FLAG_RXOverflow | RFIFG_FLAG_EndOfPacket;

	//Start receiving
	Strobe(RF_SRX);

	return true;
}

void phy_rx_stop(void)
{
	rxtx_finish_isr();
}

bool phy_read(phy_rx_data* data)
{
	if(packetReceived) {
		data->data = buffer;
		data->length = packetLength;
		data->rssi = rssi;
		data->lqi = lqi;
		packetReceived = false;
		return true;
	}

	return false;
}

bool phy_is_rx_in_progress(void)
{
	return (state == Receive);
}

bool phy_is_tx_in_progress(void)
{
	return (state == Transmit);
}

extern int16_t phy_get_rssi(phy_rx_cfg* cfg)
{
	uint8_t rssi_raw = 0;

	if(get_radiostate() != Idle)
		return false;

	//Set radio Idle
	Strobe(RF_SIDLE);

	//Set configuration
	if(!phy_translate_settings(cfg->spectrum_id, cfg->sync_word_class, &fec, &channel_center_freq_index, &channel_bandwidth_index, &preamble_size, &sync_word))
		return false;

	set_channel(channel_center_freq_index, channel_bandwidth_index);

	//Enable interrupts
    RF1AIE  = RFIFG_FLAG_IOCFG1;
    RF1AIFG = 0;
    RF1AIES = RFIFG_FLANK_IOCFG1;

    //Start receiving
    Strobe(RF_SRX);

    system_lowpower_mode(0, 1); //Todo should system call be made here?

    rxtx_finish_isr();

    rssi_raw = ReadSingleReg(RSSI);
    return calculate_rssi(rssi_raw);
}

/*
 * Interrupt functions
 */
#pragma vector=CC1101_VECTOR
__interrupt void CC1101_ISR (void)
{
  uint16_t isr_vector = RF1AIV >> 1;
  uint16_t edge = (1 << (isr_vector - 1)) & RF1AIES;
  if(edge) isr_vector += 0x11;
  interrupt_table[isr_vector]();
  LPM4_EXIT;  //Todo should system call be made here?
}


void no_interrupt_isr() { }

void end_of_packet_isr()
{
	if (state == Receive) {
		rx_data_isr();
		rxtx_finish_isr();
	} else if (state == Transmit) {
		rxtx_finish_isr();
	} else {
		rxtx_finish_isr();
	}
}

void tx_data_isr()
{
	//Calculate number of free bytes in TXFIFO
	uint8_t txBytes = 64 - ReadSingleReg(TXBYTES);

	//Write data
	if(txBytes > remainingBytes)
		txBytes = remainingBytes;

	WriteBurstReg(RF_TXFIFOWR, bufferPosition, txBytes);
	remainingBytes -= txBytes;
	bufferPosition += txBytes;
}

void rx_data_isr()
{
	//Read number of bytes in RXFIFO
	uint8_t rxBytes = ReadSingleReg(RXBYTES);

    //If length is not set get the length from RXFIFO and set PKTLEN
	if (packetLength == 0) {
		packetLength = ReadSingleReg(RXFIFO);
		WriteSingleReg(PKTLEN, packetLength);
		WriteSingleReg(FIFOTHR, RADIO_FIFOTHR_FIFO_THR_17_48);
		remainingBytes = packetLength - 1;
		bufferPosition[0] = packetLength;
		bufferPosition++;
		rxBytes--;
	}

	//Never read the entire buffer as long as more data is going to be received
    if (remainingBytes > rxBytes) {
    	rxBytes--;
    } else {
    	rxBytes = remainingBytes;
    }

    //Read data from buffer
	ReadBurstReg(RXFIFO, bufferPosition, rxBytes);
	remainingBytes -= rxBytes;
    bufferPosition += rxBytes;

    //When all data has been received read rssi and lqi value and set packetreceived flag
    if(remainingBytes == 0)
    {
    	rssi = calculate_rssi(ReadSingleReg(RXFIFO));
    	lqi = ReadSingleReg(RXFIFO) & 0x7F;
    	packetReceived = true;
    }
}

void rxtx_finish_isr()
{
	//Disable interrupts
	RF1AIE  = 0;
	RF1AIFG = 0;

	//Flush FIFOs and go to sleep
	Strobe(RF_SIDLE);
	Strobe(RF_SFRX);
	Strobe(RF_SFTX);
	Strobe(RF_SPWD);

	//Set radio state
	state = Idle;
}

/*
 * Private functions
 */
RadioState get_radiostate(void)
{
	uint8_t state = Strobe(RF_SNOP) >> 4;

	if(state > 7)
		return Idle;

	return (RadioState)state;
}

void set_channel(uint8_t channel_center_freq_index, uint8_t channel_bandwith_index)
{
	//Set channel center frequency
	WriteSingleReg(CHANNR, channel_center_freq_index);

	//Set channel bandwidth, modulation and symbol rate
	switch(channel_bandwith_index)
	{
	case 0:
		WriteSingleReg(MDMCFG3, RADIO_MDMCFG3_DRATE_M(24));
		WriteSingleReg(MDMCFG4, (RADIO_MDMCFG4_CHANBW_E(1) | RADIO_MDMCFG4_CHANBW_M(0) | RADIO_MDMCFG4_DRATE_E(11)));
		WriteSingleReg(DEVIATN, (RADIO_DEVIATN_E(5) | RADIO_DEVIATN_M(0)));
		break;
	case 1:
		WriteSingleReg(MDMCFG3, RADIO_MDMCFG3_DRATE_M(24));
		WriteSingleReg(MDMCFG4, (RADIO_MDMCFG4_CHANBW_E(2) | RADIO_MDMCFG4_CHANBW_M(0) | RADIO_MDMCFG4_DRATE_E(11)));
		WriteSingleReg(DEVIATN, (RADIO_DEVIATN_E(5) | RADIO_DEVIATN_M(0)));
		break;
	case 2:
		WriteSingleReg(MDMCFG3, RADIO_MDMCFG3_DRATE_M(248));
		WriteSingleReg(MDMCFG4, (RADIO_MDMCFG4_CHANBW_E(1) | RADIO_MDMCFG4_CHANBW_M(0) | RADIO_MDMCFG4_DRATE_E(12)));
		WriteSingleReg(DEVIATN, (RADIO_DEVIATN_E(5) | RADIO_DEVIATN_M(0)));
		break;
	case 3:
		WriteSingleReg(MDMCFG3, RADIO_MDMCFG3_DRATE_M(248));
		WriteSingleReg(MDMCFG4, (RADIO_MDMCFG4_CHANBW_E(0) | RADIO_MDMCFG4_CHANBW_M(1) | RADIO_MDMCFG4_DRATE_E(12)));
		WriteSingleReg(DEVIATN, (RADIO_DEVIATN_E(5) | RADIO_DEVIATN_M(0)));
		break;
	}
}

void set_sync_word(uint16_t sync_word)
{
	WriteSingleReg(SYNC0, (uint8_t)(sync_word >> 8));
	WriteSingleReg(SYNC1, (uint8_t)(sync_word & 0x00FF));
}

void set_preamble_size(uint8_t preamble_size)
{
	uint8_t mdmcfg1 = ReadSingleReg(MDMCFG1) & 0x03;

	if(preamble_size >= 24)
		mdmcfg1 |= 0x70;
	else if(preamble_size >= 16)
		mdmcfg1 |= 0x60;
	else if(preamble_size >= 12)
		mdmcfg1 |= 0x50;
	else if(preamble_size >= 8)
		mdmcfg1 |= 0x40;
	else if(preamble_size >= 6)
		mdmcfg1 |= 0x30;
	else if(preamble_size >= 4)
		mdmcfg1 |= 0x20;
	else if(preamble_size >= 3)
		mdmcfg1 |= 0x10;

	WriteSingleReg(MDMCFG1, mdmcfg1);
}

void set_data_whitening(bool white_data)
{
	uint8_t pktctrl0 = ReadSingleReg(PKTCTRL0) & 0xBF;

	if (white_data)
		pktctrl0 |= RADIO_PKTCTRL0_WHITE_DATA;

	WriteSingleReg(PKTCTRL0, pktctrl0);
}

void set_length_infinite(bool infinite)
{
	uint8_t pktctrl0 = ReadSingleReg(PKTCTRL0) & 0xFC;

	if (infinite)
		pktctrl0 |= RADIO_PKTCTRL0_LENGTH_INF;

	WriteSingleReg(PKTCTRL0, pktctrl0);
}

void set_timeout(uint16_t timeout)
{
	if (timeout == 0) {
		WriteSingleReg(WORCTRL, RADIO_WORCTRL_ALCK_PD);
		WriteSingleReg(MCSM2, RADIO_MCSM2_RX_TIME(7));
	} else {
		WriteSingleReg(WORCTRL, RADIO_WORCTRL_WOR_RES_32ms);
		WriteSingleReg(MCSM2, RADIO_MCSM2_RX_TIME(0));
		WriteSingleReg(WOREVT0, timeout & 0x00FF);
		WriteSingleReg(WOREVT1, timeout >> 8);
	}
}

int16_t calculate_rssi(int8_t rssi_raw)
{
	// CC430 RSSI is 0.5 dBm units, signed byte
    int16_t rssi = (int16_t)rssi_raw;		//Convert to signed 16 bit
    rssi += 128;                      		//Make it positive...
    rssi >>= 1;                        		//...So division to 1 dBm units can be a shift...
    rssi -= 64 + RSSI_OFFSET;     			// ...and then rescale it, including offset

    return rssi;
}

