/* -*- mode: c++; tab-width: 4; c-basic-offset: 4 -*- */
/* This source file is part of the ATMEL AVR32-SoftwareFramework-AT32UC3-1.5.0 Release */

/*This file is prepared for Doxygen automatic documentation generation.*/
/*! \file ******************************************************************
 *
 * \brief Management of the USB device Audio task.
 *
 * This file manages the USB device Audio task.
 *
 * - Compiler:           IAR EWAVR32 and GNU GCC for AVR32
 * - Supported devices:  All AVR32 devices with a USB module can be used.
 * - AppNote:
 *
 * \author               Atmel Corporation: http://www.atmel.com \n
 *                       Support and FAQ: http://support.atmel.no/
 *
 ***************************************************************************/

/* Copyright (c) 2009 Atmel Corporation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * 3. The name of Atmel may not be used to endorse or promote products derived
 * from this software without specific prior written permission.
 *
 * 4. This software may only be redistributed and used in connection with an Atmel
 * AVR product.
 *
 * THIS SOFTWARE IS PROVIDED BY ATMEL "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT ARE
 * EXPRESSLY AND SPECIFICALLY DISCLAIMED. IN NO EVENT SHALL ATMEL BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE
 *
 * Modified by Alex Lee and SDR-Widget team for the sdr-widget project.
 * See http://code.google.com/p/sdr-widget/
 * Copyright under GNU General Public License v2
 */

//_____  I N C L U D E S ___________________________________________________

#include <stdio.h>
#include "usart.h"     // Shall be included before FreeRTOS header files, since 'inline' is defined to ''; leading to
                       // link errors
#include "conf_usb.h"


#if USB_DEVICE_FEATURE == ENABLED

#include "board.h"
#include "features.h"
#ifdef FREERTOS_USED
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#endif
#include "pdca.h"
#include "gpio.h"
#include "usb_drv.h"
#include "usb_descriptors.h"
#include "uac2_usb_descriptors.h"
#include "usb_standard_request.h"
#include "usb_specific_request.h"
#include "uac2_usb_specific_request.h"
#include "device_audio_task.h"
#include "uac2_device_audio_task.h"

#if LCD_DISPLAY				// Multi-line LCD display
#include "taskLCD.h"
#endif

#include "composite_widget.h"
#include "taskAK5394A.h"

//_____ M A C R O S ________________________________________________________


//_____ D E F I N I T I O N S ______________________________________________

#define FB_RATE_DELTA 64

//_____ D E C L A R A T I O N S ____________________________________________


static U32  index, spk_index;
static U16  old_gap = SPK_BUFFER_SIZE;
static U8 audio_buffer_out, spk_buffer_in;	// the ID number of the buffer used for sending out
											// to the USB and reading from USB

static U8 ep_audio_in, ep_audio_out, ep_audio_out_fb;

//!
//! @brief This function initializes the hardware/software resources
//! required for device Audio task.
//!
void uac2_device_audio_task_init(U8 ep_in, U8 ep_out, U8 ep_out_fb)
{
	index     =0;
	audio_buffer_out = 0;
	spk_index = 0;
	spk_buffer_in = 0;
	mute = FALSE;
	spk_mute = FALSE;
	ep_audio_in = ep_in;
	ep_audio_out = ep_out;
	ep_audio_out_fb = ep_out_fb;

	LED_Off(LED0);
	LED_Off(LED1);


	xTaskCreate(uac2_device_audio_task,
				configTSK_USB_DAUDIO_NAME,
				configTSK_USB_DAUDIO_STACK_SIZE,
				NULL,
				configTSK_USB_DAUDIO_PRIORITY,
				NULL);
}


//!
//! @brief Entry point of the device Audio task management
//!

