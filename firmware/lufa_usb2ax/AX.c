/* ****************************************************************************
Copyright (C) 2014 Nicolas Saugnier (nicolas.saugnier [at] esial {dot} net),
                   Richard Ibbotson (richard.ibbotson [at] btinternet {dot} com)

Date   : 2012/05/06

Based on :
    - USBtoSerial project in the LUFA Library by Dean Camera (original file)
    - USB2AXSYNC by Richard Ibbotson (USB2AX optimizations, sync_read and Dynamixel communication)
    - arbotix_python ROS package and Arbotix Robocontroller firmware by Michael Ferguson (sync_read and part of the Dynamixel communication)

Original copyright notice : 
  Copyright 2013  Dean Camera (dean [at] fourwalledcubicle [dot] com)

  Permission to use, copy, modify, distribute, and sell this
  software and its documentation for any purpose is hereby granted
  without fee, provided that the above copyright notice appear in
  all copies and that both that the copyright notice and this
  permission notice and warranty disclaimer appear in supporting
  documentation, and that the name of the author not be used in
  advertising or publicity pertaining to distribution of the
  software without specific, written prior permission.

  The author disclaims all warranties with regard to this
  software, including all implied warranties of merchantability
  and fitness.  In no event shall the author be liable for any
  special, indirect or consequential damages or any damages
  whatsoever resulting from loss of use, data or profits, whether
  in an action of contract, negligence or other tortious action,
  arising out of or in connection with the use or performance of
  this software.
*******************************************************************************/

#include "AX.h" 
#include <avr/eeprom.h>
#include "debug.h"

extern RingBuffer_t ToUSB_Buffer;

// registers
uint8_t regs[] = {MODEL_NUMBER_L, MODEL_NUMBER_H, FIRMWARE_VERSION, AX_ID_DEVICE, USART_TIMEOUT, SEND_TIMEOUT, RECEIVE_TIMEOUT, 0, 0, 0, 0, 0, 0, 0, 0, 0};
uint8_t min_vals[] = { 10,  10,  10,   0,   0,   0,   0,   0,   0,   0,   0,   0}; // starts at START_RW_ADDR
uint8_t max_vals[] = {254, 254, 254, 255, 255, 255, 255, 255, 255, 255, 255, 255}; // 


//read only
#define ADDR_MODEL_NUMBER_L         0
#define ADDR_MODEL_NUMBER_H         1
#define ADDR_FIRMWARE_VERSION       2
#define ADDR_AX_ID_DEVICE           3

// read/write
#define ADDR_USART_TIMEOUT          4
#define ADDR_SEND_TIMEOUT           5
#define ADDR_RECEIVE_TIMEOUT        6
//#define ADDR_...                    7
//#define ADDR_...                    8
//#define ADDR_...                    9
//#define ADDR_...                    10
//#define ADDR_...                    11
//#define ADDR_...                    12
//#define ADDR_...                    13
//#define ADDR_...                    14
//#define ADDR_...                    15

#define START_RW_ADDR       ADDR_USART_TIMEOUT


void axInit(){
    // init R/W regs from EEPROM if some have been saved
    // else keep default
}


/*
 * Send status packet
 */
void axStatusPacket(uint8_t err, uint8_t* data, uint8_t nb_bytes){
	 // TODO use a return level value, except for PING...
    
    uint16_t checksum = AX_ID_DEVICE + 2 + nb_bytes + err;
	
	cdc_send_byte(0xff);
	cdc_send_byte(0xff);
	cdc_send_byte(AX_ID_DEVICE);
	cdc_send_byte(2 + nb_bytes);
	cdc_send_byte(err);
	for (uint8_t i = 0; i < nb_bytes; i++){
    	cdc_send_byte(data[i]);
		checksum += data[i];
	}
	cdc_send_byte(255-(checksum %256));
}


// try to read a Dynamixel packet
// return true if successful, false otherwise 
uint16_t axReadPacket(uint8_t length){
    usart_timer = 0;
	// wait until the expected number of byte has been read
	while( local_rx_buffer_count < length ){ 
		if(usart_timer > USART_TIMEOUT){
		    break;
		}
	}
	if (local_rx_buffer_count != length){
		return false;
	}
	
	// TODO check for error in the packet, so that we don't wait if the status packet says that something went wrong... ?
	
	// compute checksum
    uint8_t checksum = 0; // accumulator for checksum
    for(uint8_t i=2; i < length; i++){
        checksum += local_rx_buffer[i];
    }
    
    if((checksum%256) != 255){
        return false; // invalid checksum
    }else{
        return true;
    }
}


