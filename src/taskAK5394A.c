/* -*- mode: c++; tab-width: 4; c-basic-offset: 4 -*- */
/*
 * taskAK5394A.c
 *
 *  Created on: Feb 14, 2010
 *  Refactored on: Feb 26, 2011
 *      Author: Alex
 *
 * Copyright (C) Alex Lee
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 */
//_____  I N C L U D E S ___________________________________________________

//#include <stdio.h>
#include "usart.h"     // Shall be included before FreeRTOS header files, since 'inline' is defined to ''; leading to
                       // link errors
#include "conf_usb.h"


#include <avr32/io.h>
#if __GNUC__
#  include "intc.h"
#endif
#include "board.h"
#ifdef FREERTOS_USED
#include "FreeRTOS.h"
#include "task.h"
#endif
#include "usb_drv.h"
#include "gpio.h"
#include "ssc_i2s.h"
#include "pm.h"
#include "Mobo_config.h"
#include "pdca.h"
#include "usb_standard_request.h"
#include "features.h"
#include "device_audio_task.h"
#include "taskAK5394A.h"
#include "cycle_counter.h"

//_____ M A C R O S ________________________________________________________

//_____ D E F I N I T I O N S ______________________________________________

//_____ D E C L A R A T I O N S ____________________________________________

volatile U32 spk_usb_heart_beat = 0, old_spk_usb_heart_beat = 0;
volatile U32 spk_usb_sample_counter = 0, old_spk_usb_sample_counter = 0;
xSemaphoreHandle mutexSpkUSB;

static const gpio_map_t SSC_GPIO_MAP = {
	{SSC_RX_CLOCK, SSC_RX_CLOCK_FUNCTION},
	{SSC_RX_DATA, SSC_RX_DATA_FUNCTION},
	{SSC_RX_FRAME_SYNC, SSC_RX_FRAME_SYNC_FUNCTION},
	{SSC_TX_CLOCK, SSC_TX_CLOCK_FUNCTION},
	{SSC_TX_DATA, SSC_TX_DATA_FUNCTION},
	{SSC_TX_FRAME_SYNC, SSC_TX_FRAME_SYNC_FUNCTION}
};

static const pdca_channel_options_t PDCA_OPTIONS = {
	.addr = (void *)audio_buffer_0,         // memory address
	.pid = AVR32_PDCA_PID_SSC_RX,           // select peripheral
	.size = ADC_BUFFER_SIZE,              // transfer counter
	.r_addr = NULL,                         // next memory address
	.r_size = 0,                            // next transfer counter
	.transfer_size = PDCA_TRANSFER_SIZE_WORD  // select size of the transfer - 32 bits
};

static const pdca_channel_options_t SPK_PDCA_OPTIONS = {
	.addr = (void *)spk_buffer_0,         // memory address
	.pid = AVR32_PDCA_PID_SSC_TX,           // select peripheral
	.size = DAC_BUFFER_SIZE,              // transfer counter
	.r_addr = NULL,                         // next memory address
	.r_size = 0,                            // next transfer counter
	.transfer_size = PDCA_TRANSFER_SIZE_WORD  // select size of the transfer - 32 bits
};

volatile U32 audio_buffer_0[ADC_BUFFER_SIZE];
volatile U32 audio_buffer_1[ADC_BUFFER_SIZE];
volatile U32 spk_buffer_0[DAC_BUFFER_SIZE];
volatile U32 spk_buffer_1[DAC_BUFFER_SIZE];

volatile avr32_ssc_t *ssc = &AVR32_SSC;

volatile int ADC_buf_DMA_write; // Written by interrupt handler, initiated by sequential code
volatile int DAC_buf_DMA_read; // Written by interrupt handler, initiated by sequential code
volatile int ADC_buf_USB_IN; // Written by sequential code
volatile int DAC_buf_USB_OUT; // Written by sequential code
volatile avr32_pdca_channel_t *pdca_channel; // Initiated below
volatile avr32_pdca_channel_t *spk_pdca_channel; // Initiated below
#if ((defined HW_GEN_DIN10) || (defined HW_GEN_DIN20))
	volatile U16 DAC_num_remaining = 0;
#endif


// BSB 20131201 attempting improved playerstarted detection
volatile S32 usb_buffer_toggle;

// BSB 20140917 attempting to help uacX_device_audio_task.c synchronize to DMA
volatile U8 audio_OUT_alive;
volatile U8 audio_OUT_must_sync;


