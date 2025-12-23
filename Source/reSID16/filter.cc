//  ---------------------------------------------------------------------------
//  This file is part of reSID, a MOS6581 SID emulator engine.
//  Copyright (C) 2004  Dag Lem <resid@nimrod.no>
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation; either version 2 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program; if not, write to the Free Software
//  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
//  ---------------------------------------------------------------------------

#define __FILTER_CC__
#include "filter.h"

#include "pico/stdlib.h" // int64_t
#include <string.h>
#include <math.h>

// Maximum cutoff frequency is specified as
// FCmax = 2.6e-5/C = 2.6e-5/2200e-12 = 11818.
//
// Measurements indicate a cutoff frequency range of approximately
// 220Hz - 18kHz on a MOS6581 fitted with 470pF capacitors. The function
// mapping FC to cutoff frequency has the shape of the tanh function, with
// a discontinuity at FCHI = 0x80.
// In contrast, the MOS8580 almost perfectly corresponds with the
// specification of a linear mapping from 30Hz to 12kHz.
// 
// The mappings have been measured by feeding the SID with an external
// signal since the chip itself is incapable of generating waveforms of
// higher fundamental frequency than 4kHz. It is best to use the bandpass
// output at full resonance to pick out the cutoff frequency at any given
// FC setting.
//
// The mapping function is specified with spline interpolation points and
// the function values are retrieved via table lookup.
//
// NB! Cutoff frequency characteristics may vary, we have modeled two
// particular Commodore 64s.

fc_point Filter::f0_points_6581[] =
{
  //  FC      f         FCHI FCLO
  // ----------------------------
  {    0,   220 },   // 0x00      - repeated end point
  {    0,   220 },   // 0x00
  {  128,   230 },   // 0x10
  {  256,   250 },   // 0x20
  {  384,   300 },   // 0x30
  {  512,   420 },   // 0x40
  {  640,   780 },   // 0x50
  {  768,  1600 },   // 0x60
  {  832,  2300 },   // 0x68
  {  896,  3200 },   // 0x70
  {  960,  4300 },   // 0x78
  {  992,  5000 },   // 0x7c
  { 1008,  5400 },   // 0x7e
  { 1016,  5700 },   // 0x7f
  { 1023,  6000 },   // 0x7f 0x07
  { 1023,  6000 },   // 0x7f 0x07 - discontinuity
  { 1024,  4600 },   // 0x80      -
  { 1024,  4600 },   // 0x80
  { 1032,  4800 },   // 0x81
  { 1056,  5300 },   // 0x84
  { 1088,  6000 },   // 0x88
  { 1120,  6600 },   // 0x8c
  { 1152,  7200 },   // 0x90
  { 1280,  9500 },   // 0xa0
  { 1408, 12000 },   // 0xb0
  { 1536, 14500 },   // 0xc0
  { 1664, 16000 },   // 0xd0
  { 1792, 17100 },   // 0xe0
  { 1920, 17700 },   // 0xf0
  { 2047, 18000 },   // 0xff 0x07
  { 2047, 18000 }    // 0xff 0x07 - repeated end point
};

fc_point Filter::f0_points_8580[] =
{
  //  FC      f         FCHI FCLO
  // ----------------------------
  {    0,     0 },   // 0x00      - repeated end point
  {    0,     0 },   // 0x00
  {  128,   800 },   // 0x10
  {  256,  1600 },   // 0x20
  {  384,  2500 },   // 0x30
  {  512,  3300 },   // 0x40
  {  640,  4100 },   // 0x50
  {  768,  4800 },   // 0x60
  {  896,  5600 },   // 0x70
  { 1024,  6500 },   // 0x80
  { 1152,  7500 },   // 0x90
  { 1280,  8400 },   // 0xa0
  { 1408,  9200 },   // 0xb0
  { 1536,  9800 },   // 0xc0
  { 1664, 10500 },   // 0xd0
  { 1792, 11000 },   // 0xe0
  { 1920, 11700 },   // 0xf0
  { 2047, 12500 },   // 0xff 0x07
  { 2047, 12500 }    // 0xff 0x07 - repeated end point
};

static bool f0Initialized = false;

