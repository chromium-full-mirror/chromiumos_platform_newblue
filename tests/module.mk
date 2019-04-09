# Copyright 2018 The Chromium OS Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
include common.mk

# Unit Tests
newblue_unittest_DEPS = CC_STATIC_LIBRARY(libnewblue_for_unittest.pie.a)
newblue_unittest_LIBS = -lgtest -pthread -lpthread $(libnewblue_LIBS)
newblue_unittest_OBJS = tests/unittest.o \
                        tests/persist_unittest.o \
                        tests/sm_unittest.o
CXX_BINARY(tests/newblue_unittest): $(newblue_unittest_OBJS) $(newblue_unittest_DEPS)
CXX_BINARY(tests/newblue_unittest): LDLIBS += $(newblue_unittest_LIBS)
clean: CLEAN(tests/newblue_unittest)
test: TEST(CXX_BINARY(tests/newblue_unittest))
