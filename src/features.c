/* -*- mode: c++; tab-width: 4; c-basic-offset: 4 -*- */
/*
 * features.h
 *
 *  Created on: 2011-02-11
 *      Author: Roger E Critchlow Jr, AD5DZ
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "flashc.h"

#include "features.h"
#include "taskLCD.h"

// Set up NVRAM (EEPROM) storage
#if defined (__GNUC__)
__attribute__((__section__(".userpage")))
#endif
features_t features_nvram;

features_t features = { FEATURES_DEFAULT };

//
// these arrays of names need to be kept in sync
// with the enumerations defined in features.h
//
const char *feature_names[] = {
  // image selection
  "flashyblinky",
  "uac1 audio",
  "uac1 dg8saq",
  "uac2 audio",
  "uac2 dg8saq",
  "hpsdr",
  "test",
  "end",
  // input channel
  "normal",
  "swapped",
  "end",
  // output channel
  "normal",
  "swapped",
  "end",
  // adc
  "none",
  "ak5394a",
  "end",
  // dac
  "none",
  "cs4344",
  "end",
  // end
  "end"
};

const char *feature_index_names[] = {
	"vsn",
	"img",
	"iq in",
	"iq out",
	"adc",
	"dac",
	"end"
};

//
static uint8_t display_row = 0;
static char display_contents[4][21];

static void display_clear() {
	int i;
	xSemaphoreTake( mutexQueLCD, portMAX_DELAY );
	lcd_q_clear();
	xSemaphoreGive( mutexQueLCD );
	display_row = 0;
	for (i = 0; i < 4; i += 1)
		memset(&display_contents[i][0], ' ', 20);
}

static void display_string_and_scroll(char *string) {
	xSemaphoreTake( mutexQueLCD, portMAX_DELAY );
#if 0
	if (display_row == 4) {
		// scroll up
		int i;
		memcpy(&display_contents[0][0], &display_contents[1][0], 3*21);
		for (i = 0; i < 3; i += 1) {
			lcd_q_goto(i,0);
			lcd_q_print(&display_contents[i][0]);
		}
		display_row = 3;
	}
#else
	display_row &= 3;
#endif
	sprintf(&display_contents[display_row][0], "%-20.20s", string);
	lcd_q_goto(display_row, 0);
	lcd_q_print(&display_contents[display_row][0]);
	xSemaphoreGive( mutexQueLCD );
	display_row += 1;
}

static void display_string_scroll_and_delay(char *string, uint16_t delay) {
	display_string_and_scroll(string);
	vTaskDelay( delay );
}

//
void features_init() {
  // Enforce "Factory default settings" when a mismatch is detected between the
  // checksum in the memory copy and the matching number in the NVRAM storage.
  // This can be the result of either a fresh firmware upload, or cmd 0x41 with data 0xff
  if( FEATURE_VERSION != FEATURE_NVRAM_VERSION ) {
    flashc_memcpy((void *)&features_nvram, &features, sizeof(features), TRUE);
  } else {
    memcpy(&features, &features_nvram, sizeof(features));
  }
}

void features_display() {
	int i;
	char buff[32];
	int delay = 10000;
	display_clear();
	display_string_scroll_and_delay("features ram:", delay);
	sprintf(buff, "%s = %02x", feature_index_names[feature_version_index], features[feature_version_index]);
	display_string_scroll_and_delay(buff, delay);
	for (i = feature_image_index; i < feature_end_index; i += 1) {
		strcpy(buff, feature_index_names[i]);
		strcat(buff, " = ");
		if (features[i] < feature_end_values)
			strcat(buff, (char *)feature_names[features[i]]);
		else
			strcat(buff, "invalid!");
		display_string_scroll_and_delay(buff, delay);
	}
	display_string_scroll_and_delay("features nvram:", delay);
	sprintf(buff, "%s = %02x", feature_index_names[feature_version_index], features_nvram[feature_version_index]);
	display_string_scroll_and_delay(buff, delay);
	for (i = feature_image_index; i < feature_end_index; i += 1) {
		strcpy(buff, feature_index_names[i]);
		strcat(buff, " = ");
		if (features_nvram[i] < feature_end_values)
			strcat(buff, (char *)feature_names[features_nvram[i]]);
		else
			strcat(buff, "invalid!");
		display_string_scroll_and_delay(buff, delay);
	}
}