sound_sample Filter::f0_6581[2048];
//sound_sample Filter::f0_6581_reSID[2048];
sound_sample Filter::f0_8580[2048];
sound_sample Filter::f0_8580_reSID[2048];
//sound_sample Filter::f0_6581_DAC[2048];
//sound_sample Filter__f0_8580[2048]; // todo

#if 0
static float approximate_dac( int x )
{
    float dac = 0.966f;
    float value = 0.0f;
    int bit = 1;
    float weight = 1.0f;
    const float dir = 2 * 0.966f;
    for ( int i = 0; i < 11; i ++ )
    {
        if ( x & bit )
            value += weight;
        bit <<= 1;
        weight *= dir;
    }
    return value / ( weight / dac / dac ) * ( 1 << 11 );
};
#endif

// ----------------------------------------------------------------------------
// Constructor.
// ----------------------------------------------------------------------------
Filter::Filter()
{
  fc = 0;

  res = 0;

  filt = 0;

  voice3off = 0;

  hp_bp_lp = 0;

  vol = 0;

  // State of filter.
  Vhp = 0;
  Vbp = 0;
  Vlp = 0;
  Vnf = 0;

  //enable_filter(true);

  if ( !f0Initialized )
  {
    // Create mappings from FC to cutoff frequency.
    interpolate(f0_points_6581, f0_points_6581
	        + sizeof(f0_points_6581)/sizeof(*f0_points_6581) - 1,
	        PointPlotter<sound_sample>(f0_6581), 1.0);

    interpolate(f0_points_8580, f0_points_8580
	      + sizeof(f0_points_8580)/sizeof(*f0_points_8580) - 1,
	      PointPlotter<sound_sample>(f0_8580), 1.0);

    memcpy( f0_8580_reSID, f0_8580, sizeof( sound_sample ) * 2048 );

    f0Initialized = true;
  }

  set_chip_model(MOS6581);
}

void Filter::set8580FilterCoeffs( int low, int center )
{
  int high = ( center - low ) * 2 + low;
  for ( int i = 0; i < 2048; i++ )
  {
    int64_t v = (int64_t)f0_8580_reSID[ i ] * (int64_t)high / (int64_t)12500 + low;
    f0_8580[ i ] = v;
  }
  set_w0();
  set_Q();
}

