/*
 * Copyright (C) 2012 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "rfcomm.h"
#include "a2dp.h"
#include "log.h"
#include "sdp.h"
#include "l2cap.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>


//endpoint media types
	#define AVDTP_MEDIA_TYP_AUDIO		0
	#define AVDTP_MEDIA_TYP_VIDEO		1
	#define AVDTP_MEDIA_TYP_MULTIMEDIA	2

//andpoint directions
	#define AVDTP_DIR_SOURCE		0
	#define AVDTP_DIR_SINK			1


//A2DP packet header (2bytes)
	//byte 0
	#define AVDTP_HDR_MASK_TRANS	0xF0
	#define AVDTP_HDR_SHIFT_TRANS	4
	#define AVDTP_HDR_MASK_PKT_TYP	0x0C
	#define AVDTP_HDR_SHIFT_PKT_TYP	2
	#define AVDTP_HDR_MASK_MSG_TYP	0x03
	#define AVDTP_HDR_SHIFT_MSG_TYP	0
	//byte 1
	#define AVDTP_HDR_MASK_SIG_ID	0x3F
	#define AVDTP_HDR_SHIFT_SIG_ID	0

//sigId
	#define AVDTP_SIG_DISCOVER			0x01
	#define AVDTP_SIG_GET_CAPABILITIES		0x02
	#define AVDTP_SIG_SET_CONFIGURATION		0x03
	#define AVDTP_SIG_GET_CONFIGURATION		0x04
	#define AVDTP_SIG_RECONFIGURE			0x05
	#define AVDTP_SIG_OPEN				0x06
	#define AVDTP_SIG_START				0x07
	#define AVDTP_SIG_CLOSE				0x08
	#define AVDTP_SIG_SUSPEND			0x09
	#define AVDTP_SIG_ABORT				0x0A
	#define AVDTP_SIG_SECURITY_CONTROL		0x0B

//pktTyp
	#define AVDTP_PKT_TYP_SINGLE			0x00
	#define AVDTP_PKT_TYP_START			0x01
	#define AVDTP_PKT_TYP_CONTINUE			0x02
	#define AVDTP_PKT_TYP_END			0x03

//msgTyp
	#define AVDTP_MSG_TYP_CMD			0x00
	#define AVDTP_MSG_TYP_ACCEPT			0x02
	#define AVDTP_MSG_TYP_REJ			0x03

//seid info (2 bytes)
	//byte0
	#define AVDTP_SEID_NFO_MASK_SEID		0xFC
	#define AVDTP_SEID_NFO_SHIFT_SEID		2
	#define AVDTP_SEID_NFO_MASK_INUSE		0x02
	#define AVDTP_SEID_NFO_SHIFT_INUSE		1
	//byte1
	#define AVDTP_SEID_NFO_MASK_MEDIA_TYP		0xF0
	#define AVDTP_SEID_NFO_SHIFT_MEDIA_TYP		4
	#define AVDTP_SEID_NFO_MASK_TYP			0x08
	#define AVDTP_SEID_NFO_SHIFT_TYP		3


//service capability categoris
	#define AVDTP_SVC_CAT_MEDIA_TRANSPORT		1
	#define AVDTP_SVC_CAT_REPORTING			2
	#define AVDTP_SVC_CAT_RECOVERY			3
	#define AVDTP_SVC_CAT_CONTENT_PROTECTION	4
	#define AVDTP_SVC_CAT_HEADER_COMPRESSION	5
	#define AVDTP_SVC_CAT_MULTIPLEXING		6
	#define AVDTP_SVC_CAT_MEDIA_CODEC		7

//codec types
	#define A2DP_CODEC_TYP_SBC			0
	//others exist...done don't implement them

#define MY_ENDPT_ID					1		//we support just one, and this is it's ID

#define A2DP_CHAN_MODE_MONO				0
#define A2DP_CHAN_MODE_DUAL_CHANNEL			1
#define A2DP_CHAN_MODE_STEREO				2
#define A2DP_CHAN_MODE_JOINT_STEREO			3

#define PSM_AVDTP   0x0019
#define AUDIO_SINK_UUID 0x110B
#define ADVANCED_AUDIO_UUID 0x110D

typedef struct A2dpInstance {
    struct A2dpState* state;
} A2dpInstance;

typedef struct A2dpState {

    //L2CAP data
    l2c_handle_t l2cHandle;
    acl_handle_t aclHandle;

    A2dpInstance* ctrlInstance;
    A2dpInstance* dataInstance;

} A2dpState;

// to enforce only one connnection at a time
static A2dpState* gState = NULL;

static pthread_mutex_t  gStateLock = PTHREAD_MUTEX_INITIALIZER;

static struct a2dpSinkDescriptor* gDesc = NULL;

#define QUALITY_LOWEST	1	//you may notice the quality reduction
#define QUALITY_MEDIUM	2	//pretty good
#define QUALITY_GREAT	3	//as good as it will get without an FPU


///config options begin

/*
	This is a rather clever little SBC decoder that I've put together 

*/

