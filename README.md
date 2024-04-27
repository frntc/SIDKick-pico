  
<p align="center" font-size: 30px;>
  <img src="Images/SKpico_logo.png" height="120"> <br>
  <b> .- the inexpensive dual-SID replacement that you can build yourself -. </b><br><br>
</p>

The SIDKick pico ("SKpico") is a drop-in replacement for the SID 6581/8580 sound chips in the Commodore 64 and 128 computers. 
It has been designed as an inexpensive alternative to other replacements while not making compromises regarding quality. 
It consists of a simple interface board and a Raspberry Pi Pico (or compatible clone).
The emulation is based on an extended version of reSID 0.16, and includes a few additional features:

-	dual 6581 and/or 8580 emulation based on reSID 0.16 (optional: extension for digi-playing techniques)
-	2nd-SID address at $d400, $d420, $d500, $d420 + $d500 simultaneously, $de00, $df00 (on C128 no $d500), or any address when an external chip select signal is used (e.g. on Ultimate 64 boards)
-	paddle/mouse support
-	built-in configuration menu (launch with "SYS 54301"/"SYS 54333", also from C128-mode)
-	sound output via PWM (mono) through the C64/C128 mainboard, or in stereo using a PCM5102A-DAC-board (recommended)

<p align="center" font-size: 30px;>
<img src="Images/SKpico_ex1.jpg" height="180">
<img src="Images/step0.jpg" height="180">
<img src="Images/SKpico_menu.jpg" height="180">  
</p>
<br />

You can listen to the SIDKick pico in two videos by emulaThor: <br>
<p align="center" font-size: 30px;>
<a href="http://www.youtube.com/watch?v=3Wam-UXWBd0"><img src="http://img.youtube.com/vi/3Wam-UXWBd0/0.jpg" height="180">
<a href="http://www.youtube.com/watch?v=aWAfkrH59jg"><img src="http://img.youtube.com/vi/aWAfkrH59jg/0.jpg" height="180">
</p>
 

## How to build a SIDKick pico

This section summarizes building and setting up the hardware. 

### PCB ordering

