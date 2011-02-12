/* This source file is part of the ATMEL AVR32-SoftwareFramework-AT32UC3-1.5.0 Release */

/*This file is prepared for Doxygen automatic documentation generation.*/
/*! \file ******************************************************************
 *
 * \brief Processing of USB device specific enumeration requests.
 *
 * This file contains the specific request decoding for enumeration process.
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
 * Modified by Alex Lee and sdr-widget team since Feb 2010.  Copyright General Purpose Licence v2.
 * Please refer to http://code.google.com/p/sdr-widget/
 */

//_____ I N C L U D E S ____________________________________________________

#include "conf_usb.h"


#if USB_DEVICE_FEATURE == ENABLED

#include "usb_drv.h"
#include "usb_descriptors.h"
#include "usb_standard_request.h"
#include "usb_specific_request.h"
#include "usart.h"
#include "pm.h"
#include "Mobo_config.h"
#include "usb_audio.h"
#include "device_audio_task.h"


//_____ M A C R O S ________________________________________________________


//_____ D E F I N I T I O N S ______________________________________________

extern pm_freq_param_t   pm_freq_param;


//_____ P R I V A T E   D E C L A R A T I O N S ____________________________

U8 usb_feature_report[3];
U8 usb_report[3];

U8 g_u8_report_rate=0;

S_line_coding   line_coding;
U8 clock_selected = 1;
Bool clock_changed = FALSE;

S_freq current_freq;
Bool freq_changed = FALSE;

static U8    wValue_msb;
static U8    wValue_lsb;
static U16   wIndex;
static U16   wLength;


extern const    void *pbuffer;
extern          U16   data_to_transfer;

U8 dg8saqBuffer[32];	// 32 bytes long return buffer for DG8SAQ commands


int Speedx[38] = {\
0x03,0x00,				//Size
0x80,0xbb,0x00,0x00,	//48k Min
0x80,0xbb,0x00,0x00,	//48k Max
0x00,0x00,0x00,0x00,	// 0 Res

0x00,0x77,0x01,0x00,	//96k Min
0x00,0x77,0x01,0x00,	//96k Max
0x00,0x00,0x00,0x00,	// 0 Res

0x00,0xee,0x02,0x00,	//192k Min
0x00,0xee,0x02,0x00,	//192k Max
0x00,0x00,0x00,0x00	// 0 Res
};



//_____ D E C L A R A T I O N S ____________________________________________






//! @brief This function configures the endpoints of the device application.
//! This function is called when the set configuration request has been received.
//!
void usb_user_endpoint_init(U8 conf_nb)
{

   if( Is_usb_full_speed_mode() )
   {

     (void)Usb_configure_endpoint(EP_RF_IN,
                            EP_ATTRIBUTES_1,
                            DIRECTION_IN,
                            EP_SIZE_1_FS,
                            DOUBLE_BANK);
     (void)Usb_configure_endpoint(EP_IQ_IN,
                            EP_ATTRIBUTES_2,
                            DIRECTION_IN,
                            EP_SIZE_2_FS,
                            DOUBLE_BANK);
     (void)Usb_configure_endpoint(EP_IQ_OUT,
                            EP_ATTRIBUTES_3,
                            DIRECTION_OUT,
                            EP_SIZE_3_FS,
                            DOUBLE_BANK);

   }else{

     (void)Usb_configure_endpoint(EP_RF_IN,
                             EP_ATTRIBUTES_1,
                             DIRECTION_IN,
                             EP_SIZE_1_HS,
                             DOUBLE_BANK);
     (void)Usb_configure_endpoint(EP_IQ_IN,
                              EP_ATTRIBUTES_2,
                              DIRECTION_IN,
                              EP_SIZE_2_HS,
                              DOUBLE_BANK);
     (void)Usb_configure_endpoint(EP_IQ_OUT,
                              EP_ATTRIBUTES_3,
                              DIRECTION_OUT,
                              EP_SIZE_3_HS,
                              DOUBLE_BANK);

   }
}


//! This function is called by the standard USB read request function when
//! the USB request is not supported. This function returns TRUE when the
//! request is processed. This function returns FALSE if the request is not
//! supported. In this case, a STALL handshake will be automatically
//! sent by the standard USB read request function.
//!
Bool usb_user_read_request(U8 type, U8 request)
{

   // Read wValue
   wValue_lsb = Usb_read_endpoint_data(EP_CONTROL, 8);
   wValue_msb = Usb_read_endpoint_data(EP_CONTROL, 8);
   wIndex = usb_format_usb_to_mcu_data(16, Usb_read_endpoint_data(EP_CONTROL, 16));
   wLength = usb_format_usb_to_mcu_data(16, Usb_read_endpoint_data(EP_CONTROL, 16));


   return FALSE;  // No supported request
}