void uac2_device_audio_task(void *pvParameters)
{
	static U32  time=0;
	static Bool startup=TRUE;
	Bool playerStarted = FALSE;
	int i;
	U16 num_samples, num_remaining, gap;

	U8 sample_MSB;
	U8 sample_SB;
	U8 sample_LSB;
	U8 sample_HSB;
	U32 sample;
	const U8 EP_AUDIO_IN = ep_audio_in;
	const U8 EP_AUDIO_OUT = ep_audio_out;
	const U8 EP_AUDIO_OUT_FB = ep_audio_out_fb;
	const U8 IN_LEFT = FEATURE_IN_NORMAL ? 0 : 1;
	const U8 IN_RIGHT = FEATURE_IN_NORMAL ? 1 : 0;
	const U8 OUT_LEFT = FEATURE_OUT_NORMAL ? 0 : 1;
	const U8 OUT_RIGHT = FEATURE_OUT_NORMAL ? 1 : 0;
	volatile avr32_pdca_channel_t *pdca_channel = pdca_get_handler(PDCA_CHANNEL_SSC_RX);
	volatile avr32_pdca_channel_t *spk_pdca_channel = pdca_get_handler(PDCA_CHANNEL_SSC_TX);

	portTickType xLastWakeTime;
	xLastWakeTime = xTaskGetTickCount();

	while (TRUE) {
		vTaskDelayUntil(&xLastWakeTime, UAC2_configTSK_USB_DAUDIO_PERIOD);

		// First, check the device enumeration state
		if (!Is_device_enumerated()) { time=0; startup=TRUE; continue; };

		if( startup ) {
			time+=UAC2_configTSK_USB_DAUDIO_PERIOD;
#define STARTUP_LED_DELAY  10000
			if ( time<= 1*STARTUP_LED_DELAY ) {
				LED_On( LED0 );
				pdca_disable_interrupt_reload_counter_zero(PDCA_CHANNEL_SSC_RX);
				pdca_disable(PDCA_CHANNEL_SSC_RX);
			}
			else if( time== 2*STARTUP_LED_DELAY )
				LED_On( LED1 );
			else if( time== 3*STARTUP_LED_DELAY )
				LED_On( LED2 );
			else if( time== 4*STARTUP_LED_DELAY )
				LED_On( LED3 );
			else if( time== 5*STARTUP_LED_DELAY )
				LED_Off( LED0 );
			else if( time== 6*STARTUP_LED_DELAY )
				LED_Off( LED1 );
			else if( time== 7*STARTUP_LED_DELAY )
				LED_Off( LED2 );
			else if( time== 8*STARTUP_LED_DELAY )
				LED_Off( LED3 );
			else if( time >= 9*STARTUP_LED_DELAY )
			{
				startup=FALSE;

				LED_Off(LED0);
				LED_Off(LED1);


				audio_buffer_in = 0;
				audio_buffer_out = 0;
				spk_buffer_in = 0;
//				spk_buffer_out = 0; // Only place outside taskAK53984A.c where spk_buffer_out is written!
				index = 0;

				if (!FEATURE_ADC_NONE){
					// Wait for the next frame synchronization event
					// to avoid channel inversion.  Start with left channel - FS goes low
					while (!gpio_get_pin_value(AK5394_LRCK));
					while (gpio_get_pin_value(AK5394_LRCK));
					// Enable now the transfer.
					pdca_enable(PDCA_CHANNEL_SSC_RX);
					pdca_enable_interrupt_reload_counter_zero(PDCA_CHANNEL_SSC_RX);
				}

				freq_changed = TRUE;						// force a freq change reset
			}
		}

		if ((usb_alternate_setting == 1)) {
			LED_On(LED1); // Green

			if(Mic_freq_valid) {
				if (current_freq.frequency == 96000) num_samples = 24;
				else if (current_freq.frequency == 48000) num_samples = 12;
				else num_samples = 48;	// freq 192khz

				if (!FEATURE_ADC_NONE) {
					if (Is_usb_in_ready(EP_AUDIO_IN)) {	// Endpoint ready for data transfer?

						Usb_ack_in_ready(EP_AUDIO_IN);	// acknowledge in ready

						// Sync AK data stream with USB data stream
						// AK data is being filled into ~audio_buffer_in, ie if audio_buffer_in is 0
						// buffer 0 is set in the reload register of the pdca
						// So the actual loading is occuring in buffer 1
						// USB data is being taken from audio_buffer_out

						// find out the current status of PDCA transfer
						// gap is how far the audio_buffer_out is from overlapping audio_buffer_in

						num_remaining = pdca_channel->tcr;
						if (audio_buffer_in != audio_buffer_out) {
							// AK and USB using same buffer
							if ( index < (AUDIO_BUFFER_SIZE - num_remaining)) gap = AUDIO_BUFFER_SIZE - num_remaining - index;
							else gap = AUDIO_BUFFER_SIZE - index + AUDIO_BUFFER_SIZE - num_remaining + AUDIO_BUFFER_SIZE;
						}
						else {
							// usb and pdca working on different buffers
							gap = (AUDIO_BUFFER_SIZE - index) + (AUDIO_BUFFER_SIZE - num_remaining);
						}

						if ( gap < AUDIO_BUFFER_SIZE/2 ) {
							// throttle back, transfer less
							num_samples--;
						}
						else if (gap > (AUDIO_BUFFER_SIZE + AUDIO_BUFFER_SIZE/2)) {
							// transfer more
							num_samples++;
						}

						Usb_reset_endpoint_fifo_access(EP_AUDIO_IN);
						for( i=0 ; i < num_samples ; i++ ) {
							   // Fill endpoint with samples
							if (!mute) {
								if (audio_buffer_out == 0) {
									sample_LSB = audio_buffer_0[index+IN_LEFT];
									sample_SB = audio_buffer_0[index+IN_LEFT] >> 8;
									sample_MSB = audio_buffer_0[index+IN_LEFT] >> 16;
								}
								else {
									sample_LSB = audio_buffer_1[index+IN_LEFT];
									sample_SB = audio_buffer_1[index+IN_LEFT] >> 8;
									sample_MSB = audio_buffer_1[index+IN_LEFT] >> 16;
								}

								Usb_write_endpoint_data(EP_AUDIO_IN, 8, sample_LSB);
								Usb_write_endpoint_data(EP_AUDIO_IN, 8, sample_SB);
								Usb_write_endpoint_data(EP_AUDIO_IN, 8, sample_MSB);

								if (audio_buffer_out == 0) {
									sample_LSB = audio_buffer_0[index+IN_RIGHT];
									sample_SB = audio_buffer_0[index+IN_RIGHT] >> 8;
									sample_MSB = audio_buffer_0[index+IN_RIGHT] >> 16;
								}
								else {
									sample_LSB = audio_buffer_1[index+IN_RIGHT];
									sample_SB = audio_buffer_1[index+IN_RIGHT] >> 8;
									sample_MSB = audio_buffer_1[index+IN_RIGHT] >> 16;
								}

								Usb_write_endpoint_data(EP_AUDIO_IN, 8, sample_LSB);
								Usb_write_endpoint_data(EP_AUDIO_IN, 8, sample_SB);
								Usb_write_endpoint_data(EP_AUDIO_IN, 8, sample_MSB);

								index += 2;
								if (index >= AUDIO_BUFFER_SIZE) {
									index=0;
									audio_buffer_out = 1 - audio_buffer_out;
								}
							}
							else {
								Usb_write_endpoint_data(EP_AUDIO_IN, 8, 0x00);
								Usb_write_endpoint_data(EP_AUDIO_IN, 8, 0x00);
								Usb_write_endpoint_data(EP_AUDIO_IN, 8, 0x00);
								Usb_write_endpoint_data(EP_AUDIO_IN, 8, 0x00);
								Usb_write_endpoint_data(EP_AUDIO_IN, 8, 0x00);
								Usb_write_endpoint_data(EP_AUDIO_IN, 8, 0x00);
							}
						}
						Usb_send_in(EP_AUDIO_IN);		// send the current bank
					}
				} // end FEATURE_ADC
			}
		} // end alt setting 1

		if (usb_alternate_setting_out == 1) {

			LED_On(LED0); // Red

			if (Is_usb_in_ready(EP_AUDIO_OUT_FB)) {	// Endpoint buffer free ?
				Usb_ack_in_ready(EP_AUDIO_OUT_FB);	// acknowledge in ready

				Usb_reset_endpoint_fifo_access(EP_AUDIO_OUT_FB);
				// Sync DAC spk data stream by calculating gap and provide feedback
				num_remaining = spk_pdca_channel->tcr;
				if (spk_buffer_in != spk_buffer_out) {
					// DAC and USB using same buffer

					if ( spk_index < (SPK_BUFFER_SIZE - num_remaining))
						gap = SPK_BUFFER_SIZE - num_remaining - spk_index;
					else
						gap = SPK_BUFFER_SIZE - spk_index + SPK_BUFFER_SIZE - num_remaining + SPK_BUFFER_SIZE;
				}
				else {
					// usb and pdca working on different buffers
					gap = (SPK_BUFFER_SIZE - spk_index) + (SPK_BUFFER_SIZE - num_remaining);
				}

				//feedback calculate only in playing mode
				if (Is_usb_full_speed_mode()) {			// FB rate is 3 bytes in 10.14 format

				// BSB 20120912 UAC2 feedback rewritten with outer outer and inner bounds, use 2*FB_RATE_DELTA for outer bounds
#define SPK_GAP_U2	SPK_BUFFER_SIZE * 6 / 4	// 6 A half buffer up in distance	=> Speed up host a lot
#define	SPK_GAP_U1	SPK_BUFFER_SIZE * 5 / 4	// 5 A quarter buffer up in distance => Speed up host a bit
#define SPK_GAP_NOM	SPK_BUFFER_SIZE	* 4 / 4	// 4 Ideal distance is half the size of linear buffer
#define SPK_GAP_L1	SPK_BUFFER_SIZE * 3 / 4 // 3 A quarter buffer down in distance => Slow down host a bit
#define SPK_GAP_L2	SPK_BUFFER_SIZE * 2 / 4 // 2 A half buffer down in distance => Slow down host a lot


					if(playerStarted) {
						if (gap < old_gap) {
							if (gap < SPK_GAP_L2) { 		// gap < outer lower bound => 2*FB_RATE_DELTA
//								LED_Toggle(LED0);
								FB_rate -= 2*FB_RATE_DELTA;
								old_gap = gap;
//								print_dbg_char_char('-');
							}
							else if (gap < SPK_GAP_L1) { 	// gap < inner lower bound => 1*FB_RATE_DELTA
//								LED_Toggle(LED0); // Only toggle LEDs outside outer bonds
								FB_rate -= FB_RATE_DELTA;
								old_gap = gap;
							}
						}
						else if (gap > old_gap) {
							if (gap > SPK_GAP_U2) { 	// gap > outer upper bound => 2*FB_RATE_DELTA
//								LED_Toggle(LED1);
								FB_rate += 2*FB_RATE_DELTA;
								old_gap = gap;
//								print_dbg_char_char('+');
							}
							else if (gap > SPK_GAP_U1) { 		// gap > inner upper bound => 1*FB_RATE_DELTA
//								LED_Toggle(LED1); // Only toggle LEDs outside outer bonds
								FB_rate += FB_RATE_DELTA;
								old_gap = gap;
							}
						}
					} // end if(playerStarted)

					sample_LSB = FB_rate;
					sample_SB = FB_rate >> 8;
					sample_MSB = FB_rate >> 16;
					Usb_write_endpoint_data(EP_AUDIO_OUT_FB, 8, sample_LSB);
					Usb_write_endpoint_data(EP_AUDIO_OUT_FB, 8, sample_SB);
					Usb_write_endpoint_data(EP_AUDIO_OUT_FB, 8, sample_MSB);
				} // end if (Is_usb_full_speed_mode())
				else {
					// HS mode
					// FB rate is 4 bytes in 16.16 format

					//feedback calculate only in playing mode
					if(playerStarted) {
						if (gap < old_gap) {
							if (gap < SPK_GAP_L2) { 		// gap < outer lower bound => 2*FB_RATE_DELTA
//								LED_Toggle(LED0);
								FB_rate -= 2*FB_RATE_DELTA;
								old_gap = gap;
//								print_dbg_char_char('-');
							}
							else if (gap < SPK_GAP_L1) { 	// gap < inner lower bound => 1*FB_RATE_DELTA
//								LED_Toggle(LED0); // Only toggle LEDs outside outer bonds
								FB_rate -= FB_RATE_DELTA;
								old_gap = gap;
							}
						}
						else if (gap > old_gap) {
							if (gap > SPK_GAP_U2) { 	// gap > outer upper bound => 2*FB_RATE_DELTA
//								LED_Toggle(LED1);
								FB_rate += 2*FB_RATE_DELTA;
								old_gap = gap;
//								print_dbg_char_char('+');
							}
							else if (gap > SPK_GAP_U1) { 		// gap > inner upper bound => 1*FB_RATE_DELTA
//								LED_Toggle(LED1); // Only toggle LEDs outside outer bonds
								FB_rate += FB_RATE_DELTA;
								old_gap = gap;
							}
						}
					} // end if(playerStarted)

					// HS mode, FB rate is 4 bytes in 16.16 format per 125�s.
					// Internal format is 18.14 samples per 1�s = 16.16 per 250�s
					// i.e. must right-shift once for 16.16 per 125�s.
					sample_LSB = FB_rate >> 1;	// was >> 0
					sample_SB = FB_rate >> 9;	// was >> 8
					sample_MSB = FB_rate >> 17;	// was >> 16
					sample_HSB = FB_rate >> 25;	// was >> 24
					Usb_write_endpoint_data(EP_AUDIO_OUT_FB, 8, sample_LSB);
					Usb_write_endpoint_data(EP_AUDIO_OUT_FB, 8, sample_SB);
					Usb_write_endpoint_data(EP_AUDIO_OUT_FB, 8, sample_MSB);
					Usb_write_endpoint_data(EP_AUDIO_OUT_FB, 8, sample_HSB);
				} // end !if (Is_usb_full_speed_mode())

				if (playerStarted) {
// 					Original Linux quirk replacement code
//					if (((current_freq.frequency == 88200) && (FB_rate > ((88 << 14) + (7 << 14)/10))) ||
//						((current_freq.frequency == 96000) && (FB_rate > ((96 << 14) + (6 << 14)/10))))
//						FB_rate -= FB_RATE_DELTA * 512;


//					Alternative Linux quirk replacement code, insert nominal FB_rate after a short interlude of requesting 99ksps (see uac2_usb_specific_request.c)
					if ( (current_freq.frequency == 88200) && (FB_rate > (98 << 14) ) ) {
						FB_rate = (88 << 14) + (1<<14)/5;
					}
					if ( (current_freq.frequency == 96000) && (FB_rate > (98 << 14) ) ) {
						FB_rate = (96) << 14;
					}
				}

				Usb_send_in(EP_AUDIO_OUT_FB);
			} // end if (Is_usb_in_ready(EP_AUDIO_OUT_FB)) // Endpoint buffer free ?

			if (Is_usb_out_received(EP_AUDIO_OUT)) {

				// BSB 20120907 Indicate packet receive start below

				// BSB 20120910 debug
				// Toggle PX56 = TP50 = GPIO_04
//				if (gpio_get_pin_value(AVR32_PIN_PX56) == 0)
//					gpio_set_gpio_pin(AVR32_PIN_PX56);
//				else
//					gpio_clr_gpio_pin(AVR32_PIN_PX56);

				Usb_reset_endpoint_fifo_access(EP_AUDIO_OUT);

				// BSB debug 20120913
				num_samples = Usb_byte_count(EP_AUDIO_OUT);
//				if ( (num_samples & (U16)7) != 0)
//					print_dbg_char_char('7');
				num_samples = num_samples / 8;

				xSemaphoreTake( mutexSpkUSB, portMAX_DELAY );
				spk_usb_heart_beat++;					// indicates EP_AUDIO_OUT receiving data from host
				spk_usb_sample_counter += num_samples; 	// track the num of samples received
				xSemaphoreGive(mutexSpkUSB);
				if(!playerStarted) {

//					gpio_set_gpio_pin(AVR32_PIN_PX55); // BSB debug 20120911, positive edge marks playerStarted FALSE->TRUE
//					print_dbg_char_char('Y'); // BSB debug 20120911

					playerStarted = TRUE;
					num_remaining = spk_pdca_channel->tcr;
//					if (spk_buffer_in != spk_buffer_out) {
//						spk_buffer_in = 1 - spk_buffer_in;
//					}
					spk_buffer_in = spk_buffer_out; // Replaces the if-test above

//					if (spk_buffer_in == 1)
//						gpio_set_gpio_pin(AVR32_PIN_PX55); // BSB 20120911 debug on GPIO_03
//					else
//						gpio_clr_gpio_pin(AVR32_PIN_PX55); // BSB 20120911 debug on GPIO_03

					spk_index = SPK_BUFFER_SIZE - num_remaining;

					// BSB added 20120912 after UAC2 time bar pull noise analysis
					if (spk_index & (U32)1)
//						print_dbg_char_char('s'); // BSB debug 20120912
					spk_index = spk_index & ~((U32)1); // Clear LSB

					LED_Off(LED0);
					LED_Off(LED1);
				}

				for (i = 0; i < num_samples; i++) {
					if (spk_mute) {
						sample_HSB = 0;
						sample_LSB = 0;
						sample_SB = 0;
						sample_MSB = 0;
					}
					else {
						sample_HSB = Usb_read_endpoint_data(EP_AUDIO_OUT, 8);
						sample_LSB = Usb_read_endpoint_data(EP_AUDIO_OUT, 8);
						sample_SB = Usb_read_endpoint_data(EP_AUDIO_OUT, 8);
						sample_MSB = Usb_read_endpoint_data(EP_AUDIO_OUT, 8);
					}

					sample = (((U32) sample_MSB) << 24) + (((U32)sample_SB) << 16) + (((U32) sample_LSB) << 8) + sample_HSB;
					//sample = (((U32) sample_MSB) << 16) + (((U32)sample_SB) << 8) + sample_LSB;
					if (spk_buffer_in == 0) {
						spk_buffer_0[spk_index+OUT_LEFT] = sample;
					}
					else {
						spk_buffer_1[spk_index+OUT_LEFT] = sample;
					}

					if (spk_mute) {
						sample_HSB = 0;
						sample_LSB = 0;
						sample_SB = 0;
						sample_MSB = 0;
					}
					else {
						sample_HSB = Usb_read_endpoint_data(EP_AUDIO_OUT, 8);
						sample_LSB = Usb_read_endpoint_data(EP_AUDIO_OUT, 8);
						sample_SB = Usb_read_endpoint_data(EP_AUDIO_OUT, 8);
						sample_MSB = Usb_read_endpoint_data(EP_AUDIO_OUT, 8);
					};

					sample = (((U32) sample_MSB) << 24) + (((U32)sample_SB) << 16) + (((U32) sample_LSB) << 8) + sample_HSB;
					//sample = (((U32) sample_MSB) << 16) + (((U32)sample_SB) << 8) + sample_LSB;
					if (spk_buffer_in == 0) {
						spk_buffer_0[spk_index+OUT_RIGHT] = sample;
					}
					else {
						spk_buffer_1[spk_index+OUT_RIGHT] = sample;
					}

					spk_index += 2;
					if (spk_index >= SPK_BUFFER_SIZE) {
						spk_index = 0;
						spk_buffer_in = 1 - spk_buffer_in;

//						if (spk_buffer_in == 1)
//							gpio_set_gpio_pin(AVR32_PIN_PX55); // BSB 20120912 debug on GPIO_03
//						else
//							gpio_clr_gpio_pin(AVR32_PIN_PX55); // BSB 20120912 debug on GPIO_03

					}
				} // end for
				Usb_ack_out_received_free(EP_AUDIO_OUT);
			}	// end if (Is_usb_out_received(EP_AUDIO_OUT))
		} // end if (usb_alternate_setting_out == 1)
		else {
			playerStarted=FALSE;
//			gpio_clr_gpio_pin(AVR32_PIN_PX55); // BSB 20120911 debug
			old_gap = SPK_BUFFER_SIZE;
		}
	} // end while vTask
}

#endif  // USB_DEVICE_FEATURE == ENABLED