//copyright notice for SBC decoder
/*
Copyright (c) 2011, Dmitry Grinberg (as published on http://dmitrygr.com)
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following condition is met: 

* Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer. 

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#define QUALITY	QUALITY_GREAT	
#define SPEED_OVER_ACCURACY			//set to cheat a bit with shifts (saves a divide per sample)
#define ITER		uint32_t			//iterator up to 180 use fastest type for your platform

///config options end


#if QUALITY == QUALITY_LOWEST

	#define CONST(x)		(x >> 24)
	#define SAMPLE_CVT(x)		(x >> 8)
	#define INSAMPLE		int8_t
	#define OUTSAMPLE		uint8_t	//no point producing 16-bit samples using the 8-bit decoder
	#define FIXED			int8_t
	#define FIXED_S			int16_t
	#define OUT_CLIP_MAX		0x7F
	#define OUT_CLIP_MIN		-0x80

	#define NUM_FRAC_BITS_PROTO	8
	#define NUM_FRAC_BITS_COS	6

#elif QUALITY == QUALITY_MEDIUM

	#define CONST(x)		(x >> 16)
	#define SAMPLE_CVT(x)		(x)
	#define INSAMPLE		int16_t
	#define OUTSAMPLE		uint16_t
	#define FIXED			int16_t
	#define FIXED_S			int32_t
	#define OUT_CLIP_MAX		0x7FFF
	#define OUT_CLIP_MIN		-0x8000

	#define NUM_FRAC_BITS_PROTO	16
	#define NUM_FRAC_BITS_COS	14

#elif QUALITY == QUALITY_GREAT

	#define CONST(x)		(x)
	#define SAMPLE_CVT(x)		(x)
	#define INSAMPLE		int16_t
	#define OUTSAMPLE		uint16_t
	#define FIXED			int32_t
	#define FIXED_S			int64_t
	#define OUT_CLIP_MAX		0x7FFF
	#define OUT_CLIP_MIN		-0x8000

	#define NUM_FRAC_BITS_PROTO	32
	#define NUM_FRAC_BITS_COS	30

#else

	#error "You did not define SBC decoder synthesizer quality to use"

#endif



static const FIXED proto_4_40[] =
{
	CONST(0x00000000), CONST(0x00FB7991), CONST(0x02CB3E8B), CONST(0x069FDC59),
	CONST(0x22B63DA5), CONST(0x4B583FE6), CONST(0xDD49C25B), CONST(0x069FDC59),
	CONST(0xFD34C175), CONST(0x00FB7991), CONST(0x002329CC), CONST(0x00FF11CA),
	CONST(0x053B7546), CONST(0x0191E578), CONST(0x31EAB920), CONST(0x4825E4A3),
	CONST(0xEC1F5E6D), CONST(0x083DDC80), CONST(0xFF3773A8), CONST(0x00B32807),
	CONST(0x0061C5A7), CONST(0x007A4737), CONST(0x07646684), CONST(0xF89F23A7),
	CONST(0x3F23948D), CONST(0x3F23948D), CONST(0xF89F23A7), CONST(0x07646684),
	CONST(0x007A4737), CONST(0x0061C5A7), CONST(0x00B32807), CONST(0xFF3773A8),
	CONST(0x083DDC80), CONST(0xEC1F5E6D), CONST(0x4825E4A3), CONST(0x31EAB920),
	CONST(0x0191E578), CONST(0x053B7546), CONST(0x00FF11CA), CONST(0x002329CC)
};

static const FIXED proto_8_80[] =
{
	CONST(0x00000000), CONST(0x0083D8D4), CONST(0x0172E691), CONST(0x034FD9E0),
	CONST(0x116860F5), CONST(0x259ED8EB), CONST(0xEE979F0B), CONST(0x034FD9E0),
	CONST(0xFE8D196F), CONST(0x0083D8D4), CONST(0x000A42E6), CONST(0x0089DE90),
	CONST(0x020E372C), CONST(0x02447D75), CONST(0x153E7D35), CONST(0x253844DE),
	CONST(0xF2625120), CONST(0x03EBE849), CONST(0xFF1ACF26), CONST(0x0074E5CF),
	CONST(0x00167EE3), CONST(0x0082B6EC), CONST(0x02AD6794), CONST(0x00BFA1FF),
	CONST(0x18FAB36D), CONST(0x24086BF5), CONST(0xF5FF2BF8), CONST(0x04270CA8),
	CONST(0xFF93E21B), CONST(0x0060C1E9), CONST(0x002458FC), CONST(0x0069F16C),
	CONST(0x03436717), CONST(0xFEBDD6E5), CONST(0x1C7762DF), CONST(0x221D9DE0),
	CONST(0xF950DCFC), CONST(0x0412523E), CONST(0xFFF44825), CONST(0x004AB4C5),
	CONST(0x0035FF13), CONST(0x003B1FA4), CONST(0x03C04499), CONST(0xFC4086B8),
	CONST(0x1F8E43F2), CONST(0x1F8E43F2), CONST(0xFC4086B8), CONST(0x03C04499),
	CONST(0x003B1FA4), CONST(0x0035FF13), CONST(0x004AB4C5), CONST(0xFFF44825),
	CONST(0x0412523E), CONST(0xF950DCFC), CONST(0x221D9DE0), CONST(0x1C7762DF),
	CONST(0xFEBDD6E5), CONST(0x03436717), CONST(0x0069F16C), CONST(0x002458FC),
	CONST(0x0060C1E9), CONST(0xFF93E21B), CONST(0x04270CA8), CONST(0xF5FF2BF8),
	CONST(0x24086BF5), CONST(0x18FAB36D), CONST(0x00BFA1FF), CONST(0x02AD6794),
	CONST(0x0082B6EC), CONST(0x00167EE3), CONST(0x0074E5CF), CONST(0xFF1ACF26),
	CONST(0x03EBE849), CONST(0xF2625120), CONST(0x253844DE), CONST(0x153E7D35),
	CONST(0x02447D75), CONST(0x020E372C), CONST(0x0089DE90), CONST(0x000A42E6)
};

static const FIXED costab_4[] =
{
	CONST(0x2D413CCD), CONST(0xD2BEC333), CONST(0xD2BEC333), CONST(0x2D413CCD),
	CONST(0x187DE2A7), CONST(0xC4DF2862), CONST(0x3B20D79E), CONST(0xE7821D59),
	CONST(0x00000000), CONST(0x00000000), CONST(0x00000000), CONST(0x00000000),
	CONST(0xE7821D59), CONST(0x3B20D79E), CONST(0xC4DF2862), CONST(0x187DE2A7),
	CONST(0xD2BEC333), CONST(0x2D413CCD), CONST(0x2D413CCD), CONST(0xD2BEC333),
	CONST(0xC4DF2862), CONST(0xE7821D59), CONST(0x187DE2A7), CONST(0x3B20D79E),
	CONST(0xC0000000), CONST(0xC0000000), CONST(0xC0000000), CONST(0xC0000000),
	CONST(0xC4DF2862), CONST(0xE7821D59), CONST(0x187DE2A7), CONST(0x3B20D79E)
};

static const FIXED costab_8[] =
{
	CONST(0x2D413CCD), CONST(0xD2BEC333), CONST(0xD2BEC333), CONST(0x2D413CCD),
	CONST(0x2D413CCD), CONST(0xD2BEC333), CONST(0xD2BEC333), CONST(0x2D413CCD),
	CONST(0x238E7673), CONST(0xC13AD060), CONST(0x0C7C5C1E), CONST(0x3536CC52),
	CONST(0xCAC933AE), CONST(0xF383A3E2), CONST(0x3EC52FA0), CONST(0xDC71898D),
	CONST(0x187DE2A7), CONST(0xC4DF2862), CONST(0x3B20D79E), CONST(0xE7821D59),
	CONST(0xE7821D59), CONST(0x3B20D79E), CONST(0xC4DF2862), CONST(0x187DE2A7),
	CONST(0x0C7C5C1E), CONST(0xDC71898D), CONST(0x3536CC52), CONST(0xC13AD060),
	CONST(0x3EC52FA0), CONST(0xCAC933AE), CONST(0x238E7673), CONST(0xF383A3E2),
	CONST(0x00000000), CONST(0x00000000), CONST(0x00000000), CONST(0x00000000),
	CONST(0x00000000), CONST(0x00000000), CONST(0x00000000), CONST(0x00000000),
	CONST(0xF383A3E2), CONST(0x238E7673), CONST(0xCAC933AE), CONST(0x3EC52FA0),
	CONST(0xC13AD060), CONST(0x3536CC52), CONST(0xDC71898D), CONST(0x0C7C5C1E),
	CONST(0xE7821D59), CONST(0x3B20D79E), CONST(0xC4DF2862), CONST(0x187DE2A7),
	CONST(0x187DE2A7), CONST(0xC4DF2862), CONST(0x3B20D79E), CONST(0xE7821D59),
	CONST(0xDC71898D), CONST(0x3EC52FA0), CONST(0xF383A3E2), CONST(0xCAC933AE),
	CONST(0x3536CC52), CONST(0x0C7C5C1E), CONST(0xC13AD060), CONST(0x238E7673),
	CONST(0xD2BEC333), CONST(0x2D413CCD), CONST(0x2D413CCD), CONST(0xD2BEC333),
	CONST(0xD2BEC333), CONST(0x2D413CCD), CONST(0x2D413CCD), CONST(0xD2BEC333),
	CONST(0xCAC933AE), CONST(0x0C7C5C1E), CONST(0x3EC52FA0), CONST(0x238E7673),
	CONST(0xDC71898D), CONST(0xC13AD060), CONST(0xF383A3E2), CONST(0x3536CC52),
	CONST(0xC4DF2862), CONST(0xE7821D59), CONST(0x187DE2A7), CONST(0x3B20D79E),
	CONST(0x3B20D79E), CONST(0x187DE2A7), CONST(0xE7821D59), CONST(0xC4DF2862),
	CONST(0xC13AD060), CONST(0xCAC933AE), CONST(0xDC71898D), CONST(0xF383A3E2),
	CONST(0x0C7C5C1E), CONST(0x238E7673), CONST(0x3536CC52), CONST(0x3EC52FA0),
	CONST(0xC0000000), CONST(0xC0000000), CONST(0xC0000000), CONST(0xC0000000),
	CONST(0xC0000000), CONST(0xC0000000), CONST(0xC0000000), CONST(0xC0000000),
	CONST(0xC13AD060), CONST(0xCAC933AE), CONST(0xDC71898D), CONST(0xF383A3E2),
	CONST(0x0C7C5C1E), CONST(0x238E7673), CONST(0x3536CC52), CONST(0x3EC52FA0),
	CONST(0xC4DF2862), CONST(0xE7821D59), CONST(0x187DE2A7), CONST(0x3B20D79E),
	CONST(0x3B20D79E), CONST(0x187DE2A7), CONST(0xE7821D59), CONST(0xC4DF2862),
	CONST(0xCAC933AE), CONST(0x0C7C5C1E), CONST(0x3EC52FA0), CONST(0x238E7673),
	CONST(0xDC71898D), CONST(0xC13AD060), CONST(0xF383A3E2), CONST(0x3536CC52)
};

static const int8_t loudness_4[4][4] =
{
	{ -1, 0, 0, 0 }, { -2, 0, 0, 1 },
	{ -2, 0, 0, 1 }, { -2, 0, 0, 1 }
};

static const int8_t loudness_8[4][8] =
{
	{ -2, 0, 0, 0, 0, 0, 0, 1 }, { -3, 0, 0, 0, 0, 0, 1, 2 },
	{ -4, 0, 0, 0, 0, 0, 1, 2 }, { -4, 0, 0, 0, 0, 0, 1, 2 }
};

static void synth_4(OUTSAMPLE* dst, const INSAMPLE* src, FIXED* V){  //A2DP figure 12.3

    ITER i, j;
    const FIXED* tabl = proto_4_40;
    const FIXED* costab = costab_4;

    //shift
    for(i = 79; i >= 8; i--) V[i] = V[i - 8];

    //matrix
    i = 8;
    do{
        FIXED_S t;
		t  = (FIXED_S)costab[0] * (FIXED_S)src[0];
		t += (FIXED_S)costab[1] * (FIXED_S)src[1];
		t += (FIXED_S)costab[2] * (FIXED_S)src[2];
		t += (FIXED_S)costab[3] * (FIXED_S)src[3];
        costab += 4;
        *V++ = t >> NUM_FRAC_BITS_COS;
    }while(--i);
    V -= 8;
    

    //calculate audio samples
    j = 0;
    do{

        OUTSAMPLE s;
        FIXED_S sample;
		sample  = (FIXED_S)V[  0] * (FIXED_S)tabl[0];
		sample += (FIXED_S)V[ 12] * (FIXED_S)tabl[1];
		sample += (FIXED_S)V[ 16] * (FIXED_S)tabl[2];
		sample += (FIXED_S)V[ 28] * (FIXED_S)tabl[3];
		sample += (FIXED_S)V[ 32] * (FIXED_S)tabl[4];
		sample += (FIXED_S)V[ 44] * (FIXED_S)tabl[5];
		sample += (FIXED_S)V[ 48] * (FIXED_S)tabl[6];
		sample += (FIXED_S)V[ 60] * (FIXED_S)tabl[7];
		sample += (FIXED_S)V[ 64] * (FIXED_S)tabl[8];
		sample += (FIXED_S)V[ 76] * (FIXED_S)tabl[9];
        tabl += 10;
        V++;

        sample >>= (NUM_FRAC_BITS_PROTO - 1 - 2);    //-2 is for the -4 we need to multiply by :)
        sample = -sample;

        if(sample >= OUT_CLIP_MAX) sample = OUT_CLIP_MAX;
        if(sample <= OUT_CLIP_MIN) sample = OUT_CLIP_MIN;
        s = sample;

        *dst++ = s;
        
    }while(--j);
}

static void synth_8(OUTSAMPLE* dst, const INSAMPLE* src, FIXED* V){  //A2DP figure 12.3

    ITER i, j;
    const FIXED* tabl = proto_8_80;
    const FIXED* costab = costab_8;

    //shift
    for(i = 159; i >= 16; i--) V[i] = V[i - 16];

    //matrix
    i = 16;
    do{
        FIXED_S t;
		t  = (FIXED_S)costab[0] * (FIXED_S)src[0];
		t += (FIXED_S)costab[1] * (FIXED_S)src[1];
		t += (FIXED_S)costab[2] * (FIXED_S)src[2];
		t += (FIXED_S)costab[3] * (FIXED_S)src[3];
		t += (FIXED_S)costab[4] * (FIXED_S)src[4];
		t += (FIXED_S)costab[5] * (FIXED_S)src[5];
		t += (FIXED_S)costab[6] * (FIXED_S)src[6];
		t += (FIXED_S)costab[7] * (FIXED_S)src[7];
        costab += 8;
        *V++ = t >> NUM_FRAC_BITS_COS;
        
    }while(--i);
    
    V -= 16;

    //calculate audio samples
    j = 8;
    do{

        OUTSAMPLE s;
        FIXED_S sample;

		sample  = (FIXED_S)V[  0] * (FIXED_S)tabl[0];
		sample += (FIXED_S)V[ 24] * (FIXED_S)tabl[1];
		sample += (FIXED_S)V[ 32] * (FIXED_S)tabl[2];
		sample += (FIXED_S)V[ 56] * (FIXED_S)tabl[3];
		sample += (FIXED_S)V[ 64] * (FIXED_S)tabl[4];
		sample += (FIXED_S)V[ 88] * (FIXED_S)tabl[5];
		sample += (FIXED_S)V[ 96] * (FIXED_S)tabl[6];
		sample += (FIXED_S)V[120] * (FIXED_S)tabl[7];
		sample += (FIXED_S)V[128] * (FIXED_S)tabl[8];
		sample += (FIXED_S)V[152] * (FIXED_S)tabl[9];
        tabl += 10;
        V++;

        sample >>= (NUM_FRAC_BITS_PROTO - 1 - 3);    //-3 is for the -8 we need to multiply by :)
        sample = -sample;

        if(sample > OUT_CLIP_MAX) sample = OUT_CLIP_MAX;
        if(sample < OUT_CLIP_MIN) sample = OUT_CLIP_MIN;
        s = sample;

        *dst++ = s;
        
    }while(--j);
}

static void synth(OUTSAMPLE* dst, const INSAMPLE* src, uint8_t nBands, FIXED* V){  //A2DP sigure 12.3

    //efficient SBC synth by Dmitry Grinberg (as published May 26, 2012)
    if(nBands == 4) synth_4(dst, src, V);
    else synth_8(dst, src, V);
}

static inline int32_t mulshift(int32_t val, uint32_t bits){        //return approximately      val / ((2^bits) - 1)

    static const uint32_t tab[] = {
        0x00000000, 0x00000000, 0x55555555, 0x24924925,
        0x11111111, 0x08421084, 0x04104104, 0x02040810,
        0x01010101, 0x00804020, 0x00401004, 0x00200400,
        0x00100100, 0x00080040, 0x00040010, 0x00020004,
        0x00010001
    };
       
       if (bits > 1)
               val = (((uint64_t)(uint32_t)val) * (uint64_t)tab[bits]) >> 32; /* if all goes well, this will compile to a single UMULL instr on ARMv5+ */
       
       return val;
}


