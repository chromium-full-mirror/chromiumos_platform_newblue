/*
 * Copyright 2018 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef _UHID_H_
#define _UHID_H_

#include "newblue-macros.h"
#include "sg.h"
#include "types.h"


NEWBLUE_BEGIN_DECLS

void hidConnStateCbk(ble_hid_conn_t hidId, uint8_t state) NEWBLUE_EXPORT;
void hidReportRxCbk(ble_hid_conn_t hidId, int32_t reportId, sg data, bool byRequest) NEWBLUE_EXPORT;

/* ONLY FOR UNITTEST PURPOSE */
bool parseReportDescriptorForTest(const uint8_t* rdData,
    const uint32_t rdSize,
    bool* reportIdPresent) NEWBLUE_EXPORT;

NEWBLUE_END_DECLS

#endif