// please refer to Antti S. Lankila's original measurements!
const float filterMeasurements6581[][ 7 ] = {
    { 0.0f, 0.0f, 0.0f, 0.0f, 220, 18000, 3 },
    //"R2 20/83",   [FilterNata6581R3_2083]
    { 1646009.8408527481f, 30099093.879106432f, 1.0054673476018452f, 14227.912103009101, 216, 23318, 4 },
    //"R3 03/84",   [ FilterZrX6581R3_0384 ]
    { 1.9e6f, 8.5e7f, 1.0058f, 2e4, 182, 16611, 6 },
    //"R3 07/84",   [ FilterTrurl6581R3_0784 ]
    { 1.3e6f, 3.7e8f, 1.0066f, 1.8e4, 261, 18547, 6 },
    //"R3 19/84",   [ FilterZrX6581R3_1984 ]
    { 1.83e6f, 2.6e9f, 1.0064f, 2.5e4, 185, 11299, 8 },
    //"R3 33/84",   [ FilterTrurl6581R3_3384 ]
    { 1.16e6f, 9.9e6f, 1.0058f, 1.48e4f, 326, 23063, 3 },
    //"R3 36/84",   [ FilterZrx6581R3_3684 ]
    { 1.65e6f, 1.2e10f, 1.006f, 1e4f, 205, 5261, 9 },
    //"R3 39/84 (1)",   [ FilterAlankila6581R3_3984_1 ]
    { 1791818.499169074f, 35564657.77812113f, 1.0051916772894813f, 16797.553721476484f, 198, 19348, 5 },
    //"R3 39/84 (2)",   [ FilterAlankila6581R3_3984_2 ]
    { 1.3e6f, 5.5e7f, 1.0057f, 1.3e4, 266, 25378, 4 },
    //"R3 39/85",   [ FilterZrx6581R3_3985 ]
    { 1.5e6f, 1.8e8f, 1.0065f, 1.8e4, 227, 18721, 5 },
    //"R3 42/85",   [ FilterLordNightmare6581R3_4285 ]
    { 1.55e6f, 2.5e8f, 1.006f, 1.9e4f, 219, 16993, 6 },
    //"R3 44/85",   [ FilterLordNightmare6581R3_4485 ]
    { 1319746.6334817847f, 325484805.5228794f, 1.0049967859070095f, 8865.676531378584f, 257, 16533, 7 },
    //"R3 48/85",   [ FilterTrurl6581R3_4885 ]
    { 840577.4520801408f, 1909158.8633669745f, 1.0068865662510837f, 14858.140079688419f, 578, 23191, 1 },
    //"R3 04/86",   [ FilterTrurl6581R3_0486S ]
    { 1164920.4999651583f, 12915042.165290257f, 1.0058853753357735f, 12914.5661141159f, 316, 26354, 2 },
    //"R4 19/86",   [ FilterLordNightmare6581R4_1986S ]
    { 1326141.6346824165f, 2059653512.4261823f, 1.0055788469632323f, 7591.487542582334f, 255, 11301, 8 },
    //"R4AR 22/86", [ FilterZrx6581R4AR_2286 ]
    { 1.3e6f, 1.9e8f, 1.0066f, 1.8e4f, 262, 18799, 5 },
    //"R4AR 44/86", [ FilterTrurl6581R4AR_4486 ]
    { 1.1e6f, 8e6f, 1.0052f, 1.7e4f, 350, 20002, 4 },
    //"R4AR 34/88 (A)", [ FilterAlankila6581R4AR_3789 ]
    { 1299501.5675945764f, 284015710.29875594f, 1.0065089724604026f, 18741.324073610594f, 261, 17879, 6 },
    //"R4AR 37/89 (T)", [ FilterTrurl6581R4AR_3789 ] # C = CSG 6581R4AR 3789 14   HONG KONG HH342116 HC - 30
    { 1.08e6f*0+ 1.48e6, 1.8e6f*0+3.0e6f, 1.006f, 1.3e4f+0*(1.0e4f), 341, 34147*0+26248, 0 },
    //"R4AR 34/88 (G)", [ FilterGrue6581R4AR_3488 ]
    { 1.45e6f, 1.75e8f, 1.0055f, 1e4f*0+1.06e4f, 235, 26480+0*27760, 6 },
};

float Filter::evalType3( float br, float o, float s, float mfr, int x )
{
    const float cap = 470e-12;
    const float sc = 2.0f * 3.1415926535897932f * cap;
    float kink = f0_6581_DAC[ x ] / 65536.0f; 
    float dynamic = mfr + o / powf( s, kink );
    float resistance = ( br * dynamic ) / ( br + dynamic );
    return 1.0f / ( sc * resistance );
}

//#include "../filterLUTs.h"

void Filter::set6581FilterCoeffs( const signed short *preset, int minFreq, int maxFreq, int distortion )
{
    int rangeS = maxFreq - minFreq;
    for ( int x = 0; x < 2048; x++ )
    {
      int v = preset[ x ];
      v = ( v * rangeS ) / 32768;
      v += minFreq;
      f0_6581[ x ] = v;
    }
    distortionStrength = distortion;

    set_w0();
    set_Q();
}

// ----------------------------------------------------------------------------
// Enable filter.
// ----------------------------------------------------------------------------
/*void Filter::enable_filter(bool enable)
{
  enabled = enable;
}*/


// ----------------------------------------------------------------------------
// Set chip model.
// ----------------------------------------------------------------------------

void Filter::set_chip_model(chip_model model)
{
  chipModel = model;

  if (model == MOS6581) {
    // The mixer has a small input DC offset. This is found as follows:
    //
    // The "zero" output level of the mixer measured on the SID audio
    // output pin is 5.50V at zero volume, and 5.44 at full
    // volume. This yields a DC offset of (5.44V - 5.50V) = -0.06V.
    //
    // The DC offset is thus -0.06V/1.05V ~ -1/18 of the dynamic range
    // of one voice. See voice.cc for measurement of the dynamic
    // range.

    mixer_DC = -0xfff*0xff/18 >> 7;

    f0 = f0_6581;
    f0_points = f0_points_6581;
    f0_count = sizeof(f0_points_6581)/sizeof(*f0_points_6581);
  }
  else {
    // No DC offsets in the MOS8580.
    mixer_DC = 0;

    f0 = f0_8580;
    f0_points = f0_points_8580;
    f0_count = sizeof(f0_points_8580)/sizeof(*f0_points_8580);
  }

  set_w0();
  set_Q();
}


