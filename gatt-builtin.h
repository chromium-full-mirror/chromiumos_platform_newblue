/*
 * Copyright 2018 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */
#ifndef _GATT_BUILTIN_H_
#define _GATT_BUILTIN_H_

#include "newblue-macros.h"
#include "types.h"


/*
 * This file provides GATT profiles for GATT & GAP as required by the spec. It should be inited after
 * GATT and deinited before GATT.
 */

#define GAP_APPEAR_UNKNOWN                                       0    /* None */
#define GAP_APPEAR_GENERIC_PHONE                                64    /* Generic category */
#define GAP_APPEAR_GENERIC_COMPUTER                            128    /* Generic category */
#define GAP_APPEAR_GENERIC_WATCH                               192    /* Generic category */
#define GAP_APPEAR_WATCH_SPORTS_WATCH                          193    /* Watch subtype */
#define GAP_APPEAR_GENERIC_CLOCK                               256    /* Generic category */
#define GAP_APPEAR_GENERIC_DISPLAY                             320    /* Generic category */
#define GAP_APPEAR_GENERIC_REMOTE_CONTROL                      384    /* Generic category */
#define GAP_APPEAR_GENERIC_EYE_GLASSES                         448    /* Generic category */
#define GAP_APPEAR_GENERIC_TAG                                 512    /* Generic category */
#define GAP_APPEAR_GENERIC_KEYRING                             576    /* Generic category */
#define GAP_APPEAR_GENERIC_MEDIA_PLAYER                        640    /* Generic category */
#define GAP_APPEAR_GENERIC_BARCODE_SCANNER                     704    /* Generic category */
#define GAP_APPEAR_GENERIC_THERMOMETER                         768    /* Generic category */
#define GAP_APPEAR_THERMOMETER_EAR                             769    /* Thermometer subtype */
#define GAP_APPEAR_GENERIC_HEART_RATE_SENSOR                   832    /* Generic category */
#define GAP_APPEAR_HEART_RATE_SENSOR_HEART_RATE_BELT           833    /* Heart Rate Sensor subtype */
#define GAP_APPEAR_GENERIC_BLOOD_PRESSURE                      896    /* Generic category */
#define GAP_APPEAR_BLOOD_PRESSURE_ARM                          897    /* Blood Pressure subtype */
#define GAP_APPEAR_BLOOD_PRESSURE_WRIST                        898    /* Blood Pressure subtype */
#define GAP_APPEAR_HUMAN_INTERFACE_DEVICE_HID                  960    /* HID Generic */
#define GAP_APPEAR_KEYBOARD                                    961    /* HID subtype */
#define GAP_APPEAR_MOUSE                                       962    /* HID subtype */
#define GAP_APPEAR_JOYSTICK                                    963    /* HID subtype */
#define GAP_APPEAR_GAMEPAD                                     964    /* HID subtype */
#define GAP_APPEAR_DIGITIZER_TABLET                            965    /* HID subtype */
#define GAP_APPEAR_CARD_READER                                 966    /* HID subtype */
#define GAP_APPEAR_DIGITAL_PEN                                 967    /* HID subtype */
#define GAP_APPEAR_BARCODE_SCANNER                             968    /* HID subtype */
#define GAP_APPEAR_GENERIC_GLUCOSE_METER                      1024    /* Generic category */
#define GAP_APPEAR_GENERIC_RUNNING_WALKING_SENSOR             1088    /* Generic category */
#define GAP_APPEAR_RUNNING_WALKING_SENSOR_IN_SHOE             1089    /* Running Walking Sensor subtype */
#define GAP_APPEAR_RUNNING_WALKING_SENSOR_ON_SHOE             1090    /* Running Walking Sensor subtype */
#define GAP_APPEAR_RUNNING_WALKING_SENSOR_ON_HIP              1091    /* Running Walking Sensor subtype */
#define GAP_APPEAR_GENERIC_CYCLING                            1152    /* Generic category */
#define GAP_APPEAR_CYCLING_CYCLING_COMPUTER                   1153    /* Cycling subtype */
#define GAP_APPEAR_CYCLING_SPEED_SENSOR                       1154    /* Cycling subtype */
#define GAP_APPEAR_CYCLING_CADENCE_SENSOR                     1155    /* Cycling subtype */
#define GAP_APPEAR_CYCLING_POWER_SENSOR                       1156    /* Cycling subtype */
#define GAP_APPEAR_CYCLING_SPEED_AND_CADENCE_SENSOR           1157    /* Cycling subtype */
#define GAP_APPEAR_PULSEOX_GENERIC                            3136    /* Pulse Oximeter subtype */
#define GAP_APPEAR_PULSEOX_FINGERTIP                          3137    /* Pulse Oximeter subtype */
#define GAP_APPEAR_PULSEOX_WRIST_WORN                         3138    /* Pulse Oximeter subtype */
#define GAP_APPEAR_OSA_GENERIC                                5184    /* Outdoor Sports Activity subtype */
#define GAP_APPEAR_OSA_LOCATION_DISPLAY_DEVICE                5185    /* Outdoor Sports Activity subtype */
#define GAP_APPEAR_OSA_LOCATION_AND_NAVIGATION_DISPLAY_DEVICE 5186    /* Outdoor Sports Activity subtype */
#define GAP_APPEAR_OSA_LOCATION_POD                           5187    /* Outdoor Sports Activity subtype */
#define GAP_APPEAR_OSA_LOCATION_AND_NAVIGATION_POD            5188    /* Outdoor Sports Activity subtype */


bool gattBuiltinInit(void) NEWBLUE_EXPORT;
void gattBuiltinDeinit(void) NEWBLUE_EXPORT;

/* these are safe even while not inited! this is by design */
void gattBuiltinSetDevName(const char *name) NEWBLUE_EXPORT;
//TODO: these can be advertised too. we shodul make sure they match
void gattBuiltinSetPreferredConnParamsWhenSlave(uint16_t minInt, uint16_t maxInt, uint16_t lat,
    uint16_t to) NEWBLUE_EXPORT;
void gattBuiltinSetAppearance(uint16_t appearance) NEWBLUE_EXPORT; /* GAP_APPEAR_* */





#endif