static void a2dpSbcPlay(sg sgbuf) {

    //convenience
    int offset = 0;
    int length = sgLength(sgbuf);
    #define left (length - offset)

    //workspace
    ITER i, j, k, ch, numChan;
    uint32_t scaleFactors[2][8];
    int32_t bitneed[2][8];
    uint32_t bits[2][8], join = 0;
    INSAMPLE samples[16][2][8];

    //audio data
    #define bufNumBuffers	3	//if less than 3, we're being suboptimal in speed.
    #define bufNumSamples	1536	//FYI: 128 samples is the maximum number of samples a single packet may have. MUST be a multiple of 8!
    static OUTSAMPLE audio[bufNumBuffers][bufNumSamples];
    static uint32_t whichBuf = 0;
    static uint32_t bufSamples = 0;
    static FIXED V[2][160] = {{0,},};

    uint8_t numFrames;
    
    //process the packet header
    if (left < 12) return;	//too short of a packet header

/*
 * we no longer need to parse packet header here since in A2DP the params are agreed-upon earlier
 *    ver = (*buf[0]) >> 6;
 *    pad = ((*buf[0]) >> 5) & 1;
 *    ext = ((*buf[0]) >> 4) & 1;
 *    cc = (*buf[0]) & 0x0F;
 *    mark = (*buf[1]) >> 7;
 *    pt = (*buf[1]) & 0x7F;
 *    seq = (((uint16_t)buf[2]) << 8) | buf[3];
 *    time = (((uint32_t)buf[4]) << 24) | (((uint32_t)buf[5]) << 16) | (((uint32_t)buf[6]) << 8) | buf[7];
 *    src = (((uint32_t)buf[8]) << 24) | (((uint32_t)buf[9]) << 16) | (((uint32_t)buf[10]) << 8) | buf[11];
 */
    offset += 12;

    //process the count
    if(left < 1) return;		//too short of a count
    sgSerialize(sgbuf, offset++, 1, &numFrames);

    //process packets
    while (numFrames--) {
        uint8_t sync, tmp, samplingRate, blocks, chanMode, snr, numSubbands, bitpoolSz, hdrCRC, bitpos = 0x80;
        int8_t max_bitneed[2] = {0, 0};

        //process frame header
        if(left < 4) break;		//too short a frame header

        sgSerialize(sgbuf, offset++, 1, &sync);
        if (sync != 0x9C) {
            loge("sync %02X != 0x9C, bailing\n", sync);
            return;
        }
        sgSerialize(sgbuf, offset++, 1, &tmp);
        samplingRate = tmp >> 6;	//see A2DP table 12.16
        blocks = (tmp >> 4) & 3;	//see A2DP table 12.17
        chanMode = (tmp >> 2) & 3;	//see A2DP table 12.18
        snr = (tmp >> 1) & 1;	//see A2DP table 12.19
        numSubbands = tmp & 1;	//see A2DP table 12.20
        sgSerialize(sgbuf, offset++, 1, &bitpoolSz);
        sgSerialize(sgbuf, offset++, 1, &hdrCRC);

        numChan = (chanMode == A2DP_CHAN_MODE_MONO) ? 1 : 2;

        //process some numbers based on the tables
        numSubbands = numSubbands ? 8 : 4;
        blocks = (blocks + 1) << 2;

        //read "join" table if expected
        if(chanMode == A2DP_CHAN_MODE_JOINT_STEREO){
            sgSerialize(sgbuf, offset, 1, &tmp);
            join = tmp;	//we use it as a bitfield starting at top bit. 
            join >>= (8 - (numSubbands - 1));
            join <<= (8 - (numSubbands - 1));
            if(numSubbands == 8) offset++;
            else bitpos = 0x08;
        }

        //read scale factors
        for(ch = 0; ch < numChan; ch++) for(i = 0; i < numSubbands; i++){
            sgSerialize(sgbuf, offset, 1, &tmp);
            if(bitpos == 0x80) {
                scaleFactors[ch][i] = tmp >> 4;
                bitpos = 0x08;
            } else {
                scaleFactors[ch][i] = tmp & 0x0F;
                offset++;
                bitpos = 0x80;
            }
        }

        //calculate bitneed table and max_bitneed value (A2DP 12.6.3.1)
        for(ch = 0; ch < numChan; ch++){
            if(snr){

                for(i = 0; i < numSubbands; i++){

                    bitneed[ch][i] = scaleFactors[ch][i];
                    if(bitneed[ch][i] > max_bitneed[ch]) max_bitneed[ch] = bitneed[ch][i];
                }
            }
            else{

                const signed char* tbl;

                if(numSubbands == 4) tbl = loudness_4[samplingRate];
                else tbl = loudness_8[samplingRate];

                for(i = 0; i < numSubbands; i++){

                    if(scaleFactors[ch][i]){

                        int loudness = scaleFactors[ch][i] - tbl[i];

                        if(loudness > 0) loudness /= 2;
                        bitneed[ch][i] = loudness;
                    }
                    else bitneed[ch][i] = -5;
                    if(bitneed[ch][i] > max_bitneed[ch]) max_bitneed[ch] = bitneed[ch][i];
                }
            }
        }

        if(chanMode == A2DP_CHAN_MODE_MONO || chanMode == A2DP_CHAN_MODE_DUAL_CHANNEL){
            for(ch = 0; ch < numChan; ch++){
                //fit bitslices into the bitpool
                int32_t bitcount = 0, slicecount = 0, bitslice = max_bitneed[ch] + 1;
                do{
                    bitslice--;
                    bitcount += slicecount;
                    slicecount = 0;
                    for(i = 0; i < numSubbands; i++){

                        if(bitneed[ch][i] > bitslice + 1 && bitneed[ch][i] < bitslice + 16) slicecount++;
                        else if(bitneed[ch][i] == bitslice + 1) slicecount += 2;
                    }

                }while(bitcount + slicecount < bitpoolSz);

                //distribute bits
                for(i = 0; i < numSubbands; i++){

                    if(bitneed[ch][i] < bitslice + 2) bits[ch][i] = 0;
                    else{

                        int8_t v = bitneed[ch][i] - bitslice;
                        if(v > 16) v = 16;
                        bits[ch][i] = v;
                    }
                }

                //allocate remaining bits
                for(i = 0; i < numSubbands && bitcount < bitpoolSz; i++){

                    if(bits[ch][i] >= 2 && bits[ch][i] < 16){

                        bits[ch][i]++;
                        bitcount++;
                    }
                    else if(bitneed[ch][i] == bitslice + 1 && bitpoolSz > bitcount + 1){

                        bits[ch][i] = 2;
                        bitcount += 2;
                    }
                }
                for(i = 0; i < numSubbands && bitcount < bitpoolSz; i++){

                    if(bits[ch][i] < 16){

                        bits[ch][i]++;
                        bitcount++;
                    }
                }
            }
        }
        else{
            //calculate max_bitneed value (A2DP 12.6.3.2)
            uint8_t max_bitneed_val = max_bitneed[0] > max_bitneed[1] ? max_bitneed[0] : max_bitneed[1];

            //fit bitslices into the bitpool
            int32_t bitcount = 0, slicecount = 0, bitslice = max_bitneed_val + 1;
            do{
                bitslice--;
                bitcount += slicecount;
                slicecount = 0;
                for(ch = 0; ch < 2; ch++) for(i = 0; i < numSubbands; i++){

                    if(bitneed[ch][i] > bitslice + 1 && bitneed[ch][i] < bitslice + 16) slicecount++;
                    else if(bitneed[ch][i] == bitslice + 1) slicecount += 2;
                }

            }while(bitcount + slicecount < bitpoolSz);

            //distribute bits
            for(ch = 0; ch < 2; ch++) for(i = 0; i < numSubbands; i++){

                if(bitneed[ch][i] < bitslice + 2) bits[ch][i] = 0;
                else{

                    int8_t v = bitneed[ch][i] - bitslice;
                    if(v > 16) v = 16;
                    bits[ch][i] = v;
                }
            }

            //allocate remaining bits
            i = 0;
            ch = 0;
            while(i < numSubbands && bitcount < bitpoolSz){

                if(bits[ch][i] >= 2 && bits[ch][i] < 16){

                    bits[ch][i]++;
                    bitcount++;
                }
                else if(bitneed[ch][i] == bitslice + 1 && bitpoolSz > bitcount + 1){

                    bits[ch][i] = 2;
                    bitcount += 2;
                }
                if(++ch == 2){
                    ch = 0;
                    i++;
                }
            }
            i = 0;
            ch = 0;
            while(i < numSubbands && bitcount < bitpoolSz){

                if(bits[ch][i] < 16){

                    bits[ch][i]++;
                    bitcount++;
                }
                if(++ch == 2){
                    ch = 0;
                    i++;
                }
            }
        }

        //reconstruct subband samples (A2DP 12.6.4)
        #ifndef SPEED_OVER_ACCURACY
            int32_t levels[2][8];
            for(ch = 0; ch < numChan; ch++) for(i = 0; i < numSubbands; i++) levels[ch][i] = (1 << bits[ch][i]) - 1;
        #endif
        for(j = 0; j < blocks; j++){

            ITER bf = join;

            for(ch = 0; ch < numChan; ch++) for(i = 0; i < numSubbands; i++){

                if(bits[ch][i]){

                    uint32_t val = 0;
                    k = bits[ch][i];
                    sgSerialize(sgbuf, offset, 1, &tmp);
                    do{

                        val <<= 1;
                        if(tmp & bitpos) val++;
                        if(!(bitpos >>= 1)){
                            bitpos = 0x80;
                            sgSerialize(sgbuf, ++offset, 1, &tmp);
                        }
                    }while(--k);

                    val = (val << 1) | 1;
                    val <<= scaleFactors[ch][i];

                    #ifdef SPEED_OVER_ACCURACY
                        //clever arm hack to approximate division quickly
                        val = mulshift(val, bits[ch][i]);
                    #else
                        val /= levels[ch][i];
                    #endif

                    val -= (1 << scaleFactors[ch][i]);

                    samples[j][ch][i] = SAMPLE_CVT(val);

                }
                else samples[j][ch][i] = SAMPLE_CVT(0);
            }

            //joint processing (if needed - if not needed join will be 0)
            for(i = 0; i < numSubbands; i++, bf <<= 1) if(bf & 0x80){

                INSAMPLE t = samples[j][0][i];

                samples[j][0][i] += samples[j][1][i];
                samples[j][1][i] = t - samples[j][1][i];
            }
        }

        //synthesis
        
        for(j = 0; j < blocks; j++){

			OUTSAMPLE t_out[2][8];
			ITER ch;

            //do the actual synthesis
            for(ch = 0; ch < numChan; ch++) synth(t_out[ch], samples[j][ch], numSubbands, V[ch]);
            for(i = 0; i < numSubbands; i++) for(ch = 0; ch < 2; ch++) audio[whichBuf][bufSamples++] = t_out[numChan == 2 ? ch : 0][i];	//we always output stereo

            //if buffer is full, enqueue it
            if(bufSamples == bufNumSamples){

                gDesc->dataCb(gDesc->userData, audio[whichBuf++], bufSamples);
                
                if(whichBuf == bufNumBuffers) whichBuf = 0;
                bufSamples = 0;
            }
        }
        //if we used a byte partially, skip the rest of it, it is "padding"
        if(bitpos != 0x80) offset++;
        if(left < 0) loge("A2DP: buffer over-read: %d\n", left);
    }
}