/*! \brief The PDCA interrupt handler for the ADC interface.
 *
 * The handler reload the PDCA settings with the correct address and size using the reload register.
 * The interrupt will happen when the reload counter reaches 0
 */
__attribute__((__interrupt__)) static void pdca_int_handler(void) {
	if (ADC_buf_DMA_write == 0) {
		// Set PDCA channel reload values with address where data to load are stored, and size of the data block to load.
		// Register names are different from those used in AVR32108. BUT: it seems pdca_reload_channel() sets the
		// -next- pointer, the one to be selected automatically after the current one is done. That may be why
		// we choose the same buffer number here as in the seq. code
		pdca_reload_channel(PDCA_CHANNEL_SSC_RX, (void *)audio_buffer_1, ADC_BUFFER_SIZE);
		ADC_buf_DMA_write = 1;
#ifdef USB_STATE_MACHINE_DEBUG
    	gpio_set_gpio_pin(AVR32_PIN_PX17);			// Pin 83
#endif
	}
	else if (ADC_buf_DMA_write == 1) {
		pdca_reload_channel(PDCA_CHANNEL_SSC_RX, (void *)audio_buffer_0, ADC_BUFFER_SIZE);
		ADC_buf_DMA_write = 0;
#ifdef USB_STATE_MACHINE_DEBUG
    	gpio_clr_gpio_pin(AVR32_PIN_PX17);			// Pin 83
#endif
	}

#if ((defined HW_GEN_DIN10) || (defined HW_GEN_DIN20))
	DAC_num_remaining = spk_pdca_channel->tcr;	// How much is left of DAC's DMA? Record here at precise timing and use in seq code
	if (DAC_buf_DMA_read == 1)
		DAC_num_remaining |= BUF_IS_ONE;
#endif


}

/*! \brief The PDCA interrupt handler for the DAC interface.
 *
 * The handler reload the PDCA settings with the correct address and size using the reload register.
 * The interrupt will happen when the reload counter reaches 0
 */
__attribute__((__interrupt__)) static void spk_pdca_int_handler(void) {
	if (DAC_buf_DMA_read == 0) {
		// Set PDCA channel reload values with address where data to load are stored, and size of the data block to load.
		pdca_reload_channel(PDCA_CHANNEL_SSC_TX, (void *)spk_buffer_1, DAC_BUFFER_SIZE);
		DAC_buf_DMA_read = 1;

		/*
#if ((defined HW_GEN_DIN10) || (defined HW_GEN_DIN20))
		if (ADC_buf_USB_IN == -1) {				// At init align ADC_DMA addressing with DAC_DMA addressing
			pdca_init_channel(PDCA_CHANNEL_SSC_RX, &PDCA_OPTIONS); // init PDCA channel with options.
			pdca_enable(PDCA_CHANNEL_SSC_RX);	// Enable I2S reception at MCU's ADC port. FIX: Also do this at sample rate chg?

			pdca_reload_channel(PDCA_CHANNEL_SSC_RX, (void *)audio_buffer_1, ADC_BUFFER_SIZE);
			pdca_enable_interrupt_reload_counter_zero(PDCA_CHANNEL_SSC_RX);
			ADC_buf_DMA_write = 1;
		}
#endif
*/

#ifdef USB_STATE_MACHINE_DEBUG
#ifdef PRODUCT_FEATURE_AMB
		gpio_set_gpio_pin(AVR32_PIN_PX56); // For AMB use PX56/GPIO_04
#else
		gpio_set_gpio_pin(AVR32_PIN_PX33); // BSB 20140820 debug on GPIO_09/TP70 (was PX56 / GPIO_04)
#endif
#endif
	}
	else if (DAC_buf_DMA_read == 1) {
		pdca_reload_channel(PDCA_CHANNEL_SSC_TX, (void *)spk_buffer_0, DAC_BUFFER_SIZE);
		DAC_buf_DMA_read = 0;
/*
#if ((defined HW_GEN_DIN10) || (defined HW_GEN_DIN20))
		if (ADC_buf_USB_IN == -1) {				// At init align ADC_DMA addressing with DAC_DMA addressing
			pdca_init_channel(PDCA_CHANNEL_SSC_RX, &PDCA_OPTIONS); // init PDCA channel with options.
			pdca_enable(PDCA_CHANNEL_SSC_RX);	// Enable I2S reception at MCU's ADC port. FIX: Also do this at sample rate chg?

			pdca_reload_channel(PDCA_CHANNEL_SSC_RX, (void *)audio_buffer_0, ADC_BUFFER_SIZE);
			pdca_enable_interrupt_reload_counter_zero(PDCA_CHANNEL_SSC_RX);
			ADC_buf_DMA_write = 0;
		}
#endif
*/

#ifdef USB_STATE_MACHINE_DEBUG
#ifdef PRODUCT_FEATURE_AMB
		gpio_clr_gpio_pin(AVR32_PIN_PX56); // For AMB use PX56/GPIO_04
#else
		gpio_clr_gpio_pin(AVR32_PIN_PX33); // BSB 20140820 debug on GPIO_09/TP70 (was PX56 / GPIO_04)
#endif
#endif
	}

	// BSB 20131201 attempting improved playerstarted detection, FIX: move to seq. code!
	if (usb_buffer_toggle < USB_BUFFER_TOGGLE_LIM)
		usb_buffer_toggle++;

	// BSB 20140917 attempting to help uacX_device_audio_task.c synchronize to DMA
	if (!audio_OUT_alive)				// If no packet has been received since last DMA reset,
		audio_OUT_must_sync = 1;		// indicate that next arriving packet must enter mid-buffer
	audio_OUT_alive = 0;				// Start detecting packets on audio OUT endpoint at DMA reset
}

