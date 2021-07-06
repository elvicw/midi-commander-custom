/*
 *
 * EEPROM Routines taken in part form Github - nimaltd/ee24
 *
 * MIDI USB Class and routines started off with Github - spectran/stm32f103-usb-midi
 * but many issues were found resulting a quite a lot of the data handling being modified.
 *
 */

#include "usbd_midi_if.h"
#include "stm32f1xx_hal.h"
#include "midi_sysex_proc.h"
#include "midi_defines.h"
#include <string.h>

extern I2C_HandleTypeDef hi2c1;


#define SYSEX_MAX_LENGTH 64
uint8_t sysex_rx_buffer[SYSEX_MAX_LENGTH];
uint8_t sysex_rx_counter = 0;
uint8_t sysex_tx_assembly_buffer[SYSEX_MAX_LENGTH+32];
uint8_t midi_msg_tx_buffer[SYSEX_MAX_LENGTH];

typedef struct {
	uint8_t start;
	uint8_t manuf_id;
	uint8_t msg_cmd;
	uint8_t start_parameters;

} MIDI_sysex_head_TypeDef;

USBD_MIDI_ItfTypeDef USBD_Interface_fops_FS =
{
  MIDI_DataRx,
  MIDI_DataTx
};

void abort_sysex_message(void){
	sysex_rx_counter = 0;
}

void sysex_send_message(uint8_t* buffer, uint8_t length){
	uint8_t *buff_ptr = buffer;
	uint8_t *assembly_ptr = sysex_tx_assembly_buffer;

	while(buff_ptr < length + buffer){
		uint8_t data_to_go = length + buffer - buff_ptr;

		if(data_to_go > 3){
			assembly_ptr[0] = CIN_SYSEX_STARTS_OR_CONTINUES;
			memcpy(assembly_ptr+1, buff_ptr, 3);
			buff_ptr += 3;
			assembly_ptr += 4;
		} else if (data_to_go == 3) {
			assembly_ptr[0] = CIN_SYSEX_ENDS_WITH_FOLLOWING_THREE_BYTES;
			memcpy(assembly_ptr+1, buff_ptr, 3);
			buff_ptr += 3;
			assembly_ptr += 4;
		} else if (data_to_go == 2) {
			assembly_ptr[0] = CIN_SYSEX_ENDS_WITH_FOLLOWING_TWO_BYTES;
			memcpy(assembly_ptr+1, buff_ptr, 2);
			buff_ptr += 2;
			assembly_ptr += 3;
			*assembly_ptr++ = 0xFF;
		} else if (data_to_go == 1) {
			assembly_ptr[0] = CIN_SYSEX_ENDS_WITH_FOLLOWING_SINGLE_BYTE;
			memcpy(assembly_ptr+1, buff_ptr, 1);
			buff_ptr += 1;
			assembly_ptr += 2;
			*assembly_ptr++ = 0xFF;
			*assembly_ptr++ = 0xFF;
		}
	}

	MIDI_DataTx(sysex_tx_assembly_buffer, assembly_ptr - sysex_tx_assembly_buffer);
}