You can order the PCBs from PCBWay without or with SMD-parts preassembled: [here](https://www.pcbway.com/project/shareproject/W160781ASB18_Gerber_1790f9c8.html).

You can find my [other projects](https://www.pcbway.com/project/member/?bmbno=B5CDD8BE-199B-47) there. In case you don't have an account at PCBWay yet: [register via this link](https://pcbway.com/g/x1UjP0) and get "$5 of New User Free Credit".

Even simpler: [Restore Store](https://restore-store.de) (not my shop) will offer pre-assembled SKpicos at fair prices soon.

There's a good chance that there will be more official resellers in the future. 
Please do not buy from those who knowingly violate the license and sell overpriced SKpicos out of greed (see [Hall of Shame](#license-hall-of-shame)).


### Building / Soldering

The first step when building the SKpico is soldering the surface-mount components. These are located on the **bottom side** of the PCB. 
Please see the BOM and assembly information [here](https://htmlpreview.github.io/?https://github.com/frntc/SIDKick-pico/blob/master/BOM/ibom.html).

The next step is to solder the pin header and sockets  which works best if you follow these steps:
- solder the SID-socket pin header **with 14 pins** in the middle of the PCB
- solder a socket for the Pico or the Pico directly (in case you solder directly with castellated edges: make sure to cut the pin tips and put insulation tape on them!)
- solder the SID-socket pin header close to the edge of the PCB. It has only **10 pins**, the connector Vcc-GND-CK-DIN-BCK is for the optional DAC!
- no pins are required for the 3 connections at the center-bottom of the PCB!
- optional: solder the pin headers for the DAC (blue) and additional address/signal lines (red)
 
<p align="center" font-size: 30px;>

<img src="Images/step0.jpg" height="120">   
<img src="Images/step1.jpg" height="120">   
<img src="Images/step2.jpg" height="120">   
<img src="Images/step3.jpg" height="120">   
</p>


<br />

## Installing a SIDKick pico

Pay attention to *correctly orient and insert* the RPi Pico and the SKpico (see backside of PCB for markings) into the SID-socket of your C64 or C128. Note that in a C128D you might need to remove one support bolt of the power supply to fit the SKpico. 

You can choose to emulate a single SID only. If you want to use a second SID you need to connect additional cables to get the signals to the SKpico as they are not available at the SID socket:

### Installing additional cables in C64
| SKpico pin  | C64 (see images for alternative locations) |
|----------|:-------------|
| A5 | CPU Pin 12 (required for $d420) | 
| A8/IO | CPU Pin 15 (required for $d500) <br/> OR expansion port pin 7 or 10 for $de00/$df00 addresses <br/> OR external chip select-signal |

### Installing additional cables in C128
| SKpico pin  | C128 |
|----------|:-------------|
| A5 |  CPU Pin 12 (required for $d420) | 
| A8/IO |  $d500 is not supported for the C128 <br/> connect to expansion port pin 7 or 10 for $de00/$df00 <br/> OR external chip select-signal |

The photographs show various (other) locations where these signals can be tapped, e.g. A5 to A8 and IOx are conveniently available on some mainboards (see photo of ASSY 250469), A5/A8 at the ROM-chips (not shown on the photos: on the 250469 at the kernal ROM 251913 at pin 5 and 29, for example).

<p align="center" font-size: 30px;>
<img src="Images/SKpico_326.jpg" height="160"> 
<img src="Images/SKpico_469.jpg" height="160">  
<img src="Images/SKpico_expport.jpg" height="160">  
</p>

The built-in configuration tool autodetects and displays which cables have been (properly) installed and shows only possible SID-addresses. 

**Hint:** before attaching/detaching signal cables, set the 2nd-SID address to $d400 in the config tool.

<br>

### Audio Output
<img align="right" height="160" src="Images/DAC_0.jpg">
<img align="right" height="160" src="Images/DAC_1.jpg">

The PWM sound is output via the SID-socket. If you use the optional DAC you can directly get the stereo audio signal from the audio jack or connectors on the PCB. 

**Hint:** You can also connect the DAC-output to the video-audio-connector of the C64/C128. Pin 3 is the standard audio output, pin 7 is often used for the second audio channel. If you do so: use the firmware which *does not output via PWM*.

Note, which of the two output options is active depends on the firmware that you use (there are options for PWM-only and PWM+DAC simultaneously). PWM quality is slightly better when the DAC output is not enabled.

The order of pins to connect the DAC to the SKpico is easy to determine: the order is given once you match Vin which corresponds to Vcc at the SKpico and GND to GND.

<br>

### Powering the SKpico

The SKpico is powered from the C64/C128-mainboard, DO NOT power from USB.

<br>
  


## Firmware Uploading

The SKpico-PCBs do not need to be programmed in any way. Only the RPi Pico needs to be flashed with the pre-built binaries (available in the release package). 

The procedure is simple: press the 'Boot'-button on the RPi pico, connect to your PC and it shows up as USB-drive. Then simply copy the firmware (.uf2) to this drive.

**IMPORTANT**: DO NOT connect the RPi Pico to USB while it is plugged into your C64/C128! (but it's no problem if the Pico is soldered to the SKpico-PCB)

 
<br>


### Sidekick64-/RAD Expansion Unit-InterOp
If you're using [Sidekick64](https://github.com/frntc/Sidekick64) or [RAD Expansion Unit](https://github.com/frntc/RAD) then you should update to their latest firmwares.


<br />
  

## Configuration-Tool

The built-in configuration tool allows you to choose the emulated SID-types, digi boost settings, volume and panning and is hopefully mostly self-explanatory.

The mouse/paddle-settings ("POT X/Y") deserve a bit of explanation: as one goal was to keep the interfacing circuitry simple, you might need to adjust some settings for the SKpico to work (best) with your mouse or paddles. Once you move the cursor to the potentiometer settings, a preview of the  values and movement is shown. You can now tune the configuration:
- if a mouse moves only horizontally or not at all then choose the "level" option (by pressing 'V'). This option does not work with paddles!
- if your mouse shows some weird jumps (e.g. values of 00 or FF) try the "outlier" option (key 'O'). There are two intensities: normal and aggressive outlier removal.
- additional filters can reduce the inherent jittering of mice/paddles on the C64/C128: "median" is a simple yet good outlier rejection for remaining jitter. "smooth" uses a exponential weighted average (it comes in versions for paddles and mice).

**NOTE**: potentiometer filtering modes do not work with two paddles/mice used simultaneously.

If you choose 'reSID+digi detect' as emulation option, then the SKpico uses heuristics to detect modern digi playing techniques (such as that used in [Vicious Sid](https://codebase64.org/doku.php?id=base:vicious_sid_demo_routine_explained)) which are not handled by the reSID 0.16 emulation. These techniques, when detected successfully, are emulated with special code paths. The heuristics are  based on the findings by Jürgen Wothke used in [WebSid](https://bitbucket.org/wothke/websid/src/master/).

**To avoid bus conflicts** when you use cartridges operating in the IO1/2 address spaces, make sure you do not use the IO1/2 addresses for the SKpico as well. 

<br />
  
## Firmware Building (if you want to):

The firmware can be build using the Raspberry Pi Pico SDK and there's nothing special worth mentioning.

<br />
  
 
## Disclaimer

Be careful not to damage your RPi Pico, PC, or Commodore, or anything attached to it. I am not responsible if you or your hardware gets damaged. Note that the RPi Pico gets overclocked. If you don't know what you're doing, better don't... use everything at your own risk.

<br />
  
## License

My portions of the source code are work licensed under GPLv3 (except otherwise stated in a source file).

The PCB is work licensed under a Creative Commons Attribution-NonCommercial-NoDerivs 4.0 (CC BY-NC-ND 4.0) International License. 

CC BY-NC-ND 4.0 also means: selling for profit on ebay (e.g. indicated by pricing), or running a shop and offering hardware under such license (no matter on which platform) is a violation of the license -- claiming 'service for the community', 'for those who can't solder themselves', or 'offer at cost price' to sell related services does not comply with the license. It is, of course, absolutely fine to order a small batch (e.g. 10 units) and sell surplus units to friends.

<br />

## License Hall of Shame

I have fun creating and improving the SKpico and my other projects. I'm happy when people share surplus boards at their own cost or make built SKpico available to others at low cost (I expect prices well below the official sellers). Unfortunately there are people seriously decreasing my fun factor by violating the CC BY-NC-ND 4.0-license for the hardware part. 

From now on I will list the license violations I'm aware of publicly (only public information, e.g. usernames will be used). 

Please welcome these sellers to the Hall of Shame:
<table>
<tr>
<td>  <img  height="150"  src="Images/hos_1.jpg">  </td>
<td>35 Euros? Obviously no commercial intend...
</tr>
<tr>
<td>  <img  height="150"  src="Images/hos_2.jpg">  </td>
<td>40 Euros? Seriously?
</tr>
<tr>
<td>  <img  height="150"  src="Images/hos_3.jpg">  </td>
<td>27.50 Euros for a pre-assembled PCB kit with almost zero own work invested?
</tr>
</table>

<br />

## Acknowledgements

Last but not least I would like to thank a few people: Dag Lem for reSID, discussions and insights; emulaThor, bigby, TurboMicha, quadflyer8 and others on forum64.de for testing and feedback/bug reports; androSID for discussions and lots of information on electronics; [Retrofan](https://compidiaries.wordpress.com/) for designing the SIDKick-logo and his font which is used in the configuration tool; Flex/Artline Designs for letting me use his music in the config tool. Magnus Lind for releasing the Exo(de-)cruncher which is used in the firmware. Jürgen Wothke for releasing WebSid.

Thanks for reading until the very end. I'd be happy to hear from you if you decide to build your own SKpico!

## Trademarks

Raspberry Pi is a trademark of Raspberry Pi Ltd.