/*! \brief Init interrupt controller and register pdca_int_handler interrupt.
 */
static void pdca_set_irq(void) {
	// Disable all interrupt/exception.
	Disable_global_interrupt();

	// Register the compare interrupt handler to the interrupt controller
	// and enable the compare interrupt.
	// (__int_handler) &pdca_int_handler The handler function to register.
	// AVR32_PDCA_IRQ_0 The interrupt line to register to.
	// AVR32_INTC_INT2  The priority level to set for this interrupt line.  INT0 is lowest.
	// INTC_register_interrupt(__int_handler handler, int line, int priority);
	INTC_register_interrupt( (__int_handler) &pdca_int_handler, AVR32_PDCA_IRQ_0, AVR32_INTC_INT2);
	INTC_register_interrupt( (__int_handler) &spk_pdca_int_handler, AVR32_PDCA_IRQ_1, AVR32_INTC_INT1);
	// Enable all interrupt/exception.
	Enable_global_interrupt();
}

void AK5394A_pdca_disable(void) {
}

void AK5394A_pdca_enable(void) {
	pdca_init_channel(PDCA_CHANNEL_SSC_RX, &PDCA_OPTIONS); // init PDCA channel with options.
	pdca_enable_interrupt_reload_counter_zero(PDCA_CHANNEL_SSC_RX);
}

