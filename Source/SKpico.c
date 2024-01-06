/*
	   ______/  _____/  _____/     /   _/    /             /
	 _/           /     /     /   /  _/     /   ______/   /  _/             ____/     /   ______/   ____/
	  ___/       /     /     /   ___/      /   /         __/               /    _/   /   /         /     /
		 _/    _/    _/    _/   /  _/     /  _/         /  _/             _____/    /  _/        _/    _/
  ______/   _____/  ______/   _/    _/  _/    _____/  _/    _/          _/        _/    _____/    ____/

  SKpico.c

  SIDKick pico - SID-replacement with dual-SID emulation using a RPi pico and reSID 0.16 by Dag Lem
  Copyright (c) 2023 Carsten Dachsbacher <frenetic@dachsbacher.de>

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma GCC optimize( "Ofast", "omit-frame-pointer", "modulo-sched", "modulo-sched-allow-regmoves", "gcse-sm", "gcse-las", "inline-small-functions", "delete-null-pointer-checks", "expensive-optimizations" ) 

#define CLOCK_300MHZ

// enable output via PWM
#define OUTPUT_VIA_PWM

// enable output via PCM5102-DAC
#define USE_DAC

// enable flashing LED
#define FLASH_LED

// enable heuristics for detecting digi-playing techniques
#define SUPPORT_DIGI_DETECT

// enable support of special 8-bit DAC mode
#define SID_DAC_MODE_SUPPORT

#include <malloc.h>
#include <ctype.h>
#include <string.h>
#include "pico/stdlib.h"
#include <pico/multicore.h>
#include "hardware/vreg.h"
#include "hardware/pwm.h"  
#include "hardware/flash.h"
#include "hardware/structs/bus_ctrl.h" 
#include "pico/audio_i2s.h"
#include "launch.h"
#include "prgconfig.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"

#ifdef USE_RGB_LED
#undef FLASH_LED
#include "ws2812.pio.h"
#endif

const volatile uint8_t __in_flash() busTimings[ 8 ] = { 11, 15, 1, 2, 3, 4, 5, 6 };
uint8_t DELAY_READ_BUS, DELAY_PHI2;

// support straight DAC output
#ifdef SID_DAC_MODE_SUPPORT
#define SID_DAC_OFF      0
#define SID_DAC_MONO8    1
#define SID_DAC_STEREO8  2
#define SID_DAC_MONO16   4
#define SID_DAC_STEREO16 8
uint8_t sidDACMode = SID_DAC_OFF;
#endif

#define VERSION_STR_SIZE  36
static const unsigned char VERSION_STR[ VERSION_STR_SIZE ] = {
#ifdef USE_DAC
  0x53, 0x4b, 0x10, 0x09, 0x03, 0x0f, '0', '.', '1', '2', '/', 0x44, 0x41, 0x43, '6', '4', 0, 0, 0, 0,   // version string to show
#else
  0x53, 0x4b, 0x10, 0x09, 0x03, 0x0f, '0', '.', '1', '2', '/', 0x50, 0x57, 0x4d, '6', '4', 0, 0, 0, 0,   // version string to show
#endif
  0x53, 0x4b, 0x10, 0x09, 0x03, 0x0f, 0x00, 0x00,   // signature + extension version 0
  0, 12,                                            // firmware version with stepping = 0.12
#ifdef SID_DAC_MODE_SUPPORT                         // support DAC modes? which?
  SID_DAC_MONO8 | SID_DAC_STEREO8,
#else
  0,
#endif
  0, 0, 0, 0, 0 };

extern void initReSID();
extern void resetReSID();
extern void emulateCyclesReSID( int cyclesToEmulate );
extern uint16_t crc16( const uint8_t *p, uint8_t l );
extern void updateConfiguration();
extern void writeReSID( uint8_t A, uint8_t D );
extern void writeReSID2( uint8_t A, uint8_t D );
extern void outputReSID( int16_t *left, int16_t *right );
extern void readRegs( uint8_t *p1, uint8_t *p2 );

#define D0			0
#define A0			16
#define A5			14
#define A8			15
#define OE_DATA		8
#define RW			9
#define PHI			12
#define AUDIO_PIN	13
#define SID			21
#define LED_BUILTIN 25
#define bSID		( 1 << SID )
#define bPHI		( 1 << PHI )
#define bRW			( 1 << RW )
#define bOE			( 1 << OE_DATA )
#define bPOTX		( 1 << 10 )
#define bPOTY		( 1 << 11 )

#define bPWN_POT	( ( 1 << AUDIO_PIN ) | bPOTX | bPOTY | ( 7 << 26 ) )

#define VIC_HALF_CYCLE( g )	( !( (g) & bPHI ) )
#define CPU_HALF_CYCLE( g )	(  ( (g) & bPHI ) )
#define WRITE_ACCESS( g )	( !( (g) & bRW ) )
#define READ_ACCESS( g )	(  ( (g) & bRW ) )
#define SID_ACCESS( g )		( !( (g) & bSID ) )
#define SID_ADDRESS( g )	(  ( (g) >> A0 ) & 0x1f )

#define WAIT_FOR_VIC_HALF_CYCLE { do { g = *gpioInAddr; } while ( !( VIC_HALF_CYCLE( g ) ) ); }
#define WAIT_FOR_CPU_HALF_CYCLE { do { g = *gpioInAddr; } while ( !( CPU_HALF_CYCLE( g ) ) ); }

#define SET_DATA( D )   \
      { sio_hw->gpio_set = ( D ); \
        sio_hw->gpio_clr = ( (~(uint32_t)( D )) & 255 ); } 

const uint32_t  sidFlags[ 6 ] = { bSID, ( 1 << A5 ), ( 1 << A8 ), ( 1 << A5 ) | ( 1 << A8 ), ( 1 << A8 ), ( 1 << A8 ) };
extern uint32_t SID2_FLAG;
extern uint8_t  SID2_IOx_global;

#ifdef USE_DAC
#define AUDIO_RATE (44100)
#else
#define AUDIO_RATE (73242)
#endif

#define AUDIO_BITS 11
#define AUDIO_BIAS ( (uint16_t)1 << ( AUDIO_BITS - 1 ) )

#define SAMPLES_PER_BUFFER 256

extern uint32_t C64_CLOCK;

#define DELAY_Nx3p2_CYCLES( c )							\
    asm volatile( "mov  r0, %0" : : "r" (c) : "r0" );	\
    asm volatile( "1: sub  r0, r0, #1" : : : "r0" );	\
    asm volatile( "bne   1b" );

void initGPIOs()
{
	for ( int i = 0; i < 23; i++ )
	{
		gpio_init( i );
		gpio_set_function( i, GPIO_FUNC_SIO );
	}
	sio_hw->gpio_clr = bPOTX | bPOTY;
	gpio_set_pulls( 12, false, false );
	gpio_set_pulls( 13, false, false );
	gpio_set_pulls( A8, true, false );
	
	gpio_set_dir_all_bits( bOE | bPWN_POT | ( 1 << LED_BUILTIN ) );
}


#ifdef USE_DAC

audio_buffer_pool_t *ap;

audio_buffer_pool_t *initI2S() 
{
	static audio_format_t audio_format = { .format = AUDIO_BUFFER_FORMAT_PCM_S16, .sample_freq = 44100, .channel_count = 2 };
	static audio_buffer_format_t producer_format = { .format = &audio_format, .sample_stride = 8 };
	audio_buffer_pool_t *pool = audio_new_producer_pool( &producer_format, 3, SAMPLES_PER_BUFFER ); 

	audio_i2s_config_t config = { .data_pin = PICO_AUDIO_I2S_DATA_PIN, .clock_pin_base = PICO_AUDIO_I2S_CLOCK_PIN_BASE, .dma_channel = 0, .pio_sm = 0 };
	const audio_format_t *format = audio_i2s_setup( &audio_format, &config );

	audio_i2s_connect( pool );
	
	// initial buffer data
	audio_buffer_t *b = take_audio_buffer( pool, true );
	int16_t *samples = (int16_t *)b->buffer->bytes;
	memset( samples, 0, b->max_sample_count * 4 );
	b->sample_count = b->max_sample_count;
	give_audio_buffer( pool, b );

	return pool;
}

uint16_t audioPos = 0, 
		 audioOutPos = 0;
uint32_t audioBuffer[ 256 ];
uint8_t  firstOutput = 1;

#endif


uint64_t c64CycleCounter = 0;

volatile int32_t newSample = 0xffff, newLEDValue;
volatile uint64_t lastSIDEmulationCycle = 0;

uint8_t outRegisters[ 34 * 2 ];
uint8_t *outRegisters_2 = &outRegisters[ 34 ];

#define sidAutoDetectRegs outRegisters

#define SID_MODEL_DETECT_VALUE_8580 2
#define SID_MODEL_DETECT_VALUE_6581 3
#define REG_AUTO_DETECT_STEP		32
#define REG_MODEL_DETECT_VALUE		33

uint8_t busValue = 0;

uint16_t SID_CMD = 0xffff;

#define  RING_SIZE 256
uint16_t ringBuf[ RING_SIZE ];
uint32_t ringTime[ RING_SIZE ];
uint8_t  ringWrite = 0;
uint8_t  ringRead  = 0;

void resetEverything() 
{
	ringRead = ringWrite = 0;
}

uint8_t stateGoingTowardsTransferMode = 0;

typedef enum {
	DD_IDLE = 0,
	DD_PREP,
	DD_CONF,
	DD_PREP2,
	DD_CONF2,
	DD_SET,
	DD_VAR1,
	DD_VAR2
} DD_STATE;

DD_STATE ddTB_state[ 3 ] = { DD_IDLE, DD_IDLE, DD_IDLE };
uint64_t ddTB_cycle[ 3 ] = { 0, 0, 0 };
uint8_t  ddTB_sample[ 3 ] = { 0, 0, 0 };

DD_STATE ddPWM_state[ 3 ] = { DD_IDLE, DD_IDLE, DD_IDLE };
uint64_t ddPWM_cycle[ 3 ] = { 0, 0, 0 };
uint8_t  ddPWM_sample[ 3 ] = { 0, 0, 0 };

#ifdef CLOCK_300MHZ
#define DD_TB_TIMEOUT0	135
#define DD_TB_TIMEOUT	22
#define DD_PWM_TIMEOUT	12
#endif

uint8_t  ddActive[ 3 ];
uint64_t ddCycle[ 3 ] = { 0, 0, 0 };
uint8_t	 sampleValue[ 3 ] = { 0 };

extern void outputDigi( uint8_t voice, int32_t value );

extern uint8_t POT_FILTER_global;
uint8_t paddleFilterMode = 0;
uint8_t  SID2_IOx;
uint8_t potXExtrema[ 2 ], potYExtrema[ 2 ];

void updateEmulationParameters()
{
	extern uint8_t POT_SET_PULLDOWN;
	gpio_set_pulls( 13, false, POT_SET_PULLDOWN > 0 );

	paddleFilterMode = POT_FILTER_global;
	if ( paddleFilterMode == 3 )
	{
		potXExtrema[ 0 ] = potYExtrema[ 0 ] = 128 - 30;
		potXExtrema[ 1 ] = potYExtrema[ 1 ] = 128 + 30;
	} else
	{
		potXExtrema[ 0 ] = potYExtrema[ 0 ] = 0;
		potXExtrema[ 1 ] = potYExtrema[ 1 ] = 255;
	}

	SID2_IOx = SID2_IOx_global;
}


static inline void put_pixel(uint32_t pixel_grb) 
{
    pio_sm_put_blocking(pio0, 0, pixel_grb << 8u);
}

#define RGB24( r, g, b ) ( ( (uint32_t)(r)<<8 ) | ( (uint32_t)(g)<<16 ) | (uint32_t)(b) )

void runEmulation()
{
	irq_set_mask_enabled( 0xffffffff, 0 );

	// setup PWM for LED 
	gpio_init( LED_BUILTIN );
	gpio_set_dir( LED_BUILTIN, GPIO_OUT );
	gpio_set_function( LED_BUILTIN, GPIO_FUNC_PWM );

	int led_pin_slice = pwm_gpio_to_slice_num( LED_BUILTIN );
	pwm_config configLED = pwm_get_default_config();
	pwm_config_set_clkdiv( &configLED, 1 );
	pwm_config_set_wrap( &configLED, 1l << AUDIO_BITS );
	pwm_init( led_pin_slice, &configLED, true );
	gpio_set_drive_strength( LED_BUILTIN, GPIO_DRIVE_STRENGTH_2MA );
	pwm_set_gpio_level( LED_BUILTIN, 0 );

	// setup PWM and PWM mode for audio 
	#ifdef OUTPUT_VIA_PWM
	gpio_set_dir( AUDIO_PIN, GPIO_OUT );
	gpio_set_function( AUDIO_PIN, GPIO_FUNC_PWM );

	int audio_pin_slice = pwm_gpio_to_slice_num( AUDIO_PIN );
	pwm_config config = pwm_get_default_config();
	pwm_config_set_clkdiv( &config, 1 );
	pwm_config_set_wrap( &config, 1l << AUDIO_BITS );
	pwm_init( audio_pin_slice, &config, true );
	gpio_set_drive_strength( AUDIO_PIN, GPIO_DRIVE_STRENGTH_12MA );
	pwm_set_gpio_level( AUDIO_PIN, 0 );

	static const uint32_t PIN_DCDC_PSM_CTRL = 23;
	gpio_init( PIN_DCDC_PSM_CTRL );
	gpio_set_dir( PIN_DCDC_PSM_CTRL, GPIO_OUT );
	gpio_put( PIN_DCDC_PSM_CTRL, 1 );
	#endif

	#ifdef USE_RGB_LED
	initProgramWS2812();
	#endif

	// decompress config-tool
	extern char *exo_decrunch( const char *in, char *out );
	exo_decrunch( &prgCodeCompressed[ prgCodeCompressed_size ], &prgCode[ prgCode_size ] );

	initReSID();

	updateEmulationParameters();

	#ifdef USE_DAC  
	ap = initI2S();
	#endif

	#ifdef SID_DAC_MODE_SUPPORT
	int32_t DAC_L = 0, DAC_R = 0;
	#endif

	extern uint8_t SID_DIGI_DETECT;	// from config: heuristics activated?
	uint8_t  sampleTechnique = 0;
	uint16_t silence = 0;
	uint16_t ramp = 0;
	int32_t  lastS = 0;

	uint64_t lastD418Cycle = 0;
	#ifdef USE_RGB_LED
	uint8_t  digiD418Visualization = 0;
	#endif

	while ( 1 )
	{
		uint64_t targetEmulationCycle = c64CycleCounter;
		while ( ringRead != ringWrite )
		{
			#ifdef SID_DAC_MODE_SUPPORT
			// this is placed here, as we don't use time stamps in DAC mode
			if ( sidDACMode && !( ringBuf[ ringRead ] & ( 1 << 15 ) ) )
			{
				register uint16_t cmd = ringBuf[ ringRead ++ ];
				uint8_t reg = ( cmd >> 8 ) & 0x1f;

				if ( sidDACMode == SID_DAC_STEREO8 )
				{
					if ( reg == 0x18 )
						DAC_L = ( (int)( cmd & 255 ) - 128 ) << 7;
					if ( reg == 0x19 )
						DAC_R = ( (int)( cmd & 255 ) - 128 ) << 7; 
				} else
				if ( sidDACMode == SID_DAC_MONO8 && reg == 0x18 )
				{
					DAC_L = DAC_R = ( (int)( cmd & 255 ) - 128 ) << 7;
				}
				continue;
			} 
			#endif

			if ( ringTime[ ringRead ] > lastSIDEmulationCycle )
			{
				targetEmulationCycle = ringTime[ ringRead ];
				break;
			}
			register uint16_t cmd = ringBuf[ ringRead ++ ];

			if ( cmd & ( 1 << 15 ) )
			{
				writeReSID2( ( cmd >> 8 ) & 0x1f, cmd & 255 );
			} else
			{
				uint8_t reg = cmd >> 8;

				// this is a work-around if very early writes to SID-registers are missed due to boot-up time (and d418 is only set once)
				static uint8_t d418_volume_set = 0;
				if ( !d418_volume_set )
				{
					if ( reg == 0x18 ) d418_volume_set = 1;
					if ( ringRead == 33 )
						writeReSID( 0x18, 15 );
				}

				#ifdef USE_RGB_LED
				if ( reg == 0x18 )
				{
					if ( ( targetEmulationCycle - lastD418Cycle ) < 1536 )
					{
						digiD418Visualization = 1;

						// heuristic to detect Mahoney's technique based on findings by Jürgen Wothke used in WebSid (https://bitbucket.org/wothke/websid/src/master/) )
						if ( ( 0x17[ outRegisters ] == 0x3 ) && ( 0x15[ outRegisters ] >= 0xfe ) && ( 0x16[ outRegisters ] >= 0xfe ) &&
							 ( 0x06[ outRegisters ] >= 0xfb ) && ( 0x06[ outRegisters ] == 0x0d[ outRegisters ] ) && ( 0x06[ outRegisters ] == 0x14[ outRegisters ] ) &&
							 ( 0x04[ outRegisters ] == 0x49 ) && ( 0x0b[ outRegisters ] == 0x49 ) && ( 0x12[ outRegisters ] == 0x49 ) )
							digiD418Visualization = 2;
					} else
						digiD418Visualization = 0;

					lastD418Cycle = targetEmulationCycle;
				}
				#endif

				writeReSID( reg, cmd & 255 );

				// pseudo stereo
				if ( SID2_FLAG == ( 1 << 31 ) )
					writeReSID2( reg, cmd & 255 );

				#ifdef SUPPORT_DIGI_DETECT

				//
				// Digi-Playing Detection to bypass reSID
				// (the heuristics below are based on the findings by J�rgen Wothke used in WebSid (https://bitbucket.org/wothke/websid/src/master/) )
				//

				if ( SID_DIGI_DETECT )
				{
					uint8_t voice = 0;
					if ( reg >  6 && reg < 14 ) { voice = 1; reg -= 7; }
					if ( reg > 13 && reg < 21 ) { voice = 2; reg -= 14; }

					//
					// test-bit technique

					#define DD_STATE_TBC( state, cycle )	{ ddTB_state[ voice ] = state; ddTB_cycle[ voice ] = cycle; }
					#define DD_STATE_TB( state )			{ ddTB_state[ voice ] = state; }
					#define DD_NO_TIMEOUT( threshold )		( ( c64CycleCounter - ddTB_cycle[ voice ] ) < threshold )
					
					#define DD_GET_SAMPLE( s )	{	DD_STATE_TBC( DD_IDLE, 0 );					\
													ddActive[ voice ] = sampleTechnique = 2;	\
													sampleValue[ voice ] = s;					\
													ddCycle[ voice ] = c64CycleCounter; }

					if ( reg == 4 )	
					{	
						uint8_t v = cmd & 0x19;
						switch ( v ) 
						{
						case 0x11:	
							DD_STATE_TBC( DD_PREP, c64CycleCounter );
							break;
						case 0x8: case 0x9:	
							if ( ddTB_state[ voice ] == DD_PREP && DD_NO_TIMEOUT( DD_TB_TIMEOUT0 ) )
								DD_STATE_TBC( DD_SET, c64CycleCounter - 4 ) else
								DD_STATE_TB( DD_IDLE )
							break;
						case 0x1:	// GATE
							if ( ddTB_state[ voice ] == DD_SET && DD_NO_TIMEOUT( DD_TB_TIMEOUT ) )
								DD_STATE_TB( DD_VAR1 ) else
							if ( ddTB_state[ voice ] == DD_VAR2 && DD_NO_TIMEOUT( DD_TB_TIMEOUT ) )
								DD_GET_SAMPLE( ddTB_sample[ voice ] ) else
								DD_STATE_TB( DD_IDLE )
							break;
						case 0x0:	
							if ( ddTB_state[ voice ] == DD_VAR2 && DD_NO_TIMEOUT( DD_TB_TIMEOUT ) )
								DD_GET_SAMPLE( ddTB_sample[ voice ] )
							break;
						}
					} else
					if ( reg == 1 ) 
					{	
						if ( ddTB_state[ voice ] == DD_SET && DD_NO_TIMEOUT( DD_TB_TIMEOUT ) )
						{
							ddTB_sample[ voice ] = cmd & 255;
							DD_STATE_TBC( DD_VAR2, c64CycleCounter )
						} else 
						if ( ddTB_state[ voice ] == DD_VAR1 && DD_NO_TIMEOUT( DD_TB_TIMEOUT ) )
							DD_GET_SAMPLE( cmd & 255 )
					}

					//
					// pulse modulation technique
					
					#define DD_PWM_STATE_TBC( state, cycle )	{ ddPWM_state[ voice ] = state; ddPWM_cycle[ voice ] = cycle; }
					#define DD_PWM_STATE_TB( state )			{ ddPWM_state[ voice ] = state; }
					#define DD_PWM_NO_TIMEOUT( threshold )		( ( c64CycleCounter - ddPWM_cycle[ voice ] ) < threshold )

					#define DD_PWM_GET_SAMPLE( s )	{	DD_PWM_STATE_TBC( DD_IDLE, 0 );				\
														ddActive[ voice ] = sampleTechnique = 1;	\
														sampleValue[ voice ] = s;					\
														ddCycle[ voice ] = c64CycleCounter; }

					if ( reg == 4 ) 
					{
						uint8_t v = cmd & 0x49;	
						switch ( v ) 
						{
						case 0x49:	
							if ( ( ddPWM_state[ voice ] == DD_PREP ) && DD_PWM_NO_TIMEOUT( DD_PWM_TIMEOUT ) )
								DD_PWM_STATE_TBC( DD_CONF, c64CycleCounter ) else
								DD_PWM_STATE_TBC( DD_PREP2, c64CycleCounter ) 
							break;
						case 0x41:	
							if ( ( ddPWM_state[ voice ] == DD_CONF || ddPWM_state[ voice ] == DD_CONF2 ) && DD_PWM_NO_TIMEOUT( DD_PWM_TIMEOUT ) )
								DD_PWM_GET_SAMPLE( ddPWM_sample[ voice ] ) else
								DD_PWM_STATE_TB( DD_IDLE )
							break;
						}
					} else 
					if ( reg == 2 ) 
					{
						if ( ( ddPWM_state[ voice ] == DD_PREP2 ) && DD_PWM_NO_TIMEOUT( DD_PWM_TIMEOUT ) )
							DD_PWM_STATE_TBC( DD_CONF2, c64CycleCounter ) else
							DD_PWM_STATE_TBC( DD_PREP, c64CycleCounter )
						ddPWM_sample[ voice ] = cmd & 255;
					}
				}
				#endif
			}
		} // while

		uint64_t curCycleCount = targetEmulationCycle;

		#ifdef SID_DAC_MODE_SUPPORT
		if ( !sidDACMode )
		#endif
		if ( lastSIDEmulationCycle < curCycleCount )
		{
			#ifdef SUPPORT_DIGI_DETECT
			if ( SID_DIGI_DETECT )
			{
				uint16_t v;

				if ( c64CycleCounter - ddCycle[ 0 ] > 250 ) { ddActive[ 0 ] = 0; outputDigi( 0, 0 ); }
				if ( c64CycleCounter - ddCycle[ 1 ] > 250 ) { ddActive[ 1 ] = 0; outputDigi( 1, 0 ); }
				if ( c64CycleCounter - ddCycle[ 2 ] > 250 ) { ddActive[ 2 ] = 0; outputDigi( 2, 0 ); }

				if ( ddActive[ 0 ] ) { *(int16_t *)&v = ( sampleValue[ 0 ] - 128 ) << 8; v &= ~3; v |= sampleTechnique; outputDigi( 0, *(int16_t *)&v ); }
				if ( ddActive[ 1 ] ) { *(int16_t *)&v = ( sampleValue[ 1 ] - 128 ) << 8; v &= ~3; v |= sampleTechnique; outputDigi( 1, *(int16_t *)&v ); }
				if ( ddActive[ 2 ] ) { *(int16_t *)&v = ( sampleValue[ 2 ] - 128 ) << 8; v &= ~3; v |= sampleTechnique; outputDigi( 2, *(int16_t *)&v ); }

				#ifdef USE_RGB_LED
				if ( sampleTechnique == 1 ) digiD418Visualization = 2;
				#endif
			}
			#endif

			uint64_t cyclesToEmulate = curCycleCount - lastSIDEmulationCycle;
			lastSIDEmulationCycle = curCycleCount;
			emulateCyclesReSID( cyclesToEmulate );
			readRegs( &outRegisters[ 0x1b ], &outRegisters_2[ 0x1b ] );
		}

		if ( newSample == 0xfffe )
		{
			int16_t L, R;

			#ifdef SID_DAC_MODE_SUPPORT
			if ( sidDACMode )
			{
				L = DAC_L;
				R = DAC_R;
				#ifdef USE_RGB_LED
				digiD418Visualization = 2;
				#endif
			} else
			#endif
			outputReSID( &L, &R );

			#ifdef USE_DAC
			// fill buffer, skip/stretch as needed

			if ( audioPos < 256 )
				audioBuffer[ audioPos ] = ( ( *(uint16_t *)&R ) << 16 ) | ( *(uint16_t *)&L );

			audioPos ++;

			audio_buffer_t *buffer = take_audio_buffer( ap, false );
			if ( buffer )
			{
				if ( firstOutput )
					audio_i2s_set_enabled( true );
				firstOutput = 0;

				int16_t *samples = (int16_t *)buffer->buffer->bytes;
				audioOutPos = 0;
				for ( uint i = 0; i < buffer->max_sample_count; i++ ) 
				{
					*(uint32_t *)&samples[ i * 2 + 0 ] = audioBuffer[ audioOutPos ];
					if ( audioOutPos < audioPos - 1 ) audioOutPos ++;
				}
				buffer->sample_count = buffer->max_sample_count;
				give_audio_buffer( ap, buffer );
				audioOutPos = audioPos = 0;
			}

			#endif

			// PWM output via C64/C128 mainboard
			int32_t s = L + R;

			s = ( s >> ( 1 + 16 - AUDIO_BITS ) ) + AUDIO_BIAS;

			if ( abs( s - lastS ) == 0 ) {
				if ( silence < 65530 ) silence ++;
			} else
				silence = 0;
			
			lastS = s;
			
			if ( silence > 16384 ) {
				if ( ramp ) ramp --;
			} else {
				if ( ramp < 1023 ) ramp ++;
			}
			
			if ( ramp < 1023 ) s = ( s * ramp ) >> 10;

			newSample = s;

			s -= AUDIO_BIAS;
			if ( ramp < 1023 ) s = ( s * ramp ) >> 10;
			newLEDValue = abs( s ) << 2;
			s *= s;
			s >>= ( AUDIO_BITS - 5 );
			newLEDValue += s;


			#ifdef USE_RGB_LED
			extern int32_t voiceOutAcc[ 3 ], nSamplesAcc;
			static int32_t r_ = 0, g_ = 0, b_ = 0;

			#define SAMPLE2BRIGHTNESS( _s, res ) {					\
				int32_t s = _s;										\
				s = ( s >> ( 16 - AUDIO_BITS ) );					\
				res = abs( s ) << 2;								\
				s *= s;												\
				s >>= ( AUDIO_BITS - 5 );							\
				res += s; }

			int32_t r, g, b;

			// no LEDs from voice output when using Mahoney's digi technique or PWM techniques
			if ( digiD418Visualization < 2 )
			{
				SAMPLE2BRIGHTNESS( voiceOutAcc[ 0 ] >> 2, r );
				SAMPLE2BRIGHTNESS( voiceOutAcc[ 1 ] >> 2, g );
				SAMPLE2BRIGHTNESS( voiceOutAcc[ 2 ] >> 2, b );
				r_ += r;
				g_ += g;
				b_ += b;
			}

			if ( digiD418Visualization )
			{
				int32_t t = newLEDValue << 7;
				if ( digiD418Visualization == 1 ) t <<= 4;
				r_ += t;
				g_ += t;
				b_ += t;
			}

			static uint16_t smpCnt = 0;
			if ( ++ smpCnt >= 1024 )
			{
				r_ >>= 22;
				g_ >>= 22;
				b_ >>= 22;
				put_pixel( RGB24( r_, g_, b_ ) );
				r_ = g_ = b_ = 0;
				smpCnt = 0;
			}
			#endif
		}
	}
}

void readConfiguration();
void writeConfiguration();

#define CONFIG_MODE_CYCLES		25000
#define TRANSFER_MODE_CYCLES	30000
extern uint8_t config[ 64 ];
extern uint8_t POT_OUTLIER_REJECTION;

uint8_t  transferStage   = 0;
uint16_t launcherAddress = ( launchCode[ 1 ] << 8 ) + launchCode[ 0 ];
uint8_t *transferData	 = (uint8_t *)&launchCode[ 2 ],
		*transferDataEnd = (uint8_t *)&launchCode[ launchSize ];
uint16_t jumpAddress     = 0xD401;
uint8_t  transferReg[ 32 ] = {
	0x78, 0x48, 0x68, 0xA9, launchCode[ 2 ], 0x48, 0x68, 0x8D, 
	launchCode[ 0 ], launchCode[ 1 ], 0x48, 0x68, 0x4C, 0x01, 0xD4 };

inline uint8_t median( uint8_t *x )
{
	int sum, minV, maxV;

	sum = minV = maxV = x[ 0 ];
	
	sum += x[ 1 ];
	if ( x[ 1 ] < minV ) minV = x[ 1 ];
	if ( x[ 1 ] > maxV ) maxV = x[ 1 ];

	sum += x[ 2 ];
	if ( x[ 2 ] < minV ) minV = x[ 2 ];
	if ( x[ 2 ] > maxV ) maxV = x[ 2 ];

	return sum - minV - maxV;
}

void handleBus()
{
	irq_set_mask_enabled( 0xffffffff, 0 );

	outRegisters[ 0x19 ] = 0;
	outRegisters[ 0x1A ] = 0;
	outRegisters[ 0x1B ] = 0;
	outRegisters[ 0x1C ] = 0;
	outRegisters[ 0x22 + 0x19 ] = 0;
	outRegisters[ 0x22 + 0x1A ] = 0;
	outRegisters[ 0x22 + 0x1B ] = 0;
	outRegisters[ 0x22 + 0x1C ] = 0;

	outRegisters[ REG_AUTO_DETECT_STEP ] = outRegisters[ REG_AUTO_DETECT_STEP + 34 ] = 0;
	outRegisters[ REG_MODEL_DETECT_VALUE ] = ( config[ /*CFG_SID1_TYPE*/0 ] == 0 ) ? SID_MODEL_DETECT_VALUE_6581 : SID_MODEL_DETECT_VALUE_8580;
	outRegisters[ REG_MODEL_DETECT_VALUE + 34 ] = ( config[ /*CFG_SID2_TYPE*/8 ] == 0 ) ? SID_MODEL_DETECT_VALUE_6581 : SID_MODEL_DETECT_VALUE_8580;

	register uint32_t gpioDir = bOE | bPWN_POT | ( 1 << LED_BUILTIN ), gpioDirCur = 0;
	register uint32_t g asm( "r10" );
	register uint32_t A asm( "r11" );
	register uint8_t  DELAY_READ_BUS_local = DELAY_READ_BUS,
					  DELAY_PHI2_local = DELAY_PHI2;
	register uint8_t  D asm( "r12" );
	volatile const uint32_t *gpioInAddr = &sio_hw->gpio_in;
	register volatile uint8_t newPotCounter = 0, disableDataLines asm( "r14" ) = 0;
	register uint32_t curSample asm( "r9" ) = 0;
	uint32_t tempR0;

	// variables for potentiometer handling and filtering
	uint8_t potCycleCounter = 0;
	uint8_t newPotXCandidate = 128, newPotYCandidate = 128;
	uint8_t potXHistory[ 3 ], potYHistory[ 3 ], potHistoryCnt = 0;
	uint8_t skipMeasurements = 0;
	int32_t paddleXSmooth = 128 << 8;
	int32_t paddleYSmooth = 128 << 8;
	int32_t paddleXRange, paddleYRange, newX, newY, oldX, oldY;
	uint8_t skipSmoothing = 0;

	potXExtrema[ 0 ] = potYExtrema[ 0 ] = 128 - 30;
	potXExtrema[ 1 ] = potYExtrema[ 1 ] = 128 + 30;

	uint8_t  addrLines = 99;

	int16_t  stateInConfigMode = 0;
	uint32_t stateConfigRegisterAccess = 0;

	gpio_set_dir_all_bits( gpioDir );
	sio_hw->gpio_clr = bOE;

	SID2_IOx = SID2_IOx_global;

	WAIT_FOR_CPU_HALF_CYCLE
	WAIT_FOR_VIC_HALF_CYCLE
	WAIT_FOR_CPU_HALF_CYCLE

	/*   __     __      __        __                     __               __
		/__` | |  \    |__) |  | /__` __ |__|  /\  |\ | |  \ |    | |\ | / _`
		.__/ | |__/    |__) \__/ .__/    |  | /~~\ | \| |__/ |___ | | \| \__>
	*/
