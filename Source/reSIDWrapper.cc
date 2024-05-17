/*
       ______/  _____/  _____/     /   _/    /             /
     _/           /     /     /   /  _/     /   ______/   /  _/             ____/     /   ______/   ____/
      ___/       /     /     /   ___/      /   /         __/                    _/   /   /         /     /
         _/    _/    _/    _/   /  _/     /  _/         /  _/             _____/    /  _/        _/    _/
  ______/   _____/  ______/   _/    _/  _/    _____/  _/    _/          _/        _/    _____/    ____/

  reSIDWrapper.cc

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

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include "pico/stdlib.h"
#include <pico/multicore.h>

#include "reSID16/sid.h"

#include "reSID_LUT.h"

// 6581, 8580, 8580+digiboost, none
#define CFG_SID1_TYPE           0
// 0 .. 15
#define CFG_SID1_DIGIBOOST      1
// 0 .. 14
#define CFG_SID1_VOLUME         3

#define CFG_SID2_TYPE           8
#define CFG_SID2_DIGIBOOST      9
#define CFG_SID2_ADDRESS        10
#define CFG_SID2_VOLUME         11

// 0 .. 14
#define CFG_SID_PANNING         12
#define CFG_SID_BALANCE         58

#define CFG_REGISTER_READ       2
#define CFG_TRIGGER             57
#define CFG_CLOCKSPEED          59
#define CFG_POT_FILTER          60
#define CFG_DIGIDETECT          61

static int32_t cfgVolSID1_Left, cfgVolSID1_Right;
static int32_t cfgVolSID2_Left, cfgVolSID2_Right;
static int32_t actVolSID1_Left, actVolSID1_Right;
static int32_t actVolSID2_Left, actVolSID2_Right;

uint32_t C64_CLOCK = 985248;
uint8_t  SID_DIGI_DETECT = 0;
uint32_t SID2_FLAG = 0; 
uint8_t  SID2_IOx_global = 0;
uint8_t  FM_ENABLE = 0;
uint8_t  POT_FILTER_global = 0, POT_SET_PULLDOWN = 0;
uint8_t  POT_OUTLIER_REJECTION = 0;
uint32_t SID2_ADDR_PREV = 255;
uint8_t  config[ 64 ];

#ifdef USE_RGB_LED
int32_t  voiceOutAcc[ 3 ], nSamplesAcc;
#endif

SID16 *sid16;
SID16 *sid16b;

extern "C"
{
    static const __not_in_flash( "mydata" ) unsigned char colorMap[ 9 ][ 3 ] =
	{
	    {  64, 153, 255 },
	    {  35, 195, 228 },
	    {  25, 227, 185 },
	    {  67, 247, 135 },
	    { 132, 255,  81 },
	    { 183, 247,  53 },
	    { 223, 223,  55 },
	    { 249, 188,  57 },
	    { 254, 144,  41 },
	};

    uint16_t crc16( const uint8_t *p, uint8_t l ) 
    {
        uint8_t x;
        uint16_t crc = 0xFFFF;

        while ( l-- ) 
        {
            x = crc >> 8 ^ *p++;
            x ^= x >> 4;
            crc = ( crc << 8 ) ^ ( (uint16_t)( x << 12 ) ) ^ ( (uint16_t)( x << 5 ) ) ^ ( (uint16_t)x );
        }
        return crc;
    }

    void setDefaultConfiguration()
    {
        for ( uint8_t i = 0; i < 62; i++ )
            config[ i ] = 0;

        config[ CFG_SID1_TYPE ] = 1;
        config[ CFG_SID2_TYPE ] = 3;
        config[ CFG_REGISTER_READ ] = 1;
        config[ CFG_SID2_ADDRESS ] = 0 + 4*0;
        config[ CFG_SID1_DIGIBOOST ] = 12;
        config[ CFG_SID2_DIGIBOOST ] = 12;
        config[ CFG_SID1_VOLUME ] = 14;
        config[ CFG_SID2_VOLUME ] = 14;
        config[ CFG_SID_PANNING ] = 5;
        config[ CFG_SID_BALANCE ] = 7;
        config[ CFG_CLOCKSPEED ] = 0;
        config[ CFG_POT_FILTER ] = 16;      // obvious outlier-rejection
        config[ CFG_DIGIDETECT ] = 0;
        config[ CFG_TRIGGER ] = 0;

        uint16_t c = crc16( config, 62 );
        config[ 62 ] = ( c & 255 );
        config[ 63 ] = ( c >> 8 );
    }

    void updateConfiguration()
    {
        if ( config[ CFG_SID1_TYPE ] == 0 )
            sid16->set_chip_model( MOS6581 ); else
            sid16->set_chip_model( MOS8580 );

        const uint32_t c64clock[ 3 ] = { 985248, 1022727, 1023440 };

        if ( config[ CFG_SID1_TYPE ] == 2 )
            sid16->input( - ( 1 << config[ CFG_SID1_DIGIBOOST ] ) ); else
            sid16->input( 0 );

        if ( config[ CFG_SID2_TYPE ] == 0 )
            sid16b->set_chip_model( MOS6581 ); else
            sid16b->set_chip_model( MOS8580 );

        if ( config[ CFG_SID2_TYPE ] == 2 )
            sid16b->input( - ( 1 << config[ CFG_SID2_DIGIBOOST ] ) ); else
            sid16b->input( 0 );


        C64_CLOCK = c64clock[ config[ CFG_CLOCKSPEED ] % 3 ];
        sid16->set_sampling_parameters( C64_CLOCK, SAMPLE_INTERPOLATE, 44100 );
        sid16b->set_sampling_parameters( C64_CLOCK, SAMPLE_INTERPOLATE, 44100 );

        extern const uint32_t sidFlags[ 6 ];
        SID2_FLAG = sidFlags[ config[ CFG_SID2_ADDRESS ] % 6 ];
        SID2_IOx_global = config[ CFG_SID2_ADDRESS ] >= 4 ? 1 : 0; 

        if ( SID2_FLAG == 0 && config[ CFG_SID2_TYPE ] != 3 ) // $d400 && SID #2 != none?
        {
            SID2_FLAG = ( 1 << 31 );
            SID2_IOx_global = 0;
        }

        if ( config[ CFG_SID2_TYPE ] >= 4 ) // FM
            FM_ENABLE = 6 - config[ CFG_SID2_TYPE ]; else
            FM_ENABLE = 0;

        if ( config[ CFG_SID2_ADDRESS ] != SID2_ADDR_PREV )
            sid16b->reset();

        SID2_ADDR_PREV = config[ CFG_SID2_ADDRESS ];

        POT_FILTER_global = config[ CFG_POT_FILTER ];

        POT_OUTLIER_REJECTION = ( POT_FILTER_global >> 4 ) & 3;
        POT_SET_PULLDOWN = POT_FILTER_global & 64;

        POT_FILTER_global &= 15;
        
        uint8_t panning = config[ CFG_SID_PANNING ];
        
        // only one SID? => center audio
        if ( config[ CFG_SID2_TYPE ] == 3 )
            panning = 7;

        cfgVolSID1_Left = (int)( config[ CFG_SID1_VOLUME ] ) * (int)( 14 - panning );
        cfgVolSID1_Right = (int)( config[ CFG_SID1_VOLUME ] ) * (int)( panning );

        if ( config[ CFG_SID2_TYPE ] == 3 )
        {
            cfgVolSID2_Left = cfgVolSID2_Right = 0;
        } else
        {
            cfgVolSID2_Left = (int)( config[ CFG_SID2_VOLUME ] ) * (int)( panning );
            cfgVolSID2_Right = (int)( config[ CFG_SID2_VOLUME ] ) * (int)( 14 - panning );
        }

        actVolSID1_Left = cfgVolSID1_Left;
        actVolSID1_Right = cfgVolSID1_Right;
        actVolSID2_Left = cfgVolSID2_Left;
        actVolSID2_Right = cfgVolSID2_Right;

        {
            const int32_t maxVolFactor = 14 * 15;
            const int32_t globalVolume = 256;
            int32_t balanceLeft, balanceRight;
            balanceLeft = balanceRight = 256;
            if ( config[ CFG_SID_BALANCE ] < 7 )
                balanceRight -= (int)( 7 - config[ CFG_SID_BALANCE ] ) * 32;
            if ( config[ CFG_SID_BALANCE ] > 7 )
                balanceLeft -= (int)( config[ CFG_SID_BALANCE ] - 7 ) * 32;
            actVolSID1_Left = actVolSID1_Left * balanceLeft * globalVolume / maxVolFactor;
            actVolSID1_Right = actVolSID1_Right * balanceRight * globalVolume / maxVolFactor;
            actVolSID2_Left = actVolSID2_Left * balanceLeft * globalVolume / maxVolFactor;
            actVolSID2_Right = actVolSID2_Right * balanceRight * globalVolume / maxVolFactor;
        }

        SID_DIGI_DETECT = config[ CFG_DIGIDETECT ] ? 1 : 0;

        extern void resetEverything();
        resetEverything();
    }

    void initReSID()
    {
    	extern char *exo_decrunch( const char *in, char *out );
	    exo_decrunch( (const char*)&reSID_LUTs_exo[ reSID_LUTs_exo_size ], (char*)&reSID_LUTs[32768] );

        sid16 = new SID16();
        sid16->set_chip_model( MOS8580 );
        sid16->reset();
        sid16->set_sampling_parameters( C64_CLOCK, SAMPLE_INTERPOLATE, 44100 );

        sid16b = new SID16();
        sid16b->set_chip_model( MOS8580 );
        sid16b->reset();
        sid16b->set_sampling_parameters( C64_CLOCK, SAMPLE_INTERPOLATE, 44100 );

        updateConfiguration();

        #ifdef USE_RGB_LED
        voiceOutAcc[ 0 ] = 
        voiceOutAcc[ 1 ] = 
        voiceOutAcc[ 2 ] = 0;
        nSamplesAcc = 0;
        #endif
    }

    void emulateCyclesReSID( int cyclesToEmulate )
    {
        sid16->clock( cyclesToEmulate );
        sid16b->clock( cyclesToEmulate );
    }

    void emulateCyclesReSIDSingle( int cyclesToEmulate )
    {
        sid16->clock( cyclesToEmulate );
    }

    void writeReSID( uint8_t A, uint8_t D )
    {
        sid16->write( A, D );
    }

    void writeReSID2( uint8_t A, uint8_t D )
    {
        sid16b->write( A, D );
    }

    void outputDigi( uint8_t voice, int32_t value )
    {
        sid16->forceDigiOutput( voice, value );
    }

    void outputReSID( int16_t * left, int16_t * right )
    {
        int32_t sid1 = sid16->output(),
                sid2 = sid16b->output();

        int32_t L = sid1 * actVolSID1_Left + sid2 * actVolSID2_Left;
        int32_t R = sid1 * actVolSID1_Right + sid2 * actVolSID2_Right;

        *left = L >> 16;
        *right = R >> 16;

        #ifdef USE_RGB_LED
        // SID #1 voices map to red, green, blue
        voiceOutAcc[ 0 ] = sid16->voiceOut[ 0 ];
        voiceOutAcc[ 1 ] = sid16->voiceOut[ 1 ];
        voiceOutAcc[ 2 ] = sid16->voiceOut[ 2 ];
        // SID #2 voices map to orange, cyan, purple
        voiceOutAcc[ 0 ] += ( 3 * sid16b->voiceOut[ 0 ] ) >> 2;
        voiceOutAcc[ 1 ] += sid16b->voiceOut[ 0 ] >> 2;
        voiceOutAcc[ 1 ] += sid16b->voiceOut[ 1 ] >> 1;
        voiceOutAcc[ 2 ] += sid16b->voiceOut[ 1 ] >> 1;
        voiceOutAcc[ 2 ] += sid16b->voiceOut[ 2 ] >> 1;
        voiceOutAcc[ 0 ] += sid16b->voiceOut[ 2 ] >> 1;
        nSamplesAcc ++;
        #endif
    }

    void outputReSIDFM( int16_t *left, int16_t *right, int32_t fm, uint8_t fmHackEnable, uint8_t *fmDigis )
    {
        int32_t sid1 = sid16->output();

        int32_t L = sid1 * actVolSID1_Left + fm * actVolSID2_Left;
        int32_t R = sid1 * actVolSID1_Right + fm * actVolSID2_Right;

        *left = L >> 16;
        *right = R >> 16;

    #ifdef USE_RGB_LED
        // SID #1 voices map to red, green, blue
        voiceOutAcc[ 0 ] = sid16->voiceOut[ 0 ];
        voiceOutAcc[ 1 ] = sid16->voiceOut[ 1 ];
        voiceOutAcc[ 2 ] = sid16->voiceOut[ 2 ];

        // FM voices map to colors as defined in colorMap
        if ( fmHackEnable )
        {
            //voiceOutAcc[ 0 ] = voiceOutAcc[ 1 ] = voiceOutAcc[ 2 ] = (fm-2048) << 6;
            if ( fmHackEnable & 2 )
            {
                voiceOutAcc[ 0 ] >>= 1;
                voiceOutAcc[ 1 ] >>= 1;
                voiceOutAcc[ 2 ] >>= 1;
                voiceOutAcc[ 0 ] += ( colorMap[ 8 ][ 0 ] * ( fmDigis[ 1 ] - 64 ) << 11 ) >> 7;
                voiceOutAcc[ 1 ] += ( colorMap[ 8 ][ 1 ] * ( fmDigis[ 1 ] - 64 ) << 11 ) >> 7;
                voiceOutAcc[ 2 ] += ( colorMap[ 8 ][ 2 ] * ( fmDigis[ 1 ] - 64 ) << 11 ) >> 7;
            }
            if ( fmHackEnable & 1 )
            {
                voiceOutAcc[ 0 ] += ( colorMap[ 1 ][ 0 ] * ( fmDigis[ 0 ] - 64 ) << 11 ) >> 7;
                voiceOutAcc[ 1 ] += ( colorMap[ 1 ][ 1 ] * ( fmDigis[ 0 ] - 64 ) << 11 ) >> 7;
                voiceOutAcc[ 2 ] += ( colorMap[ 1 ][ 2 ] * ( fmDigis[ 0 ] - 64 ) << 11 ) >> 7;
            }
        } else
            for ( int i = 0; i < 9; i++ )
            {
                extern int32_t outputCh[ 9 ];
                voiceOutAcc[ 0 ] += ( colorMap[ i ][ 0 ] * outputCh[ i ] ) >> 2;
                voiceOutAcc[ 1 ] += ( colorMap[ i ][ 1 ] * outputCh[ i ] ) >> 2;
                voiceOutAcc[ 2 ] += ( colorMap[ i ][ 2 ] * outputCh[ i ] ) >> 2;
            }
        nSamplesAcc ++;
    #endif
    }

    void resetReSID()
    {
        sid16->reset();
        sid16b->reset();
    }

    void readRegs( uint8_t * p1, uint8_t * p2 )
    {
        sid16->readRegisters( p1 );
        sid16b->readRegisters( p2 );
    }

    uint8_t readSID( uint8_t offset )
    {
        return sid16->read( offset );
    }

    uint8_t readSID2( uint8_t offset )
    {
        return sid16b->read( offset );
    }

}