void AK5394A_task_init(const Bool uac1) {
	// Set up CS4344
	// Set up GLCK1 to provide master clock for CS4344
	gpio_enable_module_pin(GCLK1, GCLK1_FUNCTION);	// for DA_SCLK
	// LRCK is SCLK / 64 generated by TX_SSC
	// so SCLK of 6.144Mhz ===> 96khz

	mutexSpkUSB = xSemaphoreCreateMutex();

	pdca_channel = pdca_get_handler(PDCA_CHANNEL_SSC_RX);
	spk_pdca_channel = pdca_get_handler(PDCA_CHANNEL_SSC_TX);


	// FIX: UAC1 must include sampling frequency dependent mobo_clock_division or pm_gc_setup!
	if (uac1)
		mobo_clock_division(FREQ_44);
	else
		mobo_clock_division(FREQ_96);

/*
	if (uac1) {
		pm_gc_setup(&AVR32_PM, AVR32_PM_GCLK_GCLK1, // gc
					0,                  // osc_or_pll: use Osc (if 0) or PLL (if 1)
					1,                  // pll_osc: select Osc0/PLL0 or Osc1/PLL1
					1,                  // diven - enabled
					1);                 // divided by 4.  Therefore GCLK1 = 3.072Mhz
	} else {
		pm_gc_setup(&AVR32_PM, AVR32_PM_GCLK_GCLK1, // gc
					0,                  // osc_or_pll: use Osc (if 0) or PLL (if 1)
					1,                  // pll_osc: select Osc0/PLL0 or Osc1/PLL1
					1,                  // diven - enabled
					0);                 // divided by 2.  Therefore GCLK1 = 6.144Mhz
	}
	pm_gc_enable(&AVR32_PM, AVR32_PM_GCLK_GCLK1);
*/


	pm_enable_osc1_ext_clock(&AVR32_PM);	// OSC1 is clocked by 12.288Mhz Osc
	// from AK5394A Xtal Oscillator
	pm_enable_clk1(&AVR32_PM, OSC1_STARTUP);


	// Assign GPIO to SSC.
	gpio_enable_module(SSC_GPIO_MAP, sizeof(SSC_GPIO_MAP) / sizeof(SSC_GPIO_MAP[0]));
	gpio_enable_pin_glitch_filter(SSC_RX_CLOCK);
	gpio_enable_pin_glitch_filter(SSC_RX_DATA);
	gpio_enable_pin_glitch_filter(SSC_RX_FRAME_SYNC);
	gpio_enable_pin_glitch_filter(SSC_TX_CLOCK);
	gpio_enable_pin_glitch_filter(SSC_TX_DATA);
	gpio_enable_pin_glitch_filter(SSC_TX_FRAME_SYNC);

	// set up SSC, it looks like frequency parameter is NOT in use
	if (uac1) {
		ssc_i2s_init(ssc, 48000, 24, 32, SSC_I2S_MODE_STEREO_OUT_STEREO_IN, FPBA_HZ);
	} else {
		ssc_i2s_init(ssc, 96000, 32, 32, SSC_I2S_MODE_STEREO_OUT_STEREO_IN, FPBA_HZ);
	}

	// set up PDCA
	// In order to avoid long slave handling during undefined length bursts (INCR), the Bus Matrix
	// provides specific logic in order to re-arbitrate before the end of the INCR transfer.
	//
	// HSB Bus Matrix: By default the HSB bus matrix mode is in Undefined length burst type (INCR).
	// Here we have to put in single access (the undefined length burst is treated as a succession of single
	// accesses, allowing re-arbitration at each beat of the INCR burst.
	// Refer to the HSB bus matrix section of the datasheet for more details.
	//
	// HSB Bus matrix register MCFG1 is associated with the CPU instruction master interface.
	AVR32_HMATRIX.mcfg[AVR32_HMATRIX_MASTER_CPU_INSN] = 0x1;

	ADC_buf_DMA_write = 0;
	DAC_buf_DMA_read = 0;
	// Register PDCA IRQ interruptS. // Plural those are!
	pdca_set_irq();

	// Init ADC channel for SPDIF buffering
	#if (defined HW_GEN_DIN10) || (defined HW_GEN_DIN20)
		pdca_init_channel(PDCA_CHANNEL_SSC_RX, &PDCA_OPTIONS); // init PDCA channel with options.
		pdca_enable_interrupt_reload_counter_zero(PDCA_CHANNEL_SSC_RX);
		// pdca_enable() is called from WM8805 power unmute functions
	#else
		// Init PDCA channel with the pdca_options.
		if (!FEATURE_ADC_NONE) {
			pdca_init_channel(PDCA_CHANNEL_SSC_RX, &PDCA_OPTIONS); // init PDCA channel with options.
			pdca_enable_interrupt_reload_counter_zero(PDCA_CHANNEL_SSC_RX);
		}
	#endif

	pdca_init_channel(PDCA_CHANNEL_SSC_TX, &SPK_PDCA_OPTIONS); // init PDCA channel with options.
	pdca_enable_interrupt_reload_counter_zero(PDCA_CHANNEL_SSC_TX);

	//////////////////////////////////////////////
	// Enable now the transfer.
	// Why don't we enable the RX here??
	pdca_enable(PDCA_CHANNEL_SSC_TX);


#ifdef HW_GEN_DIN20

	// At this point in time, the DAC's charge pump is about to start. Give it some time to
	// pull current while the series resistor is engaged.
	cpu_delay_ms(80, FCPU_HZ);

	// Short the shared 12R resistor at charge pump inputs while 12R at LDO input is still engaged. FIX: add board design!
	gpio_clr_gpio_pin(AVR32_PIN_PA22); // TP18

	cpu_delay_ms(200, FCPU_HZ);

	// Short the shared 12R resistor at LDO inputs. FIX: add board design!
	gpio_set_gpio_pin(AVR32_PIN_PX31);

#endif



}
