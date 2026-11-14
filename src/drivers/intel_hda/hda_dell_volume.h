#pragma once
#include <stdint.h>

namespace brook {
// Master volume for the Dell XPS 13 9343 speaker path (Realtek ALC3263/ALC288,
// subsystem 1028:0665). The volume control is a linear PCM amplitude percent
// (brook_audio_control.h): 100% = unity, 50% = -6.02 dB. On this codec the
// level is split between the DAC output amplifier (NID 0x03) and a software
// residual so the PCM is never amplified above unity:
//   total = DAC(step) * residual,  residual in (0.944, 1].
//
// DAC amplifier capabilities read back on the Dell and in the Linux codec dump
// (build/hardware/dell-linux-audio-live-20261101): ofs=0x7f, nsteps=0x7f,
// stepsize=0x01, i.e. 0.5 dB per step with 0 dB at step 127 and no mute.
// The old fixed step 80 (Linux's mixer value in that live session, not its
// maximum) is therefore -23.5 dB. Linux's "Speaker Playback Volume" (driven by
// vmaster "Master") at 100% writes step 127 = 0 dB; the speaker pin 0x14 has
// only a mute switch and EAPD (already set by firmware), no gain.
//
// Safety cap: never write above HdaDellDacMaxStep. It equals what Linux uses
// at 100%; lower it here (not elsewhere) if a quieter ceiling is wanted.
inline constexpr uint8_t HdaDellDacZeroDbStep=127;
inline constexpr uint8_t HdaDellDacMaxStep=127;
inline constexpr uint8_t HdaDellDacInitialStep=80; // configured while muted, before the first unmute
static_assert(HdaDellDacMaxStep<=HdaDellDacZeroDbStep,"never exceed 0 dB (Linux 100%)");

// Generated: down=floor(-40*log10(v/100)) half-dB steps below the cap,
// step=cap-down, residualQ15=round(32768*(v/100)/10^(-down/40)).
// test_hda_dell_volume recomputes both tables with libm.
inline constexpr uint8_t HdaDellVolumeStepTable[101]={
    0,47,60,67,72,75,79,81,84,86,87,89,91,92,93,95,96,97,98,99,
    100,100,101,102,103,103,104,105,105,106,107,107,108,108,109,109,110,110,111,111,
    112,112,112,113,113,114,114,114,115,115,115,116,116,116,117,117,117,118,118,118,
    119,119,119,119,120,120,120,121,121,121,121,122,122,122,122,123,123,123,123,123,
    124,124,124,124,124,125,125,125,125,125,126,126,126,126,126,127,127,127,127,127,
    127};
inline constexpr uint16_t HdaDellVolumeResidualQ15[101]={
    0,32768,31008,31086,31082,32690,31160,32400,31156,31239,32768,32125,31234,31944,32477,31013,31230,31326,31313,31204,
    31008,32559,32201,31782,31308,32613,32020,31392,32554,31831,31086,32123,31304,32282,31400,32323,31387,32259,31278,32101,
    31082,31859,32636,31544,32278,31165,31857,32550,31383,32037,32690,31479,32096,32713,31466,32049,32632,31356,31906,32457,
    31160,31680,32199,32718,31378,31869,32359,31012,31475,31937,32400,31025,31462,31899,32336,30939,31352,31764,32177,32589,
    31156,31545,31935,32324,32714,31251,31619,31987,32354,32722,31239,31586,31933,32280,32627,31130,31457,31785,32113,32440,
    32768};

struct HdaDellVolumeSplit {uint8_t dacStep=0;uint16_t residualQ15=0;};
// percent is clamped to 0..100. 0 (or mute) is handled by the pin mute, not
// by the DAC; the residual is then 0 too.
inline HdaDellVolumeSplit HdaDellVolumeFor(unsigned percent){
    if(percent>100)percent=100;
    const unsigned shift=HdaDellDacZeroDbStep-HdaDellDacMaxStep;
    unsigned step=HdaDellVolumeStepTable[percent];
    step=step>shift?step-shift:0;
    return {uint8_t(step),HdaDellVolumeResidualQ15[percent]};
}
}
