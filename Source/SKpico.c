/*
       ______/  _____/  _____/     /   _/    /             /
     _/           /     /     /   /  _/     /   ______/   /  _/             ____/     /   ______/   ____/
      ___/       /     /     /   ___/      /   /         __/                    _/   /   /         /     /
         _/    _/    _/    _/   /  _/     /  _/         /  _/             _____/    /  _/        _/    _/
  ______/   _____/  ______/   _/    _/  _/    _____/  _/    _/          _/        _/    _____/    ____/

  SKpico.c

  SIDKick pico - SID-replacement with dual-SID/SID+fm emulation using a RPi pico, reSID 0.16 and fmopl 
  Copyright (c) 2023/2024 Carsten Dachsbacher <frenetic@dachsbacher.de>

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

// enable output via PWM
//#define OUTPUT_VIA_PWM

// enable output via PCM5102-DAC
//#define USE_DAC

// will be available later
//#define USE_SPDIF

// enable flashing LED
//#define FLASH_LED

// enable heuristics for detecting digi-playing techniques
#define SUPPORT_DIGI_DETECT

// enable support of special 8-bit DAC mode
#define SID_DAC_MODE_SUPPORT

// enable RGB LED on GPIO 23 (do not use this with original Pico)
//#define USE_RGB_LED

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
#include "pico/audio_spdif.h"
#include "launch.h"
#include "prgconfig.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "hardware/adc.h"
#include "hardware/resets.h"
#include "hardware/watchdog.h"

#include "prgslots.h"

uint8_t  prgLaunch = 0, 
		 currentPRG = 254;		// 255 = config tool, else PRG slot
uint8_t  decompressConfig = 0;
uint16_t prgCode_sizeM;

#include "fmopl.h"
extern uint8_t FM_ENABLE;

#ifdef USE_RGB_LED
#undef FLASH_LED
#include "ws2812.pio.h"
static int32_t r_ = 0, g_ = 0, b_ = 0;
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
static const __not_in_flash( "mydata" ) unsigned char VERSION_STR[ VERSION_STR_SIZE ] = {
#if defined( USE_SPDIF )
  0x53, 0x4b, 0x10, 0x09, 0x03, 0x0f, '0', '.', '2', '1', '/', 0x53, 0x50, 0x44, 0x49, 0x46, 0, 0, 0, 0,   // version string to show
#endif
#if defined( USE_DAC ) 
  0x53, 0x4b, 0x10, 0x09, 0x03, 0x0f, '0', '.', '2', '1', '/', 0x44, 0x41, 0x43, '6', '4', 0, 0, 0, 0,   // version string to show
#elif defined( OUTPUT_VIA_PWM )
  0x53, 0x4b, 0x10, 0x09, 0x03, 0x0f, '0', '.', '2', '1', '/', 0x50, 0x57, 0x4d, '6', '4', 0, 0, 0, 0,   // version string to show
#endif
  0x53, 0x4b, 0x10, 0x09, 0x03, 0x0f, 0x00, 0x00,   // signature + extension version 0
  0, 21,                                            // firmware version with stepping = 0.12
#ifdef SID_DAC_MODE_SUPPORT                         // support DAC modes? which?
  SID_DAC_MONO8 | SID_DAC_STEREO8,
#else
  0,
#endif
  0, 0, 0, 0, 0 };

extern void initReSID();
extern void resetReSID();
extern void emulateCyclesReSID( int cyclesToEmulate );
extern void emulateCyclesReSIDSingle( int cyclesToEmulate );
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
#define RESET		22
#define LED_BUILTIN 25
#define POTX		( 10 )
#define POTY		( 11 )
#define bRESET		( 1 << RESET )
#define bSID		( 1 << SID )
#define bPHI		( 1 << PHI )
#define bRW			( 1 << RW )
#define bOE			( 1 << OE_DATA )
#define bPOTX		( 1 << POTX )
#define bPOTY		( 1 << POTY )

#define AUDIO_I2S_CLOCK_PIN_BASE 26
#define AUDIO_I2S_DATA_PIN	28

#define DAC_BITS	( ( 3 << AUDIO_I2S_CLOCK_PIN_BASE ) | ( 1 << AUDIO_I2S_DATA_PIN ) )
#define bPWN_POT	( ( 1 << AUDIO_PIN ) | bPOTX | bPOTY | DAC_BITS )

#define VIC_HALF_CYCLE( g )	( !( (g) & bPHI ) )
#define CPU_HALF_CYCLE( g )	(  ( (g) & bPHI ) )
#define WRITE_ACCESS( g )	( !( (g) & bRW ) )
#define READ_ACCESS( g )	(  ( (g) & bRW ) )
#define SID_ACCESS( g )		( !( (g) & bSID ) )
#define SID_ADDRESS( g )	(  ( (g) >> A0 ) & 0x1f )
#define SID_RESET( g )	    ( !( (g) & bRESET ) )

#define WAIT_FOR_VIC_HALF_CYCLE { do { g = *gpioInAddr; } while ( !( VIC_HALF_CYCLE( g ) ) ); }
#define WAIT_FOR_CPU_HALF_CYCLE { do { g = *gpioInAddr; } while ( !( CPU_HALF_CYCLE( g ) ) ); }

#define SET_DATA( D )   \
      { sio_hw->gpio_set = ( D ); \
        sio_hw->gpio_clr = ( (~(uint32_t)( D )) & 255 ); } 

const __not_in_flash( "mydata" ) uint32_t  sidFlags[ 6 ] = { bSID, ( 1 << A5 ), ( 1 << A8 ), ( 1 << A5 ) | ( 1 << A8 ), ( 1 << A8 ), ( 1 << A8 ) };
extern uint32_t SID2_FLAG;
extern uint8_t  SID2_IOx_global;

// audio settings
#define AUDIO_RATE (44100)
#define AUDIO_VALS 2834
#define AUDIO_BITS 11
#define AUDIO_BIAS ( AUDIO_VALS / 2 )
#define SAMPLES_PER_BUFFER (256)

extern uint32_t C64_CLOCK;

#define SET_CLOCK_125MHZ set_sys_clock_pll( 1500000000, 6, 2 );
#define SET_CLOCK_FAST   set_sys_clock_pll( 1500000000, 5, 1 );

#define DELAY_Nx3p2_CYCLES( c )								\
    asm volatile( "mov  r0, %[_c]\n\t"						\
				  "1: sub  r0, r0, #1\n\t"					\
				  "bne   1b"  : : [_c] "r" (c) : "r0", "cc", "memory" );

void initGPIOs()
{
	for ( int i = 0; i < 26; i++ )
		gpio_init( i );
	gpio_init( 28 );
	gpio_set_pulls( A8, true, false );
	gpio_set_pulls( RESET, true, false );

	gpio_set_dir_all_bits( bOE | bPWN_POT | ( 1 << LED_BUILTIN ) | ( 1 << 23 ) );
}

extern uint8_t config[ 64 ];

void initPotGPIOs()
{
	if ( config[ 57 ] )
	{
		adc_init();
		adc_gpio_init( POTY + 0 );
		adc_select_input( POTY - 26 );
		adc_run( true );

		gpio_init( POTX );
		sio_hw->gpio_clr = bPOTX | bPOTY;
		gpio_set_pulls( POTX, false, false );
		gpio_set_pulls( POTY, false, false );
	} else
	{
		adc_hw->cs = adc_hw->cs & !ADC_CS_EN_BITS;
		gpio_init( POTX );
		gpio_init( POTY );
		sio_hw->gpio_clr = bPOTX | bPOTY;
		gpio_set_pulls( POTX, false, false ); 
		gpio_set_pulls( POTY, false, false ); 
	}
}


#ifdef USE_DAC

audio_buffer_pool_t *ap;

audio_buffer_pool_t *initI2S() 
{
	static audio_format_t audio_format = { .format = AUDIO_BUFFER_FORMAT_PCM_S16, .sample_freq = 44100, .channel_count = 2 };
	static audio_buffer_format_t producer_format = { .format = &audio_format, .sample_stride = 8 };
	audio_buffer_pool_t *pool = audio_new_producer_pool( &producer_format, 3, SAMPLES_PER_BUFFER ); 

	audio_i2s_config_t config = { .data_pin = AUDIO_I2S_DATA_PIN, .clock_pin_base = AUDIO_I2S_CLOCK_PIN_BASE, .dma_channel = 0, .pio_sm = 0 };
	
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
uint32_t audioBuffer[ SAMPLES_PER_BUFFER ];
uint8_t  firstOutput = 1;

#endif

#ifdef USE_SPDIF
	removed for now
#endif

uint64_t c64CycleCounter = 0;

volatile int32_t newSample = 0xffff, newLEDValue;
volatile uint64_t lastSIDEmulationCycle = 0;

uint8_t outRegisters[ 34 * 2 ];
uint8_t *outRegisters_2 = &outRegisters[ 34 ];

uint8_t fmFakeOutput = 0;
uint8_t fmAutoDetectStep = 0;
uint8_t hack_OPL_Sample_Value[ 2 ];
uint8_t hack_OPL_Sample_Enabled;

#define sidAutoDetectRegs outRegisters

#define SID_MODEL_DETECT_VALUE_8580 2
#define SID_MODEL_DETECT_VALUE_6581 3
#define REG_AUTO_DETECT_STEP		32
#define REG_MODEL_DETECT_VALUE		33

uint8_t busValue = 0;
int32_t busValueTTL = 0;

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

#define DD_TB_TIMEOUT0	135
// def 22 and 12
#define DD_TB_TIMEOUT	22
#define DD_PWM_TIMEOUT	22

uint8_t  ddActive[ 3 ];
uint64_t ddCycle[ 3 ] = { 0, 0, 0 };
uint8_t	 sampleValue[ 3 ] = { 0 };

extern void outputDigi( uint8_t voice, int32_t value );

extern uint8_t POT_FILTER_global;
uint8_t paddleFilterMode = 0;
uint8_t SID2_IOx;
uint8_t potXExtrema[ 2 ], potYExtrema[ 2 ];

volatile uint8_t doReset = 0;

void updateEmulationParameters()
{
	extern uint8_t POT_SET_PULLDOWN;
	gpio_set_pulls( POTY, false, POT_SET_PULLDOWN > 0 ); 
	gpio_set_pulls( POTX, false, POT_SET_PULLDOWN > 0 ); 

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

#define RGB24( r, g, b ) ( ( (uint32_t)(r)<<8 ) | ( (uint32_t)(g)<<16 ) | (uint32_t)(b) )

uint8_t smoothPotValues = 0;
uint8_t newPotXCandidate = 128, newPotYCandidate = 128;
uint8_t newPotXCandidate2S = 128, newPotYCandidate2S = 128;
uint8_t skipSmoothing = 0;

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
	pwm_config_set_wrap( &configLED, AUDIO_VALS );
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
	pwm_config_set_wrap( &config, AUDIO_VALS );
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
	extern int32_t voiceOutAcc[ 3 ], nSamplesAcc;
	voiceOutAcc[ 0 ] = voiceOutAcc[ 1 ] = voiceOutAcc[ 2 ] = 0;
	r_ = g_ = b_ = 0;
	#endif

	// decompress config-tool
	extern char *exo_decrunch( const char *in, char *out );
	exo_decrunch( &prgCodeCompressed[ prgCodeCompressed_size ], &prgCode[ prgCode_size ] );
	decompressConfig = 0;
	prgLaunch = 0;
	currentPRG = 254;

	initReSID();
	
	FM_OPL *pOPL = ym3812_init( 3579545, AUDIO_RATE );
	for ( int i = 0x40; i < 0x56; i++ )
	{
		ym3812_write( pOPL, 0, i );
		ym3812_write( pOPL, 1, 63 );
	}
	fmFakeOutput = 0;
	hack_OPL_Sample_Value[ 0 ] = hack_OPL_Sample_Value[ 1 ] = 64;
	hack_OPL_Sample_Enabled = 0;

	updateEmulationParameters();

	#ifdef USE_DAC  
	ap = initI2S();
	#endif
	#ifdef USE_SPDIF
	ap = initSPDIF();
	#endif
	#ifdef USE_RGB_LED
	pio_sm_put( pio0, 1, 0 );
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

	uint8_t potXHistory[ 3 ], potYHistory[ 3 ], potHistoryCnt = 0;
	int32_t paddleXSmooth = 128 << 8;
	int32_t paddleYSmooth = 128 << 8;
	int32_t paddleXRange, paddleYRange, newX, newY, oldX, oldY;

	while ( 1 )
	{

		if ( decompressConfig )
		{
			extern char *exo_decrunch( const char *in, char *out );
			exo_decrunch( &prgCodeCompressed[ prgCodeCompressed_size ], &prgCode[ prgCode_size ] );
			decompressConfig = 0;
		}

		// paddle/mouse-smoothing 
	#define EMA	6
		if ( paddleFilterMode >= 1 && smoothPotValues )
		{
			potXHistory[ potHistoryCnt ] = newPotXCandidate2S;
			potYHistory[ potHistoryCnt ] = newPotYCandidate2S;
			potHistoryCnt ++;
			if ( potHistoryCnt >= 3 )
				potHistoryCnt = 0;

			uint8_t newPotXCand = median( potXHistory );
			uint8_t newPotYCand = median( potYHistory );

			if ( paddleFilterMode == 1 )
			{
				outRegisters[ 25 ] = newPotXCand;
				outRegisters[ 26 ] = newPotYCand;
				goto bla;
			}
			if ( !skipSmoothing )
			{
				if ( paddleFilterMode == 3 )
				{
					// only for mouse, not paddles
					potXExtrema[ 0 ] = potYExtrema[ 0 ] = 128 - 58;
					potXExtrema[ 1 ] = potYExtrema[ 1 ] = 128 + 58;
					paddleXRange = (int)( potXExtrema[ 1 ] - potXExtrema[ 0 ] ) << 8;
					paddleYRange = (int)( potYExtrema[ 1 ] - potYExtrema[ 0 ] ) << 8; 

				} else
					paddleXRange = paddleYRange = 256 << 8;

				// starting from here it's the same for mouse and paddles (for the latter extrema are always [0;255])
				newX = (int)( newPotXCand - potXExtrema[ 0 ] ) << 8;
				oldX = paddleXSmooth - ( (int)potXExtrema[ 0 ] << 8 );

				newY = (int)( newPotYCand - potYExtrema[ 0 ] ) << 8;
				oldY = paddleYSmooth - ( (int)potYExtrema[ 0 ] << 8 );

				if ( ( newX - oldX ) > paddleXRange / 2 )
					oldX += paddleXRange; else
				if ( ( oldX - newX ) > paddleXRange / 2 )
					newX += paddleXRange;

				if ( ( newY - oldY ) > paddleYRange / 2 )
					oldY += paddleYRange; else
				if ( ( oldY - newY ) > paddleYRange / 2 )
					newY += paddleYRange;

				
				newX = ( oldX * ( 256 - EMA ) + newX * EMA ) >> 8;
				if ( newX >= paddleXRange ) newX -= paddleXRange;
				
				newY = ( oldY * ( 256 - EMA ) + newY * EMA ) >> 8;
				if ( newY >= paddleYRange ) newY -= paddleYRange;
			}
			if ( paddleFilterMode > 1 && !skipSmoothing )
			{
				paddleXSmooth = newX + ( (int)potXExtrema[ 0 ] << 8 );
				outRegisters[ 25 ] = paddleXSmooth >> 8;
				paddleYSmooth = newY + ( (int)potYExtrema[ 0 ] << 8 );
				outRegisters[ 26 ] = paddleYSmooth >> 8;
			}
			bla:
			smoothPotValues = 0;
		}

		if ( doReset )
		{
			watchdog_reboot( 0, 0, 0 );
		}

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


			uint64_t cmdTime = (uint64_t)ringTime[ ringRead ];

			if ( cmdTime > lastSIDEmulationCycle )
			{
				targetEmulationCycle = cmdTime;
				break;
			}
			
			register uint16_t cmd = ringBuf[ ringRead ++ ];

			if ( cmd & ( 1 << 15 ) )
			{
				if ( FM_ENABLE )
				{
					ym3812_write( pOPL, ( ( cmd >> 8 ) >> 4 ) & 1, cmd & 255 );
				} else
				{
					writeReSID2( ( cmd >> 8 ) & 0x1f, cmd & 255 );
				}
			} else
			{
				uint8_t reg = cmd >> 8;

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
			if ( FM_ENABLE )
				emulateCyclesReSIDSingle( cyclesToEmulate ); else
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
			if ( FM_ENABLE )
			{
				OPLSAMPLE fm;
				ym3812_update_one( pOPL, &fm, 1 );

				if ( hack_OPL_Sample_Enabled )
					fm = ( (uint16_t)hack_OPL_Sample_Value[ 0 ] << 5 ) + ( (uint16_t)hack_OPL_Sample_Value[ 1 ] << 5 );

				extern void outputReSIDFM( int16_t * left, int16_t * right, int32_t fm, uint8_t fmHackEnable, uint8_t *fmDigis );
				outputReSIDFM( &L, &R, (int32_t)fm, hack_OPL_Sample_Enabled, hack_OPL_Sample_Value );
			} else
				outputReSID( &L, &R );

			#if defined( USE_DAC ) 

			// fill buffer, skip/stretch as needed
			if ( audioPos < 256 )
				audioBuffer[ audioPos ] = ( ( *(uint16_t *)&R ) << 16 ) | ( *(uint16_t *)&L );

			audioPos ++;

			audio_buffer_t *buffer = take_audio_buffer( ap, false );
			if ( buffer )
			{
				int32_t discrepancy = 0;

				if ( firstOutput )
					audio_i2s_set_enabled( true );
				firstOutput = 0;

				int16_t *samples = (int16_t *)buffer->buffer->bytes;
				audioOutPos = 0;
				for ( uint i = 0; i < buffer->max_sample_count; i++ )
				{
					*(uint32_t *)&samples[ i * 2 + 0 ] = audioBuffer[ audioOutPos ];
					if ( audioOutPos < audioPos - 1 ) audioOutPos ++; else discrepancy ++;
				}

				discrepancy += (int32_t)audioOutPos - (int32_t)audioPos;

				buffer->sample_count = buffer->max_sample_count;
				give_audio_buffer( ap, buffer );
				audioOutPos = audioPos = 0;
			}

			#endif
			
			// PWM output via C64/C128 mainboard
			int32_t s_ = L + R;

			int32_t s = s_ + 65536;
			s = ( s * AUDIO_VALS ) >> 17;

			if ( abs( s - lastS ) == 0 ) {
				if ( silence < 65530 ) silence ++;
			} else
				silence = 0;
			
			lastS = s;
			
			#define RAMP_BITS 9
			#define RAMP_LENGTH ( 1 << RAMP_BITS )

			if ( silence > 16384 ) {
				if ( ramp ) ramp --;
			} else {
				if ( ramp < ( RAMP_LENGTH - 1 ) ) ramp ++;
			}
			
			if ( ramp < ( RAMP_LENGTH - 1 ) ) s = ( s * ramp ) >> RAMP_BITS;

			newSample = s;

			s = ( s_ >> ( 1 + 16 - AUDIO_BITS ) );
			if ( ramp < ( RAMP_LENGTH - 1 ) ) s = ( s * ramp ) >> RAMP_BITS;
			newLEDValue = abs( s ) << 2;
			s *= s;
			s >>= ( AUDIO_BITS - 5 );
			newLEDValue += s;

			#ifdef USE_RGB_LED
			extern int32_t voiceOutAcc[ 3 ], nSamplesAcc;

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
				r_ >>= 24;
				g_ >>= 24;
				b_ >>= 24;
				pio_sm_put( pio0, 1, RGB24( r_, g_, b_ ) << 8 );
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
extern uint8_t POT_OUTLIER_REJECTION;

uint8_t  transferStage   = 0;
uint16_t launcherAddress = ( launchCode[ 1 ] << 8 ) + launchCode[ 0 ];
uint8_t *transferData	 = (uint8_t *)&launchCode[ 2 ],
		*transferDataEnd = (uint8_t *)&launchCode[ launchSize ];
uint16_t jumpAddress     = 0xD401;
uint8_t  transferReg[ 32 ] = {
	0x78, 0x48, 0x68, 0xA9, launchCode[ 2 ], 0x48, 0x68, 0x8D, 
	launchCode[ 0 ], launchCode[ 1 ], 0x48, 0x68, 0x4C, 0x01, 0xD4 };

const uint8_t __not_in_flash( "mydata" ) jmpCode[ 3 ] = { 0x4c, 0x00, 0xd4 }; // jmp $d400

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

	fmFakeOutput = fmAutoDetectStep = 0;
	outRegisters[ REG_AUTO_DETECT_STEP ] = outRegisters[ REG_AUTO_DETECT_STEP + 34 ] = 0;
	outRegisters[ REG_MODEL_DETECT_VALUE ] = ( config[ /*CFG_SID1_TYPE*/0 ] == 0 ) ? SID_MODEL_DETECT_VALUE_6581 : SID_MODEL_DETECT_VALUE_8580;
	outRegisters[ REG_MODEL_DETECT_VALUE + 34 ] = ( config[ /*CFG_SID2_TYPE*/8 ] == 0 ) ? SID_MODEL_DETECT_VALUE_6581 : SID_MODEL_DETECT_VALUE_8580;

	register uint32_t gpioDir = bOE | bPWN_POT | ( 1 << LED_BUILTIN ), gpioDirCur = 0;
	register uint32_t g;
	register uint32_t A;
	register uint8_t  DELAY_READ_BUS_local = DELAY_READ_BUS,
		DELAY_PHI2_local = DELAY_PHI2;
	register uint8_t  D;
	volatile const uint32_t *gpioInAddr = &sio_hw->gpio_in;
	register volatile uint8_t newPotCounter = 0, disableDataLines = 0;
	register uint32_t curSample = 0;

	// variables for potentiometer handling and filtering
	uint8_t potCycleCounter = 0;
	uint8_t skipMeasurements = 0;

	potXExtrema[ 0 ] = potYExtrema[ 0 ] = 128 - 30;
	potXExtrema[ 1 ] = potYExtrema[ 1 ] = 128 + 30;

	uint16_t prgLength;
	uint8_t  *transferPayload;
	uint8_t  addrLines = 99;

	int16_t  stateInConfigMode = 0;
	uint32_t stateConfigRegisterAccess = 0;

	gpio_set_dir_all_bits( gpioDir );
	sio_hw->gpio_clr = bOE;

	SID2_IOx = SID2_IOx_global;

	prgLaunch = 0;
	currentPRG = 254;

	WAIT_FOR_CPU_HALF_CYCLE
	WAIT_FOR_VIC_HALF_CYCLE
	WAIT_FOR_CPU_HALF_CYCLE

	/*   __     __      __        __                     __               __
		/__` | |  \    |__) |  | /__` __ |__|  /\  |\ | |  \ |    | |\ | / _`
		.__/ | |__/    |__) \__/ .__/    |  | /~~\ | \| |__/ |___ | | \| \__>
	*/