static void a2dpServiceDataRx(void* service, sg sgbuf) {
    A2dpInstance* instance = (A2dpInstance *)service;
    A2dpState* state = instance->state;
    uint8_t trans, pktTyp, msgTyp, sigId, eid;
    uint8_t buf[16];
    uint16_t replSz = 0;
    uint16_t reqSz = sgLength(sgbuf);
    int offset = 0;
    uint8_t tmp;
    uint8_t newState;

    if(instance == state->dataInstance) {
        a2dpSbcPlay(sgbuf);
        return;
    }

    if (reqSz < 2) {
        loge("A2DP: packet too small\n");
        return;
    }

    sgSerialize(sgbuf, offset++, 1, &tmp);
    trans = (tmp & AVDTP_HDR_MASK_TRANS) >> AVDTP_HDR_SHIFT_TRANS;
    pktTyp = (tmp & AVDTP_HDR_MASK_PKT_TYP) >> AVDTP_HDR_SHIFT_PKT_TYP;
    msgTyp = (tmp & AVDTP_HDR_MASK_MSG_TYP) >> AVDTP_HDR_SHIFT_MSG_TYP;
    sgSerialize(sgbuf, offset++, 1, &tmp);
    sigId = (tmp & AVDTP_HDR_MASK_SIG_ID) >> AVDTP_HDR_SHIFT_SIG_ID;
    reqSz -= 2;

    #if A2DP_DBG
        logd("A2DP data(trans %d, pktTyp %d msgTyp %d sigId %d %ub)\n", trans, pktTyp, msgTyp, sigId, reqSz);
    #endif

    switch (sigId) {
        case AVDTP_SIG_DISCOVER:
           logd("AVDTP_SIG_DISCOVER\n");
            if(reqSz)loge("A2DP: unexpected further data in AVDTP_SIG_DISCOVER\n");
            //packet header
            buf[0] = (trans << AVDTP_HDR_SHIFT_TRANS) | (AVDTP_PKT_TYP_SINGLE << AVDTP_HDR_SHIFT_PKT_TYP) | (AVDTP_MSG_TYP_ACCEPT << AVDTP_HDR_SHIFT_MSG_TYP);
            buf[1] = (sigId << AVDTP_HDR_SHIFT_SIG_ID);
            //return a single SEID info structure
            buf[2] = (MY_ENDPT_ID << AVDTP_SEID_NFO_SHIFT_SEID) | (0 << AVDTP_SEID_NFO_MASK_INUSE);
            buf[3] = (AVDTP_MEDIA_TYP_AUDIO << AVDTP_SEID_NFO_SHIFT_MEDIA_TYP) | (AVDTP_DIR_SINK << AVDTP_SEID_NFO_SHIFT_TYP);
            replSz = 4;
            break;

        case AVDTP_SIG_GET_CAPABILITIES:
            logd("AVDTP_SIG_GET_CAPABILITIES\n");

            if(reqSz > 1)loge("A2DP: unexpected further data in AVDTP_SIG_GET_CAPABILITIES\n");
            if(reqSz < 1) {
                loge("A2DP: not enough data in AVDTP_SIG_GET_CAPABILITIES\n");
                break;
            }
            sgSerialize(sgbuf, offset++, 1, &tmp);
            eid = tmp >> 2;
            if(eid != MY_ENDPT_ID){
                loge("A2DP: bad EID (%d) AVDTP_SIG_GET_CAPABILITIES\n", eid);
                break;
            }
            //packet header
            buf[0] = (trans << AVDTP_HDR_SHIFT_TRANS) | (AVDTP_PKT_TYP_SINGLE << AVDTP_HDR_SHIFT_PKT_TYP) | (AVDTP_MSG_TYP_ACCEPT << AVDTP_HDR_SHIFT_MSG_TYP);
            buf[1] = (sigId << AVDTP_HDR_SHIFT_SIG_ID);
            //capability 0 - media transport
            buf[2] = AVDTP_SVC_CAT_MEDIA_TRANSPORT;
            buf[3] = 0;
            //capability 1 - SDB codec (and its info)
            buf[4] = AVDTP_SVC_CAT_MEDIA_CODEC;
            buf[5] = 6; //generic data is 2 bytes, SBC data is 4 bytes (see A2DP spec section 4.3.2)
            buf[6] = (AVDTP_MEDIA_TYP_AUDIO << 4);
            buf[7] = A2DP_CODEC_TYP_SBC;
            buf[8] = 0xFF; //spec forces us to support all of these (even though ANDROID and LINUX both do not properly support mono [try it :)  ])
            buf[9] = 0xFF; //spec forces us to support all of these
            buf[10] = 0x02; //min allowed by spec
            buf[11] = 0x50; //nice high value
            replSz = 12;
            break;

        case AVDTP_SIG_SET_CONFIGURATION:
            logd("AVDTP_SIG_SET_CONFIGURATION\n");

            if(reqSz < 8){ //no valid command is shorter
                loge("A2DP: not enough data in AVDTP_SIG_SET_CONFIGURATION\n");
                break;
            }
            sgSerialize(sgbuf, offset++, 1, &tmp);
            eid = tmp >> 2;
            if(eid != MY_ENDPT_ID){
                loge("A2DP: bad EID (%d) AVDTP_SIG_GET_CAPABILITIES\n", eid);
                break;
            }
            offset++; //skip INT EID - we don't care
            reqSz -= 2;
            while(reqSz >= 2){
                uint8_t cap, len;
                sgSerialize(sgbuf, offset++, 1, &cap);
                sgSerialize(sgbuf, offset++, 1, &len);
                reqSz -= 2;

                if(len > reqSz) break;
                reqSz -= len;

                if(cap == AVDTP_SVC_CAT_MEDIA_CODEC){
                    sgSerialize(sgbuf, offset++, 1, &tmp);
                    if(tmp != (AVDTP_MEDIA_TYP_AUDIO << 4)){
                        loge("A2DP: requested data is not audio\n");
                        break;
                    }
                    sgSerialize(sgbuf, offset++, 1, &tmp);
                    if(tmp != A2DP_CODEC_TYP_SBC){
                        loge("A2DP: requested codec is not SBC\n");
                        break;
                    } else {
                        static const char* strMode[] = {NULL, "Joint Stereo", "Stereo", NULL, "Dual Channel", NULL, NULL, NULL, "Mono"};
                        uint8_t samplingRate, channelMode, blockLen, subbands, allocMethod, minBitpool, maxBitpool;
                        unsigned int rate;

                        sgSerialize(sgbuf, offset++, 1, &tmp);
                        samplingRate = tmp >> 4;
                        channelMode = tmp & 0x0F;
                        sgSerialize(sgbuf, offset++, 1, &tmp);
                        blockLen = tmp >> 4;
                        subbands = (tmp >> 2) & 3;
                        allocMethod = tmp & 3;
                        sgSerialize(sgbuf, offset++, 1, &minBitpool);
                        sgSerialize(sgbuf, offset++, 1, &maxBitpool);

                        switch(samplingRate){
                            case 1: rate = 48000; break;
                            case 2: rate = 44100; break;
                            case 4: rate = 32000; break;
                            case 8: rate = 16000; break;
                            default:
                                loge("A2DP: invalid sampling rate: %d\n", samplingRate);
                                goto out;
                        }
                        switch(blockLen){
                            case 1: blockLen = 16; break;
                            case 2: blockLen = 12; break;
                            case 4: blockLen = 8; break;
                            case 8: blockLen = 4; break;
                            default:
                                loge("A2DP: invalid block len: %d\n", blockLen);
                                goto out;
                        }
                        switch(subbands){
                            case 1: subbands = 8; break;
                            case 2: subbands = 4; break;
                            default:
                                loge("A2DP: invalid num subbands: %d\n", subbands);
                                goto out;
                        }
                        switch(allocMethod){
                            case 1: allocMethod = 0; break;
                            case 2: allocMethod = 1; break;
                            default:
                                loge("A2DP: invalid allocMethod: %d\n", allocMethod);
                                goto out;
                        }
                        logd("call paramsCb %p rate: %d\n", gDesc->paramsCb, rate);
                        gDesc->paramsCb(gDesc->userData, rate, channelMode);
                        logd("A2DP:\n\t* %u %s-framed frames / block\n\t* %u subbands/frame\n\t* %u KHz sampling rate\n\t* '%s' channel mode\n\t* %u-%u bits per piece per channel\n",
                            blockLen, allocMethod ? "SNR" : "Loudness", subbands,
                            rate, strMode[channelMode], minBitpool, maxBitpool);
                    }
                }
                else offset += len;
            }
            if(reqSz) { //no valid command is shorter
                loge("A2DP: malformed config info in AVDTP_SIG_SET_CONFIGURATION\n");
            } else {
                //prepare reply
                buf[0] = (trans << AVDTP_HDR_SHIFT_TRANS) | (AVDTP_PKT_TYP_SINGLE << AVDTP_HDR_SHIFT_PKT_TYP) | (AVDTP_MSG_TYP_ACCEPT << AVDTP_HDR_SHIFT_MSG_TYP);
                buf[1] = (sigId << AVDTP_HDR_SHIFT_SIG_ID);
                replSz = 2;
            }
            break;

        case AVDTP_SIG_OPEN:
            logd("AVDTP_SIG_OPEN\n");
            if(reqSz > 1) loge("A2DP: unexpected further data in AVDTP_SIG_OPEN\n");
            if(reqSz < 1){
                loge("A2DP: not enough data in AVDTP_SIG_OPEN\n");
                break;
            }
            sgSerialize(sgbuf, offset++, 1, &tmp);
            eid = tmp >> 2;
            if(eid != MY_ENDPT_ID){
                loge("A2DP: bad EID (%d) AVDTP_SIG_OPEN\n", eid);
                break;
            }
            gDesc->stateCb(gDesc->userData, A2DP_STATE_OPEN);

            //packet header
            buf[0] = (trans << AVDTP_HDR_SHIFT_TRANS) | (AVDTP_PKT_TYP_SINGLE << AVDTP_HDR_SHIFT_PKT_TYP) | (AVDTP_MSG_TYP_ACCEPT << AVDTP_HDR_SHIFT_MSG_TYP);
            buf[1] = (sigId << AVDTP_HDR_SHIFT_SIG_ID);
            replSz = 2;
            break;

        case AVDTP_SIG_START:
            logd("AVDTP_SIG_START\n");
            if(reqSz > 1) loge("A2DP: unexpected further data in AVDTP_SIG_OPEN\n");
            if(reqSz < 1){
                loge("A2DP: not enough data in AVDTP_SIG_OPEN\n");
                break;
            }
            sgSerialize(sgbuf, offset++, 1, &tmp);
            eid = tmp >> 2;
            if(eid != MY_ENDPT_ID){
                loge("A2DP: bad EID (%d) AVDTP_SIG_OPEN\n", eid);
                break;
            }
            gDesc->stateCb(gDesc->userData, A2DP_STATE_START);

            //packet header
            buf[0] = (trans << AVDTP_HDR_SHIFT_TRANS) | (AVDTP_PKT_TYP_SINGLE << AVDTP_HDR_SHIFT_PKT_TYP) | (AVDTP_MSG_TYP_ACCEPT << AVDTP_HDR_SHIFT_MSG_TYP);
            buf[1] = (sigId << AVDTP_HDR_SHIFT_SIG_ID);
            replSz = 2;
            break;

        case AVDTP_SIG_CLOSE:
        case AVDTP_SIG_SUSPEND:
        case AVDTP_SIG_ABORT:
            logd("AVDTP_SIG_CLOSE/AVDTP_SIG_SUSPEND/AVDTP_SIG_ABORT\n");
            if(reqSz > 1) loge("A2DP: unexpected further data in AVDTP_SIG_CLOSE/ABORT/SUSPEND\n");
            if(reqSz < 1){
                loge("A2DP: not enough data in AVDTP_SIG_CLOSE/ABORT/SUSPEND\n");
                break;
            }
            sgSerialize(sgbuf, offset++, 1, &tmp);
            eid = tmp >> 2;
            if(eid != MY_ENDPT_ID){
                loge("A2DP: bad EID (%d) AVDTP_SIG_CLOSE/ABORT/SUSPEND\n", eid);
                break;
            }

            gDesc->stateCb(gDesc->userData, A2DP_STATE_STOP);

            //packet header
            buf[0] = (trans << AVDTP_HDR_SHIFT_TRANS) | (AVDTP_PKT_TYP_SINGLE << AVDTP_HDR_SHIFT_PKT_TYP) | (AVDTP_MSG_TYP_ACCEPT << AVDTP_HDR_SHIFT_MSG_TYP);
            buf[1] = (sigId << AVDTP_HDR_SHIFT_SIG_ID);
            replSz = 2;
            break;

        default:
//            fprintf(stderr, "A2DP data(trans %d, pktTyp %d msgTyp %d sigId %d %ub)\n", trans, pktTyp, msgTyp, sigId, reqSz);
//            fprintf(stderr, "DATA: ");
//            while(reqSz--) fprintf(stderr, " %02X", *req++);
//            fprintf(stderr, "\n");
            break;
    }

out:
    if (replSz) {
        sg sgbuf = sgNewWithCopyData(buf, replSz);
        if (sgbuf) {
            l2cApiDataTx(state->l2cHandle, sgbuf);
        } else {
            loge("sgNewWithCopyData fail\n");
        }
    }
}