/** Read register value(s) */
int axGetRegister(uint8_t id, uint8_t addr, uint8_t nb_bytes){  
   // 0xFF 0xFF ID LENGTH INSTRUCTION PARAM... CHECKSUM    
    uint16_t checksum = ~((id + 6 + addr + nb_bytes)%256);

    local_rx_buffer_count = 0;
    setTX();
    serial_write(0xFF);
    serial_write(0xFF);
    serial_write(id);
    serial_write(4);    // length
    serial_write(AX_CMD_READ_DATA);
    serial_write(addr);
    serial_write(nb_bytes);
    serial_write(checksum);
    setRX();

    return axReadPacket(nb_bytes + 6);
}


// sync_read performs a cycle of Dynamixel reads to collect the data from servos to return over USB
void sync_read(uint8_t* params, uint8_t nb_params){
    
    // divert incoming data to a buffer for local processing  
	passthrough_mode = AX_DIVERT;
	
	uint8_t addr = params[0];    // address to read in control table
    uint8_t nb_to_read = params[1];    // # of bytes to read from each servo
    uint8_t nb_servos = nb_params - 2;
	
	cdc_send_byte(0xff);
	cdc_send_byte(0xff);
	cdc_send_byte(AX_ID_DEVICE);
	cdc_send_byte(2+(nb_to_read*nb_servos));
	cdc_send_byte(0);  //error code
    
	// get ax data
	uint8_t fcount = 5; // count of number of bytes in USB IN endpoint buffer
	uint8_t checksum = AX_ID_DEVICE + (nb_to_read*nb_servos) + 2;  // start accumulating the checksum
    uint8_t* servos = params + 2; // pointer to the ids of the servos to read from
	uint8_t* data = local_rx_buffer + 5; // pointer to the parameters in the packet received from the servo
	for(uint8_t servo_id = 0; servo_id < nb_servos; servo_id++){
        if( axGetRegister(servos[servo_id], addr, nb_to_read)){
            for(uint8_t i = 0; i < nb_to_read; i++){
                checksum += data[i];
                cdc_send_byte(data[i]);
				if(fcount++ >= CDC_TXRX_EPSIZE){ // periodically try to flush data to the host
					send_USB_data();
                    fcount = 0;
                }
            }
        } else{
            for(uint8_t i = 0; i < nb_to_read; i++){
                checksum += 0xFF;
                cdc_send_byte(0xFF);
				if(fcount++ >= CDC_TXRX_EPSIZE){ 
					send_USB_data();
                    fcount = 0;
                }
            }
		}
    }
	
    cdc_send_byte(255-((checksum)%256));
 	
	// allow data from USART to be sent directly to USB
	passthrough_mode = AX_PASSTHROUGH;
}


void local_read(uint8_t addr, uint8_t nb_bytes){
	uint16_t top = (uint16_t)addr + nb_bytes;
	if ( nb_bytes == 0 || top > sizeof(regs) ){
		axStatusPacket( AX_ERROR_RANGE, NULL, 0 );
	} else {
	    axStatusPacket(AX_ERROR_NONE, regs + addr, nb_bytes);
    }
}


uint8_t can_write_data(uint8_t addr, uint8_t* data, uint8_t nb_bytes){
    uint16_t top = (uint16_t)addr + nb_bytes;
    if (nb_bytes == 0 || top > sizeof(regs) || addr < START_RW_ADDR ){
        return false; 
    }
    // Check that the value written are acceptable
    for (uint8_t i = 0 ; i<nb_bytes; i++ ){
        uint8_t val = data[i];
        if (val < min_vals[addr - START_RW_ADDR + i] || val > max_vals[addr - START_RW_ADDR + i]){
            return false;
        }
    }
    return true;
}

void local_write(uint8_t addr, uint8_t* data, uint8_t nb_bytes){
	if ( ! can_write_data(addr, data, nb_bytes) ){
        axStatusPacket( AX_ERROR_RANGE, NULL, 0 );
    } else {
        memcpy(regs+addr, data, nb_bytes);
        //TODO save EEPROM
        axStatusPacket( AX_ERROR_NONE, NULL, 0 ); 
    }
}