handleSIDCommunication:

	
	if ( !prgLaunch && currentPRG != 255 )
	{
		decompressConfig = 1;
		currentPRG = 255;
	}

	// reinitialization of the transfer mode
	transferStage = 0;
	transferData = (uint8_t *)&launchCode[ 2 ];
	transferDataEnd = (uint8_t *)&launchCode[ launchSize ];

	transferReg[ 4 ] = launchCode[ 2 ]; // data
	( *(uint16_t *)&transferReg[ 8 ] ) = launcherAddress = *(uint16_t *)&launchCode[ 0 ];
	( *(uint16_t *)&transferReg[ 13 ] ) = jumpAddress = 0xD401;

	uint16_t _prgCode_size  = prgCode_size;

	if ( prgLaunch )
	{
		_prgCode_size = prgCode_sizeM = prgLength;
		prgLaunch = 0;
	}

	stateInConfigMode = 0;

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
			gpioDirCur = gpioDir;

			if ( config[ 57 ] )
			{
				if ( gpioDir & bPOTY )
				{
					iobank0_hw->io[ POTY ].ctrl = GPIO_FUNC_SIO << IO_BANK0_GPIO0_CTRL_FUNCSEL_LSB;
					gpio_set_dir_masked( bPOTY, 0xffffffff );
				} else
				{
					iobank0_hw->io[ POTY ].ctrl = GPIO_FUNC_NULL << IO_BANK0_GPIO0_CTRL_FUNCSEL_LSB;
					hw_clear_bits( &padsbank0_hw->io[ POTY ], PADS_BANK0_GPIO0_IE_BITS );
				}

				gpio_set_dir_masked( bPOTX, gpioDir );
			} else
			{
				gpio_set_dir_masked( bPOTX | bPOTY, gpioDir );
			}
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

		if ( busValueTTL < 0 )
		{
			busValue = 0;
		} else
			busValueTTL --;

		static uint16_t resetCnt = 0;
		if ( SID_RESET( g ) )
			resetCnt ++; else
			resetCnt = 0;

		if ( resetCnt > 100 && doReset == 0 )
		{
			doReset = 1;
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
				if ( ( g & SID2_FLAG ) && (FM_ENABLE > 1) ) 
				{
					gpio_set_dir_masked( 0xff, 0xff );
					if ( g & ( 1 << A5 ) && !( ( g >> A0 ) & 15 ) )
					{
						D = fmFakeOutput;
						fmFakeOutput = 0xc0 - fmFakeOutput;
					} else
						D = 0xff;
					SET_DATA( D );
					disableDataLines = 1;
				} else
				if ( !( g & SID2_FLAG ) || config[ 8/*CFG_SID2_TYPE*/ ] < 3 )
				{
					gpio_set_dir_masked( 0xff, 0xff );
					if ( A >= 0x1d )
					{
						D = jmpCode[ A - 0x1d ];
						stateGoingTowardsTransferMode ++;
					} else
					{
						if ( reg[ REG_AUTO_DETECT_STEP ] == 1 && ( A == 0x1b ) )
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
					SET_DATA( D );
					disableDataLines = 1;
				}
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
					if ( (g & SID2_FLAG) && FM_ENABLE )
					{
						if ( (g & ( 1 << A5 )) && !( ( g >> A0 ) & 15 ) )
						{

							static uint8_t mOPL_addr = 0;
							if ( ( A & 16 ) == 0 )
							{
								mOPL_addr = D;
							} else
							{
								if ( mOPL_addr == 1 )
								{
									if ( D == 4 )
										hack_OPL_Sample_Enabled = 128;  else
										hack_OPL_Sample_Enabled = 0;
								}
								if ( hack_OPL_Sample_Enabled && ( mOPL_addr == 0xa0 || mOPL_addr == 0xa1 ) ) // digi hack
								{
									hack_OPL_Sample_Enabled |= 1 << ( mOPL_addr - 0xa0 );
									hack_OPL_Sample_Value[ mOPL_addr - 0xa0 ] = D;
								} else
								{
									hack_OPL_Sample_Value[ 0 ] = hack_OPL_Sample_Value[ 1 ] = 0;
								}
							}

							SID_CMD = ( A << 8 ) | D | ( 1 << 15 );
							ringTime[ ringWrite ] = (uint64_t)c64CycleCounter;
							ringBuf[ ringWrite ++ ] = SID_CMD;
						}

						if ( ( g & ( 1 << ( A0 + 4 ) ) ) == 0 && D == 0x04 )
							fmAutoDetectStep = 1;
						if ( ( g & ( 1 << ( A0 + 4 ) ) ) > 0 && D == 0x60 && fmAutoDetectStep == 1 )
							fmAutoDetectStep = 2;
						if ( ( g & ( 1 << ( A0 + 4 ) ) ) == 0 && D == 0x04 && fmAutoDetectStep == 2 )
							fmAutoDetectStep = 3;
						if ( ( g & ( 1 << ( A0 + 4 ) ) ) > 0 && D == 0x80 && fmAutoDetectStep == 3 )
						{
							fmAutoDetectStep = 4;
							fmFakeOutput = 0;
						}

					} else
					{
						SID_CMD = ( A << 8 ) | D;
						if ( g & SID2_FLAG ) SID_CMD |= 1 << 15;

						ringTime[ ringWrite ] = (uint64_t)c64CycleCounter;
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
				}
				disableDataLines = 1;
				busValue = D;
				//if ( outRegisters[ REG_MODEL_DETECT_VALUE ] == SID_MODEL_DETECT_VALUE_8580 )
					//busValueTTL = 0xa2000; else
					//busValueTTL = 0x1d00;

				// fixed large value avoids artifacts in some old tunes
				busValueTTL = 0x100000;
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

					#define max( a, b ) ( (a)>(b)?(a):(b) )
					#define min( a, b ) ( (a)<(b)?(a):(b) )
					#ifdef DIAGROM_HACK
						if ( DIAGROM_THRESHOLD >= 80 )
						{
							if ( abs( newPotXCandidate - newPotYCandidate ) < 10 && newPotXCandidate >= 50 && newPotXCandidate < DIAGROM_THRESHOLD )
							{
								if ( presumablyFixedResistor < 40000 )
									presumablyFixedResistor ++;
							} else
							{
								if ( presumablyFixedResistor )
									presumablyFixedResistor --;
							}
							if ( presumablyFixedResistor > 1000 )
								diagROM_PaddleOffset = min( DIAGROM_THRESHOLD - newPotXCandidate, ( presumablyFixedResistor - 1000 ) / 1000 ); else
								diagROM_PaddleOffset = 0;
							newPotXCandidate = min( 255, (int)newPotXCandidate + (int)diagROM_PaddleOffset );
							newPotYCandidate = min( 255, (int)newPotYCandidate + (int)diagROM_PaddleOffset );
						} else
						if ( DIAGROM_THRESHOLD > 1 )
						{
							newPotXCandidate = min( 255, (int)newPotXCandidate + (int)DIAGROM_THRESHOLD );
							newPotYCandidate = min( 255, (int)newPotYCandidate + (int)DIAGROM_THRESHOLD );
						}
					#endif

					if ( !paddleFilterMode )
					{
						outRegisters[ 25 ] = newPotXCandidate;
						outRegisters[ 26 ] = newPotYCandidate;
					} else
					if ( !smoothPotValues )
					{
						newPotXCandidate2S = newPotXCandidate;
						newPotYCandidate2S = newPotYCandidate;
						smoothPotValues = 1;
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
			if ( ( newPotCounter & 1 ) && ( ( g & bPOTX ) || potCycleCounter == 255 ) )
			{
				newPotXCandidate = potCycleCounter;
				newPotCounter &= 0b110;
			}

			uint8_t potYState = 0;

			if ( config[ 57 ] )
				potYState = ( ( newPotCounter & 2 ) && ( ( (uint16_t)adc_hw->result > 1024 + config[ 57 ] * 64 ) || potCycleCounter == 255 ) ); else
				potYState = ( ( newPotCounter & 2 ) && ( ( g & bPOTY ) || potCycleCounter == 255 ) );

			if ( potYState )
			{
				newPotYCandidate = potCycleCounter;
				newPotCounter &= 0b101;
			} else

			// test validity of measurements
			if ( POT_OUTLIER_REJECTION )
			{
				uint8_t potYState = 0;

				if ( config[ 57 ] )
					potYState = ( !( newPotCounter & 2 ) && !( (uint16_t)adc_hw->result > 1024 + config[ 57 ] * 64 - 256 ) ); else
					potYState = ( !( newPotCounter & 2 ) && !( ( g & bPOTY ) ) );

				if ( ( !( newPotCounter & 1 ) && !( g & bPOTX ) && potCycleCounter == ( ( newPotXCandidate + 255 ) >> 1 ) ) ||
					 ( potYState && potCycleCounter == ( ( newPotYCandidate + 255 ) >> 1 ) ) )
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
			DELAY_Nx3p2_CYCLES( 3 )
			disableDataLines = 0;
			gpio_set_dir_masked( 0xff, 0 );
		}

	transferWaitForCPU_Halfcycle:

		//
		// wait for CPU-halfcycle
		//
		WAIT_FOR_CPU_HALF_CYCLE

		DELAY_Nx3p2_CYCLES( DELAY_PHI2_local )

		g = *gpioInAddr;
		A = SID_ADDRESS( g );

		if ( SID_ACCESS( g ) && READ_ACCESS( g ) && A <= 14 )
		{
			gpio_set_dir_masked( 0xff, 0xff );

			// output first, then update
			SET_DATA( transferReg[ A ] );
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
						transferDataEnd = (uint8_t *)&( _prgCode_size[ prgCode ] );
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
	uint8_t busTimingTestValue = 0;
	uint8_t transferPRGSlot = 0;
	while ( true )
	{
		//
		// wait for VIC-halfcycle
		//
	configWaitForVIC_Halfcycle:
		WAIT_FOR_VIC_HALF_CYCLE

		if ( disableDataLines )
		{
			DELAY_Nx3p2_CYCLES( 3 )
			disableDataLines = 0;
			gpio_set_dir_masked( 0xff, 0 );
		}

	configWaitForCPU_Halfcycle:

		//
		// wait for CPU-halfcycle
		//
		WAIT_FOR_CPU_HALF_CYCLE
		DELAY_Nx3p2_CYCLES( DELAY_PHI2_local )

		register uint32_t g2, gA5A6A8;
		g = *gpioInAddr;
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
				} else
				if ( A == 0x1c )
				{
					D = *( transferPayload ++ );
					stateInConfigMode = CONFIG_MODE_CYCLES;
				} else
				{
					// check if signal levels at A5, A6 and/or A8/IO have changed
					// trying to avoid problems due to unconnected/swapped wires
					// distinguish between A8 (active HIGH) and IO (active LOW)
					if ( A == 0 )
					{
						D = g2;
						g2 = g2 >> 8 | g >> 8 << 24;
						addrLines = ( D | ( ( D ^ 0b01000000 ) >> 4 ) ) & 15;
						A = (int32_t)( ( ( (uint32_t)D >> 6 ) << addrLines ) << 31 ) >> 31;
						g2 = g2 ^ ( ( g2 ^ ( ( gA5A6A8 & ~255 ) | ( ( gA5A6A8 >> A5 | gA5A6A8 >> A8 ) & 6 ) ) ) & A );
					} else
					{
						uint8_t t = addrLines;
						D = t << 4;
						t ^= ( t << 2 );
						addrLines &= 0b11111000 | ( ( t >> 6 ) & 1 ) | ( (uint8_t)( (int8_t)t >> 1 ) >> 5 );
						D |= addrLines & 15;
					}

					stateInConfigMode = CONFIG_MODE_CYCLES;
				}

				gpio_set_dir_masked( 0xff, 0xff );
				SET_DATA( D );

				disableDataLines = 1;
			} else
			//if ( WRITE_ACCESS( g ) )
			{
				DELAY_Nx3p2_CYCLES( DELAY_READ_BUS_local )

				g2 = g = *gpioInAddr;
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
					if ( D >= 0xfe )
					{
						// update settings and write / do not write to flash
						updateConfiguration();
						initPotGPIOs();
						updateEmulationParameters();
						if ( D == 0xff ) writeConfiguration();
						skipMeasurements = 3;
						WAIT_FOR_VIC_HALF_CYCLE
						WAIT_FOR_CPU_HALF_CYCLE
						stateInConfigMode = 0;
					} else if ( D == 0xfa )
					{
						addrLines = 0b11000000;
						stateInConfigMode = CONFIG_MODE_CYCLES;
					} else
					if ( D == 0xfb )
					{
						doReset = 0;
						stateInConfigMode = 0;
					} else
					{
						config[ ( stateConfigRegisterAccess ++ ) & 63 ] = D;
						stateInConfigMode = CONFIG_MODE_CYCLES;
					}
				} else
				if ( A == 0x1c )
				{
					transferPayload = prgDirectory;
					stateInConfigMode = CONFIG_MODE_CYCLES;
				} else
				if ( A == 0x1b )
				{
					prgLaunch = 1;
					stateInConfigMode = 0;
				} else
				if ( A == 0x14 || A == 0x15 ) // start bus timing banging: value #1 and #2
				{
					transferPRGSlot = 254 - 0x14 + A;
					transferPayload = prgCode;
					stateInConfigMode = CONFIG_MODE_CYCLES;
				} else
				if ( A == 0x1a ) // start PRG upload
				{
					transferPRGSlot = D;
					transferPayload = prgCode;
					stateInConfigMode = CONFIG_MODE_CYCLES;
				} else
				if ( A == 0x19 ) // set PRG upload page
				{
					transferPayload = prgCode + D * 256;
					stateInConfigMode = CONFIG_MODE_CYCLES;
				} else
				if ( A == 0x16 ) // upload one PRG byte
				{
					*transferPayload = D;
					transferPayload ++;
					stateInConfigMode = CONFIG_MODE_CYCLES;
				} else
				if ( A == 0x17 ) // end PRG upload, or end of bus timing banging!
				{
					SET_CLOCK_125MHZ
					DELAY_Nx3p2_CYCLES( 85000 );

					if ( transferPRGSlot >= 254 )
					{
						int *histo = (int*)&prgCode[ 16384 ];
						memset( histo, 0, 256 * sizeof( int ) );
						int sz = transferPayload - prgCode;
						for ( int i = 0; i < sz; i++ )
							histo[ prgCode[ i ] ] ++;
						int maxV = histo[ 0 ], maxIdx = 0;
						for ( int i = 1; i < sz; i++ )
							if ( histo[ i ] > maxV )
							{
								maxV = histo[ i ];
								maxIdx = i;
							}

						if ( transferPRGSlot == 254 )
						{
							DELAY_READ_BUS = maxIdx; 
						}else
						{
							DELAY_PHI2     = maxIdx;

							unsigned char *tmp = &prgCode[ 16384 + 1024 ];
							{
								#define FLASH_BUSTIMING_OFFSET ((uint32_t)&busTimings[ 0 ] - XIP_BASE)
								memcpy( tmp, (void*)FLASH_BUSTIMING_OFFSET, FLASH_SECTOR_SIZE );
								DELAY_READ_BUS_local = tmp[ 0 ] = DELAY_READ_BUS;
								DELAY_PHI2_local     = tmp[ 1 ] = DELAY_PHI2;

								flash_range_erase  ( FLASH_BUSTIMING_OFFSET, FLASH_SECTOR_SIZE );
								flash_range_program( FLASH_BUSTIMING_OFFSET, tmp, FLASH_SECTOR_SIZE );
							}
						}
					} else
					{
						int sz = transferPayload - prgCode - 18;	// -18 because menu entry is 18 byte (string null-terminated)

						uint8_t *dirEntry = &prgDirectory[ transferPRGSlot * 24 ];

						memcpy( dirEntry, prgCode + sz, 18 );
						dirEntry[ 18 ] = 0;
						dirEntry[ 19 ] = 0;
						dirEntry[ 20 ] = transferPRGSlot;
						dirEntry[ 21 ] = 0;
						dirEntry[ 22 ] = sz & 255;
						dirEntry[ 23 ] = sz >> 8;

						#define FLASH_DIR_OFFSET ((uint32_t)&prgDirectory_Flash[ 0 ] - XIP_BASE)
						flash_range_erase  ( FLASH_DIR_OFFSET, FLASH_SECTOR_SIZE );
						flash_range_program( FLASH_DIR_OFFSET, prgDirectory, FLASH_SECTOR_SIZE );

						prgCode[ 0 ] = 1;
						prgCode[ 1 ] = 8;

						#define FLASH_PRG_OFFSET ((uint32_t)&prgRepository[ transferPRGSlot * 65536 ] - XIP_BASE)
						flash_range_erase  ( FLASH_PRG_OFFSET, 65536 );
						flash_range_program( FLASH_PRG_OFFSET, prgCode, 65536 );
					}
					SET_CLOCK_FAST
					prgLaunch = 0;
					currentPRG = 254;
					stateInConfigMode = 0;
				} else
				if ( A == 0x10 )
				{
					SET_CLOCK_125MHZ
					DELAY_Nx3p2_CYCLES( 85000 );
					const uint8_t *dirEntry = &prgDirectory[ D * 24 ];
					uint32_t ofs = ( dirEntry[ 20 ] * 256 + dirEntry[ 19 ] ) * 256 + dirEntry[ 18 ];
					prgLength = *( (uint16_t *)&dirEntry[ 22 ] );
					memcpy( prgCode, &prgRepository[ ofs ], prgLength );
					SET_CLOCK_FAST
					prgLaunch = 1;
					currentPRG = 0;
					stateInConfigMode = 0;
				}
			}
		} else
		if ( WRITE_ACCESS( g ) && A == 0x1e )
		{
			// no SID-access but "A8" low => IOx
			if ( !( g & ( 1 << A8 ) ) )
				addrLines |= 4;
		}

		// for checking if signal levels at A5, A6 and/or A8/IO have changed
		// trying to avoid problems due to unconnected/swapped wires
		gA5A6A8 = gpioDir;
		addrLines &= 0b00111111 | ( ( ( g >> A5 ) & 3 ) << 6 );
		addrLines |= ( ( g >> A5 ) & 3 ) << 4;

		if ( --stateInConfigMode <= 0 )
			goto handleSIDCommunication;

	} // while ( true )
}

const uint8_t __in_flash( "section_config" ) __attribute__( ( aligned( FLASH_SECTOR_SIZE ) ) ) flashCFG[ 4096 ];

#define FLASH_CONFIG_OFFSET ((uint32_t)flashCFG - XIP_BASE)
const uint8_t *pConfigXIP = (const uint8_t *)flashCFG;

void readConfiguration()
{
	memcpy( prgDirectory, prgDirectory_Flash, 16 * 24 );

	memcpy( config, pConfigXIP, 64 );	

	DELAY_READ_BUS = busTimings[ 0 ];
	DELAY_PHI2     = busTimings[ 1 ];

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

	SET_CLOCK_125MHZ
	DELAY_Nx3p2_CYCLES( 85000 );
	readConfiguration();
}



int main()
{
	vreg_set_voltage( VREG_VOLTAGE_1_30 );
	readConfiguration();
	initGPIOs();
	initPotGPIOs();

	// start bus handling and emulation
	multicore_launch_core1( handleBus );
	bus_ctrl_hw->priority = BUSCTRL_BUS_PRIORITY_PROC1_BITS;

	runEmulation();
	return 0;
}