void sysex_erase_eeprom(uint8_t* data_packet_start){
	if(data_packet_start[0] != 0x42 || data_packet_start[1] != 0x24){
		return;
	}

	uint8_t eraseData[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
	uint32_t bytes = 0;
	while ( bytes < 1024)
	{
		HAL_I2C_Mem_Write(&hi2c1, 0xA0 | ((bytes & 0x0300) >> 7), (bytes & 0xff), I2C_MEMADD_SIZE_8BIT, eraseData, sizeof(eraseData), 100);
		HAL_Delay(5);
		bytes += sizeof(eraseData);
	}

	midi_msg_tx_buffer[0] = SYSEX_START;
	midi_msg_tx_buffer[1] = MIDI_MANUF_ID;
	midi_msg_tx_buffer[2] = SYSEX_RSP_ERASE_EEPROM;
	midi_msg_tx_buffer[3] = SYSEX_END;
	sysex_send_message(midi_msg_tx_buffer, 4);

}

void sysex_write_eeprom(uint8_t* data_packet_start){
	uint16_t ee_byte_address = data_packet_start[0] * 16;

	uint8_t reassembled_array[16];
	data_packet_start++;
	for (int i=0; i<16; i++){
		reassembled_array[i] = data_packet_start[2*i] << 4 | data_packet_start[2*i + 1];
	}

	HAL_I2C_Mem_Write(&hi2c1, 0xA0 | ((ee_byte_address & 0x0300) >> 7), (ee_byte_address & 0xff), I2C_MEMADD_SIZE_8BIT, reassembled_array, 16, 100);

	midi_msg_tx_buffer[0] = SYSEX_START;
	midi_msg_tx_buffer[1] = MIDI_MANUF_ID;
	midi_msg_tx_buffer[2] = SYSEX_RSP_WRITE_EEPROM;
	midi_msg_tx_buffer[3] = SYSEX_END;
	sysex_send_message(midi_msg_tx_buffer, 4);

}

void sysex_dump_eeprom_page(uint8_t page_number){
	uint8_t eeprom_buffer[16];

	midi_msg_tx_buffer[0] = SYSEX_START;
	midi_msg_tx_buffer[1] = MIDI_MANUF_ID;
	midi_msg_tx_buffer[2] = SYSEX_RSP_DUMP_EEPROM;
	midi_msg_tx_buffer[3] = page_number;

	uint16_t ee_byte_address = page_number * 16;
	HAL_StatusTypeDef status = HAL_I2C_Mem_Read(&hi2c1, 0xA0 | ((ee_byte_address & 0x0300) >> 7), (ee_byte_address & 0xff), I2C_MEMADD_SIZE_8BIT, eeprom_buffer, 16, 100);

	if(status == HAL_OK){
		// Because we cannot use the high bit in each byte for a midi packet, we need to break it down.
		// Easiest (and perhaps lasy way) is to break each byte into two nibbles, so that's what we're doing.

		for(int i=0; i<16; i++){
			midi_msg_tx_buffer[4+(i*2)] = eeprom_buffer[i] >> 4;
			midi_msg_tx_buffer[4+(i*2)+1] = eeprom_buffer[i] & 0x0F;
		}


		midi_msg_tx_buffer[4+32] = SYSEX_END;

		sysex_send_message(midi_msg_tx_buffer, 37);
	}
}

void process_sysex_message(void){

	// Check start and end bytes
	if(sysex_rx_buffer[0] != SYSEX_START ||
			sysex_rx_buffer[sysex_rx_counter -1] != SYSEX_END){
		abort_sysex_message();
		return;
	}

	MIDI_sysex_head_TypeDef *pSysexHead = (MIDI_sysex_head_TypeDef*)sysex_rx_buffer;

	if(pSysexHead->manuf_id != MIDI_MANUF_ID){
		abort_sysex_message();
		return;
	}

	switch(pSysexHead->msg_cmd){
	case SYSEX_CMD_DUMP_EEPROM:
		sysex_dump_eeprom_page(pSysexHead->start_parameters);
		break;
	case SYSEX_CMD_ERASE_EEPROM:
		sysex_erase_eeprom(&(pSysexHead->start_parameters));
		break;
	case SYSEX_CMD_WRITE_EEPROM:
		// TODO: check data length
		sysex_write_eeprom(&(pSysexHead->start_parameters));
		break;
	default:
		break;
	}


	sysex_rx_counter = 0;

}

uint16_t MIDI_DataRx(uint8_t *msg, uint16_t length)
{

	uint8_t cable = (msg[0]>>4) & 0xF;

	uint8_t processed_data_cnt = 0;

	while(processed_data_cnt < length){
		uint8_t usb_msg_cin = msg[processed_data_cnt] & 0xF;

		if(sysex_rx_counter != 0){
			if(usb_msg_cin != CIN_SYSEX_STARTS_OR_CONTINUES &&
					usb_msg_cin != CIN_SYSEX_ENDS_WITH_FOLLOWING_SINGLE_BYTE &&
					usb_msg_cin != CIN_SYSEX_ENDS_WITH_FOLLOWING_TWO_BYTES &&
					usb_msg_cin != CIN_SYSEX_ENDS_WITH_FOLLOWING_THREE_BYTES){
				abort_sysex_message();
			}
		}

		switch(usb_msg_cin){
		case CIN_SYSEX_STARTS_OR_CONTINUES:
			memcpy(sysex_rx_buffer + sysex_rx_counter, msg + processed_data_cnt + 1, 3);
			sysex_rx_counter += 3;
			processed_data_cnt += 4;
			break;
		case CIN_SYSEX_ENDS_WITH_FOLLOWING_SINGLE_BYTE:
			sysex_rx_buffer[sysex_rx_counter] = msg[processed_data_cnt + 1];
			sysex_rx_counter++;
			processed_data_cnt += 2;
			process_sysex_message();
			break;
		case CIN_SYSEX_ENDS_WITH_FOLLOWING_TWO_BYTES:
			memcpy(sysex_rx_buffer + sysex_rx_counter, msg + processed_data_cnt + 1, 2);
			sysex_rx_counter += 2;
			processed_data_cnt += 3;
			process_sysex_message();
			break;
		case CIN_SYSEX_ENDS_WITH_FOLLOWING_THREE_BYTES:
			memcpy(sysex_rx_buffer + sysex_rx_counter, msg + processed_data_cnt + 1, 3);
			sysex_rx_counter += 3;
			processed_data_cnt += 4;
			process_sysex_message();
			break;

		default:
			// Un-recognised message - most likely just padding.
			// skip to end of USB packet
			processed_data_cnt = length;
			break;

		}

	}

	return 0;


	uint8_t chan = msg[1] & 0x0F;
	uint8_t msgtype = msg[1] & 0xF0;
	uint8_t b1 =  msg[2];
	uint8_t b2 =  msg[3];
	uint16_t b = ((b2 & 0x7F) << 7) | (b1 & 0x7F);
 
  switch (msgtype)
  {
  case 0x80:
	  //key = b1;
	  //velocity = b2;
	  //notepos = key - 8 + transpose;
	  //stop_note(notepos);
	  //HAL_GPIO_TogglePin(GPIOB,GPIO_PIN_7); //blink LED
	  break;
  case 0x90:
	  //key = b1;
	  //velocity = b2;
	  //notepos = key - 8 + transpose;
	  //if(!velocity)
	  //{
	  //stop_note(notepos);
	  //}
	  //else
	  //{
	  //play_note(notepos, velocity);
	  //}
	  //HAL_GPIO_TogglePin(GPIOB,GPIO_PIN_7); //blink LED
	  break;
  case 0xA0:
	  break;
  case 0xB0:
	  /*ctrl = b1;
					data = b2;
		switch(ctrl)
					{
						case (1): // Modulation Wheel
							mod = data;
								break;
						case (16): // Tuning
							tun = data;
							break;
						case (17): // Wave Select
							wavesel = data >> 5;
							break;
						case (18): // OSC Mix
							oscmix = (((float)(data)) * 0.007874f);
							break;
						case (19): // De-Tune 
							det = data >> 4;
							break;
						case (33): // Scale
							scale = (data - 64) >> 2;
							break;
						case (34): // Resonance
							resonance = (((float)(data)) * 0.007874f * 4.0f);
							break;
						case (35): // Pulse Width Value
							pwval = data;
							break;
						case (36): // PWM LFO Mod Level
							pwm = data;
							break;
						case (37): // VCF Attack
							vcfattack = (((float)(data)) * 10.0f);
							break;
						case (38): // VCF Decay
							vcfdecay = (((float)(data)) * 10.0f);
							break;
						case (39): // VCF Sustain
							vcfsustain = (((float)(data)) * 0.007874f);
							break;
						case (40): // VCF Release
							vcfrelease = (((float)(data)) * 10.0f);
							break;
						case (42): // VCA Attack
							vcaattack = (((float)(data)) * 10.0f);
							break;
						case (43): // VCA Decay
							vcadecay = (((float)(data)) * 10.0f);
							break;
						case (44): // VCA Sustain
							vcasustain = (((float)(data)) * 0.007874f);
							break;
						case (45): // VCA Release
							vcarelease = (((float)(data)) * 10.0f);
							break;
						case (48): // VCF Follow Level
							vcfkflvl = (((float)(data)) * 0.007874f);
							break;
						case (49): // ENV Follow Level
							envkflvl = (((float)(data)) * 0.007874f);
							break;
						case (50): // Velocity Select
							velsel = data >> 5;
							break;
						case (51): // VCF Envelope Level
							vcfenvlvl = (((float)(data)) * 0.007874f);
							break;
						case (12): // Mod LFO rate
							lfo1rate = (128 - data) << 2; 
							break;
						case (13): // Pwm LFO rate
							lfo2rate = (128 - data) << 3;
							break;
						case (14): // VCF LFO Mod Level
							vcf = data;
							break;
						case (15): // Vcf LFO rate
							lfo3rate = (128 - data) << 3;
							break;
						case (64): // Sustain pedal controller
							sus = data;
							break;
					}*/
	  break;
  case 0xC0:
	  //data = b1;
	  break;
  case 0xD0:
	  break;
  case 0xE0:
	  //data = b2;
	  //		bend = data;
	  break;
  case 0xF0: {

	  break;
  }
  }
  return 0;
}

uint16_t MIDI_DataTx(uint8_t *msg, uint16_t length)
{
  USBD_MIDI_SendPacket(msg, length);
  return USBD_OK;
}
