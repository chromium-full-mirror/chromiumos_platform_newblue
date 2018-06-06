/*
 * Copyright 2018 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */
#ifndef _NEWBLUE_MACROS_H_
#define _NEWBLUE_MACROS_H_

#define NEWBLUE_EXPORT __attribute__((__visibility__("default")))

#ifndef __packed
# define __packed __attribute__ ((__packed__))
#endif

#endif
