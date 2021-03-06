/*
 * Copyright (C) 2017-2018 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include "cl_api_tests.h"

using namespace OCLRT;

typedef api_tests clCreateSubDevicesTests;

namespace ULT {

TEST_F(clCreateSubDevicesTests, GivenInvalidDeviceWhenCreatingSubDevicesThenInvalidDeviceErrorIsReturned) {
    auto retVal = clCreateSubDevices(
        nullptr,
        nullptr,
        0,
        nullptr,
        nullptr);
    EXPECT_EQ(retVal, CL_INVALID_DEVICE);
}
} // namespace ULT
