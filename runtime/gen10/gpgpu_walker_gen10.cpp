/*
 * Copyright (C) 2017-2018 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include "runtime/gen10/hw_info.h"
#include "runtime/command_queue/gpgpu_walker.h"
#include "runtime/command_queue/gpgpu_walker.inl"
#include "runtime/command_queue/gpgpu_walker_base.inl"
#include "runtime/command_queue/hardware_interface.h"
#include "runtime/command_queue/hardware_interface.inl"
#include "runtime/command_queue/hardware_interface_base.inl"

namespace OCLRT {

template class HardwareInterface<CNLFamily>;

template class GpgpuWalkerHelper<CNLFamily>;

template struct EnqueueOperation<CNLFamily>;

} // namespace OCLRT