// ----------------------------------------------------------------------------
// SID reset.
// ----------------------------------------------------------------------------
void Filter::reset()
{
  fc = 0;

  res = 0;

  filt = 0;

  voice3off = 0;

  hp_bp_lp = 0;

  vol = 0;

  // State of filter.
  Vhp = 0;
  Vbp = 0;
  Vlp = 0;
  Vnf = 0;

  set_w0();
  set_Q();
}


// ----------------------------------------------------------------------------
// Register functions.
// ----------------------------------------------------------------------------
void Filter::writeFC_LO(reg8 fc_lo)
{
  fc = (fc & 0x7f8) | (fc_lo & 0x007);
  set_w0();
}

void Filter::writeFC_HI(reg8 fc_hi)
{
  fc = ((fc_hi << 3) & 0x7f8) | (fc & 0x007);
  set_w0();
}

void Filter::writeRES_FILT(reg8 res_filt)
{
  res = (res_filt >> 4) & 0x0f;
  set_Q();

  filt = res_filt & 0x0f;
}

void Filter::writeMODE_VOL(reg8 mode_vol)
{
  voice3off = mode_vol & 0x80;

  hp_bp_lp = (mode_vol >> 4) & 0x07;

  vol = mode_vol & 0x0f;
}

// Set filter cutoff frequency.
void Filter::set_w0()
{
  const double pi = 3.1415926535897932385;
  // CD
  const int64_t c_w0 = 65536.0*2.0*pi* 1.048576;
  w0 = static_cast<sound_sample>( ( (int64_t)f0[fc] * c_w0 ) >> 16 );

  // Multiply with 1.048576 to facilitate division by 1 000 000 by right-
  // shifting 20 times (2 ^ 20 = 1048576).
  //w0 = static_cast<sound_sample>(2*pi*f0[fc]*1.048576);

  // Limit f0 to 16kHz to keep 1 cycle filter stable.
  const sound_sample w0_max_1 = static_cast<sound_sample>(2*pi*16000*1.048576);
  w0_ceil_1 = w0 <= w0_max_1 ? w0 : w0_max_1;

  // Limit f0 to 4kHz to keep delta_t cycle filter stable.
  const sound_sample w0_max_dt = static_cast<sound_sample>(2*pi*4000*1.048576);
  w0_ceil_dt = w0 <= w0_max_dt ? w0 : w0_max_dt;
}

// Set filter resonance.
void Filter::set_Q()
{
  // Q is controlled linearly by res. Q has approximate range [0.707, 1.7].
  // As resonance is increased, the filter must be clocked more often to keep
  // stable.

  // The coefficient 1024 is dispensed of later by right-shifting 10 times
  // (2 ^ 10 = 1024).
  //_1024_div_Q = static_cast<sound_sample>(1024.0/(0.707 + 1.0*res/0x0f));

  /* TODO
  
  float Q = res / 15.f;
  if (model == MOS6581)
      _1_div_Q = 1.f / (0.707f + Q * 1.2f);
  if (model == MOS8580)
      _1_div_Q = 1.f / (0.707f + Q * 1.6f);
  
  */

  _1024_div_Q = ( 10240000 / ( 7070 + 10000 * res / 0x0f ) );
}

// ----------------------------------------------------------------------------
// Spline functions.
// ----------------------------------------------------------------------------

// ----------------------------------------------------------------------------
// Return the array of spline interpolation points used to map the FC register
// to filter cutoff frequency.
// ----------------------------------------------------------------------------
void Filter::fc_default(const fc_point*& points, int& count)
{
  points = f0_points;
  count = f0_count;
}

// ----------------------------------------------------------------------------
// Given an array of interpolation points p with n points, the following
// statement will specify a new FC mapping:
//   interpolate(p, p + n - 1, filter.fc_plotter(), 1.0);
// Note that the x range of the interpolation points *must* be [0, 2047],
// and that additional end points *must* be present since the end points
// are not interpolated.
// ----------------------------------------------------------------------------
PointPlotter<sound_sample> Filter::fc_plotter()
{
  return PointPlotter<sound_sample>(f0);
}
