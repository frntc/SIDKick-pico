/*
       ______/  _____/  _____/     /   _/    /             /
     _/           /     /     /   /  _/     /   ______/   /  _/             ____/     /   ______/   ____/
      ___/       /     /     /   ___/      /   /         __/                    _/   /   /         /     /
         _/    _/    _/    _/   /  _/     /  _/         /  _/             _____/    /  _/        _/    _/
  ______/   _____/  ______/   _/    _/  _/    _____/  _/    _/          _/        _/    _____/    ____/

  reSIDWrapper.h

  SIDKick pico - SID-replacement with dual-SID/SID+fm emulation using a RPi pico, reSID 0.16 and fmopl 
  Copyright (c) 2023-2025 Carsten Dachsbacher <frenetic@dachsbacher.de>

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

#define CFG_CHECHSUM1           62
#define CFG_CHECHSUM2           63
#define CFG_CHECHSUM3           52
#define CFG_CHECHSUM4           53

#define CFG_CUSTOM_TIMING_READBUS 54
#define CFG_CUSTOM_TIMING_PHI2    55
#define CFG_CUSTOM_USE_TIMINGS    56

#define CFG_PADDLEOFFSET        39

#define CFG_FILTER_8580_LOW     40
#define CFG_FILTER_8580_CENTER  41

#define CFG_FILTER_6581_PRESET  42
#define CFG_FILTER_6581_LOW     43
#define CFG_FILTER_6581_HIGH    44
#define CFG_FILTER_6581_DISTORTION    45

#define CFG_FILTER_EXT_ENABLE   46
#define CFG_FILTER_EXT_HIGHPASS 47
#define CFG_FILTER_EXT_LOWPASS  48



#define SID_MODEL_DETECT_VALUE_8580 2
#define SID_MODEL_DETECT_VALUE_6581 3
#define REG_AUTO_DETECT_STEP		32
#define REG_MODEL_DETECT_VALUE		33