handleSIDCommunication:

	// reinitialization of the transfer mode
	transferStage = 0;
	transferData = (uint8_t *)&launchCode[ 2 ];
	transferDataEnd = (uint8_t *)&launchCode[ launchSize ];

	transferReg[ 4 ] = launchCode[ 2 ]; // data
	( *(uint16_t *)&transferReg[ 8 ] ) = launcherAddress = *(uint16_t *)&launchCode[ 0 ];
	( *(uint16_t *)&transferReg[ 13 ] ) = jumpAddress = 0xD401;

	stateInConfigMode = 0;
	stateConfigRegisterAccess = 0;

	while ( true )
	{
		//
		// wait for VIC-halfcycle
		//
		WAIT_FOR_VIC_HALF_CYCLE

		if ( disableDataLines )
		{
			gpio_set_dir_masked( 0xff, 0 );
			disableDataLines = 0;

			if ( stateGoingTowardsTransferMode == 3 )
			{
				stateInConfigMode = TRANSFER_MODE_CYCLES;
				goto transferWaitForCPU_Halfcycle;
			}
		}

		if ( gpioDir != gpioDirCur )
		{
			gpio_set_dir_masked( bPWN_POT, gpioDir );
			gpioDirCur = gpioDir;
		}


		if ( newSample < 0xfffe )
		{
			#ifdef OUTPUT_VIA_PWM
			pwm_set_gpio_level( AUDIO_PIN, newSample );
			#endif
			#ifdef FLASH_LED
			pwm_set_gpio_level( LED_BUILTIN, newLEDValue );
			#endif

			newSample = 0xffff;
		}

		// we have to generate a new sample after C64_CLOCK / AUDIO_RATE cycles
		++ c64CycleCounter;
		curSample += AUDIO_RATE;
		if ( curSample > C64_CLOCK )
		{
			curSample -= C64_CLOCK;
			newSample = 0xfffe;
		}

		// perform some parts of paddle/mouse-smoothing during VIC-cycle
		if ( paddleFilterMode >= 1 && !( newPotCounter & 4 ) )
		{
			switch ( potCycleCounter )
			{
			case 2:
				if ( paddleFilterMode == 1 )
				{
					outRegisters[ 25 ] = newPotXCandidate;
					outRegisters[ 26 ] = newPotYCandidate;
				}

				if ( !skipSmoothing )
				{
					if ( paddleFilterMode == 3 )
					{
						// only for mouse, not paddles
						if ( newPotXCandidate < potXExtrema[ 0 ] ) potXExtrema[ 0 ] --;
						if ( newPotXCandidate > potXExtrema[ 1 ] ) potXExtrema[ 1 ] ++;
						if ( newPotYCandidate < potYExtrema[ 0 ] ) potYExtrema[ 0 ] --;
						if ( newPotYCandidate > potYExtrema[ 1 ] ) potYExtrema[ 1 ] ++;
						paddleXRange = (int)( potXExtrema[ 1 ] - potXExtrema[ 0 ] ) << 8;
						paddleYRange = (int)( potYExtrema[ 1 ] - potYExtrema[ 0 ] ) << 8;
					} else
						paddleXRange = paddleYRange = 256 << 8;
				}
				break;

			case 3:
				if ( !skipSmoothing )
				{
					// starting from here it's the same for mouse and paddles (for the latter extrema are always [0;255])
					newX = (int)( newPotXCandidate - potXExtrema[ 0 ] ) << 8;
					newY = (int)( newPotYCandidate - potYExtrema[ 0 ] ) << 8;

					oldX = paddleXSmooth - ( (int)potXExtrema[ 0 ] << 8 );
					oldY = paddleYSmooth - ( (int)potYExtrema[ 0 ] << 8 );
				}
				break;

			case 4:
				if ( !skipSmoothing )
				{
					if ( ( newX - oldX ) > paddleXRange / 2 )
						oldX += paddleXRange; else
						if ( ( oldX - newX ) > paddleXRange / 2 )
							newX += paddleXRange;

					if ( ( newY - oldY ) > paddleYRange / 2 )
						oldY += paddleYRange; else
						if ( ( oldY - newY ) > paddleYRange / 2 )
							newY += paddleYRange;
				}
				break;

			case 5:
				if ( !skipSmoothing )
				{
					#define EMA	6
					newX = ( oldX * ( 256 - EMA ) + newX * EMA ) >> 8;
					newY = ( oldY * ( 256 - EMA ) + newY * EMA ) >> 8;

					if ( newX >= paddleXRange ) newX -= paddleXRange;
					if ( newY >= paddleYRange ) newY -= paddleYRange;
				}
				break;

			case 6:
				if ( paddleFilterMode > 1 && !skipSmoothing )
				{
					paddleXSmooth = newX + ( (int)potXExtrema[ 0 ] << 8 );
					paddleYSmooth = newY + ( (int)potYExtrema[ 0 ] << 8 );

					outRegisters[ 25 ] = paddleXSmooth >> 8;
					outRegisters[ 26 ] = paddleYSmooth >> 8;
				}
			default:
				break;
			}
		}

		register uint8_t CPUWritesDelay = DELAY_READ_BUS_local;

		//
		// wait for CPU-halfcycle
		//
		WAIT_FOR_CPU_HALF_CYCLE

		DELAY_Nx3p2_CYCLES( DELAY_PHI2_local )

		g = *gpioInAddr;
		A = SID_ADDRESS( g );

		uint8_t *reg;

		if ( SID2_IOx )
		{
			if ( SID_ACCESS( g ) )
			{
				g &= ~SID2_FLAG;
				goto HANDLE_SID_ACCESS;
			} else
			// fancy remapping to handle all SID2-addresses in the same way
			if ( !( g & SID2_FLAG ) )
			{
				g ^= SID2_FLAG | bSID;
				goto HANDLE_SID_ACCESS;
			} 
		}

		if ( SID_ACCESS( g ) )
		{
			HANDLE_SID_ACCESS:
			reg = outRegisters + ( ( g & SID2_FLAG ) ? 34 : 0 );
			if ( READ_ACCESS( g ) )
			{
				if ( A >= 0x1d )
				{
					const uint8_t jmpCode[ 3 ] = { 0x4c, 0x00, 0xd4 }; // jmp $d400
					D = jmpCode[ A - 0x1d ];
					stateGoingTowardsTransferMode ++;
				} else
				{
					if ( reg[ REG_AUTO_DETECT_STEP ] == 1 && (A == 0x1b) )
					{
					  reg[ REG_AUTO_DETECT_STEP ] = 0;
					  D = reg[ REG_MODEL_DETECT_VALUE ];
					} else
					{
						if ( A >= 0x19 && A <= 0x1c )
							D = reg[ A ]; else
							D = busValue;
					}
					stateGoingTowardsTransferMode = 0;
				}

				gpio_set_dir_all_bits( 255 | gpioDir );
				SET_DATA( D );

				disableDataLines = 1;
			} else
			//if ( WRITE_ACCESS( g ) )
			{
				DELAY_Nx3p2_CYCLES( CPUWritesDelay )

				stateGoingTowardsTransferMode = 0;

				D = ( *gpioInAddr ) & 255;

				if ( A == 0x1f )
				{
					if ( D == 0xff )
					{
						stateInConfigMode = CONFIG_MODE_CYCLES; // SID remains in config mode for 1/40sec
						stateConfigRegisterAccess = 0;
						goto configWaitForVIC_Halfcycle;
					}
					#ifdef SID_DAC_MODE_SUPPORT
					else if ( D == 0xfc )
					{
						sidDACMode = SID_DAC_MONO8;
					} else
					if ( D == 0xfb )
					{
						sidDACMode = SID_DAC_STEREO8;
					}
					#endif
				} else
				{
					SID_CMD = ( A << 8 ) | D;
					if ( g & SID2_FLAG ) SID_CMD |= 1 << 15;
				  	ringTime[ ringWrite ] = c64CycleCounter;
					ringBuf[ ringWrite ++ ] = SID_CMD;


					if ( REG_AUTO_DETECT_STEP[ reg ] == 0 &&
						 0x12[ reg ] == 0xff &&
						 0x0e[ reg ] == 0xff &&
						 0x0f[ reg ] == 0xff &&
						 A == 0x12 && D == 0x20 )
					{
						reg[ REG_AUTO_DETECT_STEP ] = 1;
					}
					reg[ A ] = D;
				}
				disableDataLines = 1;
			}
		}

		/*   __   __  ___  ___      ___    __         ___ ___  ___  __
			|__) /  \  |  |__  |\ |  |  | /  \  |\/| |__   |  |__  |__)
			|    \__/  |  |___ | \|  |  | \__/  |  | |___  |  |___ |  \
		*/
		if ( potCycleCounter == 0 )
		{
			if ( newPotCounter & 4 )			// in phase 2?
			{
				gpioDir |= bPOTX | bPOTY;       // enter phase 1
				newPotCounter = 0;

				if ( POT_OUTLIER_REJECTION > 1 )
				{
					#define GUARD 8
					if ( ( newPotXCandidate < ( 64 - GUARD ) || newPotXCandidate > ( 192 + GUARD ) ) ||
						 ( newPotYCandidate < ( 64 - GUARD ) || newPotYCandidate > ( 192 + GUARD ) ) )
						skipMeasurements = 2;
				}

				if ( skipMeasurements )
				{
					skipMeasurements --;
					skipSmoothing = 1;
				} else
				{
					skipSmoothing = 0;

					if ( !paddleFilterMode )
					{
						outRegisters[ 25 ] = newPotXCandidate;
						outRegisters[ 26 ] = newPotYCandidate;
					} else
					{
						potXHistory[ potHistoryCnt ] = newPotXCandidate;
						potYHistory[ potHistoryCnt ] = newPotYCandidate;
						potHistoryCnt ++;
						if ( potHistoryCnt >= 3 )
							potHistoryCnt = 0;

						newPotXCandidate = median( potXHistory );
						newPotYCandidate = median( potYHistory );
					}
				}
			} else
			{
				gpioDir &= ~( bPOTX | bPOTY );  // enter phase 2
				newPotCounter = 0b111;
			}
		} else
		if ( newPotCounter & 4 )				// in phase 2, but cycle counter != 0
		{
			if ( ( newPotCounter & 1 ) && ( ( g & bPOTY ) || potCycleCounter == 255 ) )
			{
				newPotXCandidate = potCycleCounter;
				newPotCounter &= 0b110;
			}
			if ( ( newPotCounter & 2 ) && ( ( g & bPOTX ) || potCycleCounter == 255 ) )
			{
				newPotYCandidate = potCycleCounter;
				newPotCounter &= 0b101;
			}
			// test validity of measurements
			if ( POT_OUTLIER_REJECTION )
			{
				//if ( ( !( newPotCounter & 1 ) && !( g & bPOTY ) ) ||
				//	 ( !( newPotCounter & 2 ) && !( g & bPOTX ) ) )
				//	skipMeasurements = 2;
				if ( ( !( newPotCounter & 1 ) && !( g & bPOTY ) && potCycleCounter == ( ( newPotXCandidate + 255 ) >> 1 ) ) ||
					 ( !( newPotCounter & 2 ) && !( g & bPOTX ) && potCycleCounter == ( ( newPotYCandidate + 255 ) >> 1 ) ) )
					skipMeasurements = 2;
			}
		}


		potCycleCounter ++;

	} // while ( true )


	/*   __   __   __     ___  __             __   ___  ___  __      __        __                     __               __
		|__) |__) / _` __  |  |__)  /\  |\ | /__` |__  |__  |__)    |__) |  | /__` __ |__|  /\  |\ | |  \ |    | |\ | / _`
		|    |  \ \__>     |  |  \ /~~\ | \| .__/ |    |___ |  \    |__) \__/ .__/    |  | /~~\ | \| |__/ |___ | | \| \__>
	*/

	while ( true )
	{
		//
		// wait for VIC-halfcycle
		//
		WAIT_FOR_VIC_HALF_CYCLE

		if ( disableDataLines )
		{
			disableDataLines = 0;
			gpio_set_dir_all_bits( gpioDir );
		}

	transferWaitForCPU_Halfcycle:

		//
		// wait for CPU-halfcycle
		//
		WAIT_FOR_CPU_HALF_CYCLE

		DELAY_Nx3p2_CYCLES( DELAY_PHI2 )

		g = *gpioInAddr;
		A = SID_ADDRESS( g );

		if ( SID_ACCESS( g ) && READ_ACCESS( g ) && A < 0x1f )
		{
			gpio_set_dir_all_bits( 255 | gpioDir );

			// output first, then update
			D = transferReg[ A ];
			SET_DATA( D );
			disableDataLines = 1;

			stateInConfigMode = TRANSFER_MODE_CYCLES;

			switch ( A )
			{
			case 2:
				if ( transferData >= transferDataEnd )
				{
					if ( transferStage == 0 )
					{
						transferStage = 1;
						( *(uint16_t *)&transferReg[ 8 ] ) = ( *(uint16_t *)&prgCode[ 0 ] );  // transfer address
						transferData = (uint8_t *)&prgCode[ 2 ];
						transferReg[ 4 ] = *transferData;
						transferDataEnd = (uint8_t *)&( prgCode_size[ prgCode ] );
					} else
					{
						( *(uint16_t *)&transferReg[ 13 ] ) = jumpAddress = launcherAddress;
					}
				}
				break;

			case 4:
				transferReg[ 4 ] = *( ++transferData );   // next byte to be transferred
				break;

			case 9:
				( *(uint16_t *)&transferReg[ 8 ] ) ++; // increment destination address
				break;

			case 14:
				if ( jumpAddress == launcherAddress )
				{
					stateGoingTowardsTransferMode = 0;
					goto handleSIDCommunication;
				}
				break;
			}
		} 

		if ( SID_ACCESS( g ) )
			stateInConfigMode = TRANSFER_MODE_CYCLES;

		if ( --stateInConfigMode <= 0 || ( SID_ACCESS( g ) && !READ_ACCESS( g ) ) )
		{
			stateGoingTowardsTransferMode = 0;
			goto handleSIDCommunication;
		}

	}

	/*   __   __        ___    __      __        __                     __               __
		/  ` /  \ |\ | |__  | / _`    |__) |  | /__` __ |__|  /\  |\ | |  \ |    | |\ | / _`
		\__, \__/ | \| |    | \__>    |__) \__/ .__/    |  | /~~\ | \| |__/ |___ | | \| \__>
	*/
	while ( true )
	{
		//
		// wait for VIC-halfcycle
		//
	configWaitForVIC_Halfcycle:
		WAIT_FOR_VIC_HALF_CYCLE

		if ( disableDataLines )
		{
			disableDataLines = 0;
			gpio_set_dir_all_bits( gpioDir );
		}

	configWaitForCPU_Halfcycle:

		//
		// wait for CPU-halfcycle
		//
		WAIT_FOR_CPU_HALF_CYCLE
		DELAY_Nx3p2_CYCLES( DELAY_PHI2 )

		register uint32_t g2;
		g2 = g = *gpioInAddr;
		A = SID_ADDRESS( g );

		if ( SID_ACCESS( g ) )
		{
			if ( READ_ACCESS( g ) )
			{
				if ( A == 0x1d )
				{
					if ( stateConfigRegisterAccess < 65536 )
						D = config[ ( stateConfigRegisterAccess ++ ) & 63 ]; else
						if ( stateConfigRegisterAccess < 65536 + VERSION_STR_SIZE )
							D = VERSION_STR[ stateConfigRegisterAccess - 65536 ];
					stateInConfigMode = CONFIG_MODE_CYCLES;
				} else //if ( A == 0x1c )
				{
					// if bit 7 and bit 5 are different => A5-signal has changed
					// if bit 6 and bit 4 are different => A8-signal has changed
					register uint8_t t;
					D = addrLines << 4;

					t = addrLines ^ ( addrLines << 2 );
					if ( !( t & 0b01000000 ) ) // A5-signal has never changed
						addrLines &= 0b11111110;
					if ( !( t & 0b10000000 ) ) // A8/IO-signal has never changed
						addrLines &= 0b11111001;

					D |= addrLines & 15;
					stateInConfigMode = CONFIG_MODE_CYCLES;
				}

				gpio_set_dir_all_bits( 255 | gpioDir );
				SET_DATA( D );

				disableDataLines = 1;
			} else
			//if ( WRITE_ACCESS( g ) )
			{
				DELAY_Nx3p2_CYCLES( DELAY_READ_BUS )

				g = *gpioInAddr;
				D = g & 255;

				stateInConfigMode = 0;

				if ( A == 0x1e )
				{
					if ( D >= 224 )
						stateConfigRegisterAccess = 65536 - 224 + D; else
						stateConfigRegisterAccess = D * 64;
					stateInConfigMode = CONFIG_MODE_CYCLES;

					addrLines |= ( g >> A5 ) & 3;
				} else
				if ( A == 0x1d )
				{
					if ( D == 0xff )
					{
						// update settings and write to flash
						updateConfiguration();
						updateEmulationParameters();
						writeConfiguration();
						skipMeasurements = 3;
						WAIT_FOR_VIC_HALF_CYCLE
						WAIT_FOR_CPU_HALF_CYCLE
						stateInConfigMode = 0;
					} else if ( D == 0xfe )
					{
						// update settings, but DO NOT WRITE to flash
						updateConfiguration();
						updateEmulationParameters();
						skipMeasurements = 3;
						WAIT_FOR_VIC_HALF_CYCLE
						WAIT_FOR_CPU_HALF_CYCLE
						stateInConfigMode = 0;
					} else if ( D == 0xfa )
					{
						addrLines = 0b11000000;
						stateInConfigMode = CONFIG_MODE_CYCLES;
					} else
					{
						config[ ( stateConfigRegisterAccess ++ ) & 63 ] = D;
						stateInConfigMode = CONFIG_MODE_CYCLES;
					}
				}
			}
		} else
		if ( WRITE_ACCESS( g ) && A == 0x1e )
		{
			// no SID-access but "A8" low => IOx
			if ( !( g & ( 1 << A8 ) ) )
				addrLines |= 4;
		}

		addrLines &= 0b00111111 | ( ( ( g2 >> A5 ) & 3 ) << 6 );
		addrLines |= ( ( g2 >> A5 ) & 3 ) << 4;

		if ( --stateInConfigMode <= 0 )
			goto handleSIDCommunication;

	} // while ( true )
}

