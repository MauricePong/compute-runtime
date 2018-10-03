/*
 * Copyright (C) 2017-2018 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include "runtime/command_stream/command_stream_receiver.h"
#include "runtime/helpers/options.h"
#include "runtime/helpers/ptr_math.h"
#include "runtime/mem_obj/buffer.h"
#include "unit_tests/aub_tests/command_queue/command_enqueue_fixture.h"
#include "unit_tests/mocks/mock_context.h"
#include "unit_tests/aub_tests/aub_tests_configuration.h"
#include "test.h"

#include <memory>

using namespace OCLRT;

struct ReadBufferHw
    : public CommandEnqueueAUBFixture,
      public ::testing::WithParamInterface<size_t>,
      public ::testing::Test {

    void SetUp() override {
        CommandEnqueueAUBFixture::SetUp();
    }

    void TearDown() override {
        CommandEnqueueAUBFixture::TearDown();
    }
};

typedef ReadBufferHw AUBReadBuffer;

HWTEST_P(AUBReadBuffer, simple) {
    MockContext context(this->pDevice);

    cl_float srcMemory[] = {1.0f, 2.0f, 3.0f, 4.0f};
    cl_float destMemory[] = {0.0f, 0.0f, 0.0f, 0.0f};
    auto retVal = CL_INVALID_VALUE;
    auto srcBuffer = std::unique_ptr<Buffer>(Buffer::create(
        &context,
        CL_MEM_USE_HOST_PTR,
        sizeof(srcMemory),
        srcMemory,
        retVal));
    ASSERT_NE(nullptr, srcBuffer.get());

    auto pSrcMemory = &srcMemory[0];
    auto pDestMemory = &destMemory[0];

    cl_bool blockingRead = CL_FALSE;
    size_t offset = GetParam();
    size_t sizeWritten = sizeof(cl_float);
    cl_uint numEventsInWaitList = 0;
    cl_event *eventWaitList = nullptr;
    cl_event *event = nullptr;

    GraphicsAllocation *allocation = pCommandStreamReceiver->createAllocationAndHandleResidency(pDestMemory, sizeof(destMemory));

    srcBuffer->forceDisallowCPUCopy = true;
    retVal = pCmdQ->enqueueReadBuffer(
        srcBuffer.get(),
        blockingRead,
        offset,
        sizeWritten,
        pDestMemory,
        numEventsInWaitList,
        eventWaitList,
        event);

    EXPECT_EQ(CL_SUCCESS, retVal);

    allocation = pCommandStreamReceiver->getMemoryManager()->graphicsAllocations.peekHead();
    while (allocation && allocation->getUnderlyingBuffer() != pDestMemory) {
        allocation = allocation->next;
    }
    retVal = pCmdQ->flush();
    EXPECT_EQ(CL_SUCCESS, retVal);

    pSrcMemory = ptrOffset(pSrcMemory, offset);

    cl_float *destGpuaddress = reinterpret_cast<cl_float *>(allocation->getGpuAddress());
    // Compute our memory expecations based on kernel execution
    size_t sizeUserMemory = sizeof(destMemory);
    AUBCommandStreamFixture::expectMemory<FamilyType>(destGpuaddress, pSrcMemory, sizeWritten);

    // If the copykernel wasn't max sized, ensure we didn't overwrite existing memory
    if (offset + sizeWritten < sizeUserMemory) {
        pDestMemory = ptrOffset(pDestMemory, sizeWritten);
        destGpuaddress = ptrOffset(destGpuaddress, sizeWritten);

        size_t sizeRemaining = sizeUserMemory - sizeWritten - offset;
        AUBCommandStreamFixture::expectMemory<FamilyType>(destGpuaddress, pDestMemory, sizeRemaining);
    }
}

INSTANTIATE_TEST_CASE_P(AUBReadBuffer_simple,
                        AUBReadBuffer,
                        ::testing::Values(
                            0 * sizeof(cl_float),
                            1 * sizeof(cl_float),
                            2 * sizeof(cl_float),
                            3 * sizeof(cl_float)));

HWTEST_F(AUBReadBuffer, reserveCanonicalGpuAddress) {
    if (!GetAubTestsConfig<FamilyType>().testCanonicalAddress) {
        return;
    }

    MockContext context(this->pDevice);

    cl_float srcMemory[] = {1.0f, 2.0f, 3.0f, 4.0f};
    cl_float dstMemory[] = {0.0f, 0.0f, 0.0f, 0.0f};
    GraphicsAllocation *srcAlocation = new GraphicsAllocation(srcMemory, 0xFFFF800400001000, 0xFFFF800400001000, sizeof(srcMemory));

    std::unique_ptr<Buffer> srcBuffer(Buffer::createBufferHw(&context,
                                                             CL_MEM_USE_HOST_PTR,
                                                             sizeof(srcMemory),
                                                             srcAlocation->getUnderlyingBuffer(),
                                                             srcMemory,
                                                             srcAlocation,
                                                             false,
                                                             false,
                                                             false));
    ASSERT_NE(nullptr, srcBuffer);

    srcBuffer->forceDisallowCPUCopy = true;
    auto retVal = pCmdQ->enqueueReadBuffer(srcBuffer.get(),
                                           CL_TRUE,
                                           0,
                                           sizeof(dstMemory),
                                           dstMemory,
                                           0,
                                           nullptr,
                                           nullptr);
    EXPECT_EQ(CL_SUCCESS, retVal);

    GraphicsAllocation *dstAllocation = pCommandStreamReceiver->createAllocationAndHandleResidency(dstMemory, sizeof(dstMemory));
    cl_float *dstGpuAddress = reinterpret_cast<cl_float *>(dstAllocation->getGpuAddress());

    AUBCommandStreamFixture::expectMemory<FamilyType>(dstGpuAddress, srcMemory, sizeof(dstMemory));
}