static void a2dpServiceClose(void* service) {
    A2dpInstance* instance = (A2dpInstance *)service;
    A2dpState* state = instance->state;

    pthread_mutex_lock(&gStateLock);

    if (state != gState) {
        loge("unknown state %p\n", state);
        goto out;
    }

    if (instance == state->ctrlInstance) {
        state->ctrlInstance = NULL;
    } else if (instance == state->dataInstance) {
        state->dataInstance = NULL;
    }
    free(instance);
    if (!state->ctrlInstance && !state->dataInstance) {
        logd("gState closed\n");
        gDesc->stateCb(gDesc->userData, A2DP_STATE_CLOSED);

        free(state);
        gState = NULL;
    }

out:
    pthread_mutex_unlock(&gStateLock);
}

static uint8_t a2dpServiceChAlloc(void *userData, l2c_handle_t handle, void **instanceP) {
    A2dpState* state;
    A2dpInstance* instance;
    acl_handle_t aclHandle = l2cApiGetAclHandle(handle);
    uint8_t ret = SVC_ALLOC_FAIL_OTHER;

    logw("a2dpServiceChAlloc %lld aclHandle: %lld\n", handle, aclHandle);

    pthread_mutex_lock(&gStateLock);

    if (gState) {
        if (gState->dataInstance == NULL && gState->aclHandle == aclHandle) {
            instance = (A2dpInstance *)malloc(sizeof(*instance));
            instance->state = gState;
            gState->dataInstance = instance;
            *instanceP = instance;
            ret = SVC_ALLOC_SUCCESS;
            goto out;
        }
        loge("a2dpServiceChAlloc rejecting second connection\n");
        goto out;
    }
    
    state = (A2dpState *)malloc(sizeof(A2dpState));
    instance = (A2dpInstance *)malloc(sizeof(*instance));

    if (state && instance) {
        state->l2cHandle = handle;
        state->aclHandle = aclHandle;
        state->ctrlInstance = instance;
        state->dataInstance = NULL;
        instance->state = state;
        gState = state;
        *instanceP = instance;
        ret = SVC_ALLOC_SUCCESS;
    }

out:
    pthread_mutex_unlock(&gStateLock);
    return ret;
}