#define FLASH_CONFIG_OFFSET (1024 * 1024)
const uint8_t *pConfigXIP = (const uint8_t *)( XIP_BASE + FLASH_CONFIG_OFFSET );

inline uint32_t *getAddressPersistent()
{
	extern uint32_t ADDR_PERSISTENT[];
	return ADDR_PERSISTENT;
}

#define SET_CLOCK_125MHZ set_sys_clock_pll( 1500000000, 6, 2 );

#ifdef CLOCK_300MHZ
#define SET_CLOCK_FAST set_sys_clock_pll( 1500000000, 5, 1 );
#endif

void readConfiguration()
{
	memcpy( config, pConfigXIP, 64 );	

	DELAY_PHI2     = busTimings[ 1 ];
	DELAY_READ_BUS = busTimings[ 0 ];

	SET_CLOCK_FAST

	uint16_t c = crc16( config, 62 );

	if ( ( c & 255 ) != config[ 62 ] || ( c >> 8 ) != config[ 63 ] )
	{
		// load default values
		extern void setDefaultConfiguration();
		setDefaultConfiguration();
	}
}

void writeConfiguration()
{
	SET_CLOCK_125MHZ
	//sleep_ms( 2 );
	DELAY_Nx3p2_CYCLES( 85000 );
	flash_range_erase( FLASH_CONFIG_OFFSET, FLASH_SECTOR_SIZE );

	uint16_t c = crc16( config, 62 );
	config[ 62 ] = c & 255;
	config[ 63 ] = c >> 8;

	flash_range_program( FLASH_CONFIG_OFFSET, config, FLASH_PAGE_SIZE );
	SET_CLOCK_FAST
}


int main()
{
	vreg_set_voltage( VREG_VOLTAGE_1_30 );
	readConfiguration();
	initGPIOs();

	// start bus handling and emulation
	multicore_launch_core1( handleBus );
	bus_ctrl_hw->priority = BUSCTRL_BUS_PRIORITY_PROC1_BITS;
	runEmulation();

	return 0;
}

