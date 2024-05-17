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

// please note that modifications have been made to this source code 
// for the use in the SIDKick pico firmware!

#ifndef __VOICE_H__
#define __VOICE_H__

#include "siddefs.h"
#include "wave.h"
#include "envelope.h"

class Voice
{
public:
  Voice();

  void set_chip_model(chip_model model);
  void set_sync_source(Voice*);
  void reset();

  void writeCONTROL_REG(reg8);

  // Amplitude modulated waveform output.
  // Range [-2048*255, 2047*255].
  RESID_INLINE sound_sample output();
  RESID_INLINE sound_sample output( int w );

protected:
  WaveformGenerator wave;
  EnvelopeGenerator envelope;

  // Waveform D/A zero level.
  sound_sample wave_zero;

  // Multiplying D/A DC offset.
  sound_sample voice_DC;

int freezedEnvelope;

friend class SID16;
};


// ----------------------------------------------------------------------------
// Inline functions.
// The following function is defined inline because it is called every
// time a sample is calculated.
// ----------------------------------------------------------------------------

#if RESID_INLINING || defined(RESID_VOICE_CC)

// ----------------------------------------------------------------------------
// Amplitude modulated waveform output (20 bits).
// Ideal range [-2048*255, 2047*255].
// ----------------------------------------------------------------------------

// The output for a voice is produced by a multiplying DAC, where the
// waveform output modulates the envelope output.
//
// As noted by Bob Yannes: "The 8-bit output of the Envelope Generator was then
// sent to the Multiplying D/A converter to modulate the amplitude of the
// selected Oscillator Waveform (to be technically accurate, actually the
// waveform was modulating the output of the Envelope Generator, but the result
// is the same)".
//
//          7   6   5   4   3   2   1   0   VGND
//          |   |   |   |   |   |   |   |     |   Missing
//         2R  2R  2R  2R  2R  2R  2R  2R    2R   termination
//          |   |   |   |   |   |   |   |     |
//          --R---R---R---R---R---R---R--   ---
//          |          _____
//        __|__     __|__   |
//        -----     =====   |
//        |   |     |   |   |
// 12V ---     -----     ------- GND
//               |
//              vout
//
// Bit on:  wout (see figure in wave.h)
// Bit off: 5V (VGND)
//
// As is the case with all MOS 6581 DACs, the termination to (virtual) ground
// at bit 0 is missing. The MOS 8580 has correct termination.
//

RESID_INLINE
int Voice::output()
{
	if ( wave.floating_output_ttl >= 0x14000 - 64 )
		freezedEnvelope = envelope.output();

	// Multiply oscillator output with envelope output.
	return ( wave.output() - wave_zero ) * envelope.output() + voice_DC;
}

RESID_INLINE
sound_sample Voice::output( int w )
{
  // Multiply oscillator output with envelope output.
  //return w << 4;
//  return ( w - wave_zero)*freezedEnvelope + voice_DC;
	return ( ( w >> 4 ) ) * envelope.output();
//	return ( ( w >> 4 ) ) * freezedEnvelope;
}

#endif // RESID_INLINING || defined(__VOICE_CC__)

#endif // not __VOICE_H__