//! This function returns the size and the pointer on a user information
//! structure
//!
Bool usb_user_get_descriptor(U8 type, U8 string)
{
  pbuffer = NULL;

  switch (type)
  {
  case STRING_DESCRIPTOR:
    switch (string)
    {
    case LANG_ID:
      data_to_transfer = sizeof(usb_user_language_id);
      pbuffer = &usb_user_language_id;
      break;

    case MAN_INDEX:
      data_to_transfer = sizeof(usb_user_manufacturer_string_descriptor);
      pbuffer = &usb_user_manufacturer_string_descriptor;
      break;

    case PROD_INDEX:
      data_to_transfer = sizeof(usb_user_product_string_descriptor);
      pbuffer = &usb_user_product_string_descriptor;
      break;

    case SN_INDEX:
      data_to_transfer = sizeof(usb_user_serial_number);
      pbuffer = &usb_user_serial_number;
      break;

    case CLOCK_SOURCE_1_INDEX:
      data_to_transfer = sizeof(usb_user_clock_source_1);
      pbuffer = &usb_user_clock_source_1;
      break;

    case CLOCK_SOURCE_2_INDEX:
      data_to_transfer = sizeof(usb_user_clock_source_2);
      pbuffer = &usb_user_clock_source_2;
      break;

    case CLOCK_SELECTOR_INDEX:
       data_to_transfer = sizeof(usb_user_clock_selector);
       pbuffer = &usb_user_clock_selector;
       break;

    case WL_INDEX:
       data_to_transfer = sizeof(usb_user_wl);
       pbuffer = &usb_user_wl;
       break;

    case AIT_INDEX:
       data_to_transfer = sizeof(usb_user_ait);
       pbuffer = &usb_user_ait;
       break;

    case AOT_INDEX:
       data_to_transfer = sizeof(usb_user_aot);
       pbuffer = &usb_user_aot;
       break;

    case AIN_INDEX:
       data_to_transfer = sizeof(usb_user_ain);
       pbuffer = &usb_user_ain;
       break;

    case AIA_INDEX:
       data_to_transfer = sizeof(usb_user_aia);
       pbuffer = &usb_user_aia;
       break;


    default:
      break;
    }
    break;

  default:
    break;
  }

  return pbuffer != NULL;
}

Bool usb_user_DG8SAQ(U8 type, U8 command){

	U16 wValue, wIndex, wLength;
	U8 replyLen;
	int x;

    // Grab the wValue, wIndex / wLength
	wValue  = usb_format_usb_to_mcu_data(16, Usb_read_endpoint_data(EP_CONTROL, 16));
	wIndex  = usb_format_usb_to_mcu_data(16, Usb_read_endpoint_data(EP_CONTROL, 16));
	wLength  = usb_format_usb_to_mcu_data(16, Usb_read_endpoint_data(EP_CONTROL, 16));

		//-------------------------------------------------------------------------------
		// Process USB Host to Device transmissions.  No result is returned.
		//-------------------------------------------------------------------------------
		if (type == (DRD_OUT | DRT_STD | DRT_VENDOR))
		{
		        Usb_ack_setup_received_free();
		        while (!Is_usb_control_out_received());
		        Usb_reset_endpoint_fifo_access(EP_CONTROL);

		        //for (x = 0; x<wLength;x++)
			    if (wLength>0) for (x = wLength-1; x>=0;x--)
		        {
			        dg8saqBuffer[x] = Usb_read_endpoint_data(EP_CONTROL, 8);
		        }
		        Usb_ack_control_out_received_free();
		        Usb_ack_control_in_ready_send();
		        while (!Is_usb_control_in_ready());

				// This is our all important hook - Do the magic... control Si570 etc...
				dg8saqFunctionWrite(command, wValue, wIndex, dg8saqBuffer, wLength);
		}
		//-------------------------------------------------------------------------------
		// Process USB query commands and return a result (flexible size data payload)
		//-------------------------------------------------------------------------------
		else if (type == (DRD_IN | DRT_STD | DRT_VENDOR))
		{
			// This is our all important hook - Process and execute command, read CW paddle state etc...
			replyLen = dg8saqFunctionSetup(command, wValue, wIndex, dg8saqBuffer);

			Usb_ack_setup_received_free();

	        Usb_reset_endpoint_fifo_access(EP_CONTROL);

	        // Write out if packet is larger than zero
	  		if (replyLen)
	  		{
				for (x = replyLen-1; x>=0;x--)
		        {
			        Usb_write_endpoint_data(EP_CONTROL, 8, dg8saqBuffer[x]);	// send the reply
		        }
	  		}

            Usb_ack_control_in_ready_send();
            while (!Is_usb_control_in_ready());			// handshake modified by Alex 16 May 2010

	        while ( !Is_usb_control_out_received());
	        Usb_ack_control_out_received_free();

		}

		return TRUE;
}


#endif  // USB_DEVICE_FEATURE == ENABLED