static void a2dpServiceChStateCbk(void *userData, void *instance, uint8_t state, const void *data, uint32_t len) {
    switch (state) {
    case L2C_STATE_OPEN:
        logw("L2C_STATE_OPEN handle: %lld\n", *((l2c_handle_t *)data));
        if (len != sizeof(l2c_handle_t)) {
            loge("invalid length for open event\n");
            break;
        }
        break;
    case L2C_STATE_MTU:
        logw("L2C_STATE_MTU mtu: %d\n", *((uint16_t *)data));
        if (len != sizeof(uint16_t)) {
            loge("invalid length for mtu event\n");
            break;
        }
        break;
    case L2C_STATE_ENCR:
        logw("L2C_STATE_ENCR\n");
        break;
    case L2C_STATE_RX:
//        logw("L2C_STATE_RX\n");
        if (len != sizeof(sg)) {
            loge("invalid length for RX event\n");
            break;
        }
        a2dpServiceDataRx(instance, *((sg *)data));
        break;
    case L2C_STATE_CLOSED:
        logw("L2C_STATE_CLOSED\n");
        a2dpServiceClose(instance);
        break;
    }
}

static uint8_t sdpDescrA2DP[] = {
        //service class ID list
        SDP_ITEM_DESC(SDP_TYPE_UINT, SDP_SZ_2), SDP_U16(SDP_ATTR_SVC_CLS_ID_LIST),
        SDP_ITEM_DESC(SDP_TYPE_ARRAY, SDP_SZ_u8), 3,
            SDP_ITEM_DESC(SDP_TYPE_UUID, SDP_SZ_2), SDP_U16(AUDIO_SINK_UUID),
        //ServiceId
        SDP_ITEM_DESC(SDP_TYPE_UINT, SDP_SZ_2), SDP_U16(SDP_ATTR_SVC_ID),
        SDP_ITEM_DESC(SDP_TYPE_UUID, SDP_SZ_2), SDP_U16(AUDIO_SINK_UUID),
        //ProtocolDescriptorList
        SDP_ITEM_DESC(SDP_TYPE_UINT, SDP_SZ_2), SDP_U16(SDP_ATTR_PROTOCOL_DESCR_LIST),
        SDP_ITEM_DESC(SDP_TYPE_ARRAY, SDP_SZ_u8), 16,
            SDP_ITEM_DESC(SDP_TYPE_ARRAY, SDP_SZ_u8), 6,
                SDP_ITEM_DESC(SDP_TYPE_UUID, SDP_SZ_2), SDP_U16(SDP_PROTO_L2CAP),
                SDP_ITEM_DESC(SDP_TYPE_UINT, SDP_SZ_2), SDP_U16(PSM_AVDTP),
            SDP_ITEM_DESC(SDP_TYPE_ARRAY, SDP_SZ_u8), 6,
                SDP_ITEM_DESC(SDP_TYPE_UUID, SDP_SZ_2), SDP_U16(PSM_AVDTP),
                SDP_ITEM_DESC(SDP_TYPE_UINT, SDP_SZ_2), SDP_U16(0x0100),	//AVDTP version (as per spec)
        //browse group list
        SDP_ITEM_DESC(SDP_TYPE_UINT, SDP_SZ_2), SDP_U16(SDP_ATTR_BROWSE_GRP_LIST),
            SDP_ITEM_DESC(SDP_TYPE_ARRAY, SDP_SZ_u8), 3,
            SDP_ITEM_DESC(SDP_TYPE_UUID, SDP_SZ_2), SDP_U16(SDP_BROWSE_GROUP_ID_PUBLIC),
        //profile descriptor list
        SDP_ITEM_DESC(SDP_TYPE_UINT, SDP_SZ_2), 0x00, 0x09, 
            SDP_ITEM_DESC(SDP_TYPE_ARRAY, SDP_SZ_u8), 8,
            SDP_ITEM_DESC(SDP_TYPE_ARRAY, SDP_SZ_u8), 6,
                SDP_ITEM_DESC(SDP_TYPE_UUID, SDP_SZ_2), SDP_U16(ADVANCED_AUDIO_UUID), // Advanced Audio
                SDP_ITEM_DESC(SDP_TYPE_UINT, SDP_SZ_2), SDP_U16(0x0100), // Version 1.0
        //name
        SDP_ITEM_DESC(SDP_TYPE_UINT, SDP_SZ_2), 0x01, 0x00, SDP_ITEM_DESC(SDP_TYPE_TEXT, SDP_SZ_u8), 9,
            'A', '2', 'D', 'P', ' ', 'S', 'i', 'n', 'k'
};


bool a2dpSinkInit(const struct a2dpSinkDescriptor* desc) {
    // FIXME synchronization
    if (gDesc) {
        loge("A2DP sink already initialized\n");
        return false;
    }

    gDesc = malloc(sizeof(*gDesc));
    if (!gDesc)
        return false;
    memcpy(gDesc, desc, sizeof(*gDesc));

    static const struct l2cServicePsmDescriptor a2dpService = {
        .serviceInstanceAlloc = a2dpServiceChAlloc,
        .serviceInstanceStateCbk = a2dpServiceChStateCbk,
        .serviceConnectionlessRx = NULL,
        .mtu = 0xFFFF,
    };

    if(!l2cApiServicePsmRegister(PSM_AVDTP, &a2dpService)) return 0;
    sdpServiceDescriptorAdd(sdpDescrA2DP, sizeof(sdpDescrA2DP));

    return true;
}


void a2dpSinkDeinit() {
    // FIXME synchronization and L2CAP cleanup
    if (gDesc) {
        free(gDesc);
        gDesc = NULL;
    }
}

