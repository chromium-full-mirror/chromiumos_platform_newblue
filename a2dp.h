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
#ifndef _A2DP_H_
#define _A2DP_H_

struct a2dpParams {


};

#define A2DP_STATE_NONE     0   /* Nothing happened yet */
#define A2DP_STATE_OPEN     1   /* connection opened */
#define A2DP_STATE_START    2   /* playback started */
#define A2DP_STATE_STOP     3   /* playback stopped */
#define A2DP_STATE_CLOSED   4   /* connection closed */
typedef void (*a2dpStateCbk)(void *userData, uint8_t state);
typedef void (*a2dpParamsCbk)(void *userData, unsigned int rate, uint8_t channelMode);
typedef void (*a2dpDataCbk)(void *userData, const uint16_t* samples, uint32_t sampleCount);

/* a FixedCh service is described thusly */
struct a2dpSinkDescriptor {
    void *userData; /* passed to all callbacks */

    /* callback for connection state */
    a2dpStateCbk stateCb;
    /* callback for audio parameters */
    a2dpParamsCbk paramsCb;
    /* callback for audio data */
    a2dpDataCbk dataCb;
};




bool a2dpSinkInit(const struct a2dpSinkDescriptor* desc);
void a2dpSinkDeinit();

#endif
