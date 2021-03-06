/*
 * Copyright (C) 2017-2019 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include "runtime/command_stream/aub_command_stream_receiver.h"
#include "runtime/command_stream/command_stream_receiver.h"
#include "runtime/command_stream/command_stream_receiver_with_aub_dump.h"
#include "runtime/command_stream/device_command_stream.h"
#include "runtime/command_stream/linear_stream.h"
#include "runtime/command_stream/preemption.h"
#include "runtime/gen_common/hw_cmds.h"
#include "runtime/helpers/built_ins_helper.h"
#include "runtime/helpers/gmm_callbacks.h"
#include "runtime/helpers/flush_stamp.h"
#include "runtime/helpers/options.h"
#include "runtime/mem_obj/buffer.h"
#include "runtime/memory_manager/internal_allocation_storage.h"
#include "runtime/memory_manager/memory_manager.h"
#include "runtime/os_interface/windows/os_context_win.h"
#include "runtime/os_interface/windows/os_interface.h"
#include "runtime/os_interface/windows/wddm_device_command_stream.h"
#include "runtime/os_interface/windows/wddm_memory_manager.h"
#include "runtime/os_interface/windows/wddm_residency_controller.h"

#include "unit_tests/fixtures/device_fixture.h"
#include "unit_tests/fixtures/gmm_environment_fixture.h"
#include "unit_tests/fixtures/memory_management_fixture.h"
#include "unit_tests/helpers/debug_manager_state_restore.h"
#include "unit_tests/helpers/execution_environment_helper.h"
#include "unit_tests/mocks/mock_buffer.h"
#include "unit_tests/mocks/mock_builtins.h"
#include "unit_tests/mocks/mock_device.h"
#include "unit_tests/mocks/mock_gmm_page_table_mngr.h"
#include "unit_tests/mocks/mock_graphics_allocation.h"
#include "unit_tests/mocks/mock_program.h"
#include "unit_tests/mocks/mock_submissions_aggregator.h"
#include "unit_tests/mocks/mock_wddm_interface23.h"
#include "unit_tests/os_interface/windows/mock_gdi_interface.h"
#include "unit_tests/os_interface/windows/mock_wddm_memory_manager.h"
#include "unit_tests/os_interface/windows/wddm_fixture.h"

#include "test.h"

using namespace OCLRT;
using namespace ::testing;

class WddmCommandStreamFixture {
  public:
    std::unique_ptr<MockDevice> device;
    DeviceCommandStreamReceiver<DEFAULT_TEST_FAMILY_NAME> *csr;
    MockWddmMemoryManager *memoryManager = nullptr;
    WddmMock *wddm = nullptr;
    DebugManagerStateRestore stateRestore;

    virtual void SetUp() {
        HardwareInfo *hwInfo = nullptr;
        DebugManager.flags.CsrDispatchMode.set(static_cast<uint32_t>(DispatchMode::ImmediateDispatch));
        auto executionEnvironment = getExecutionEnvironmentImpl(hwInfo);
        wddm = static_cast<WddmMock *>(executionEnvironment->osInterface->get()->getWddm());

        csr = new WddmCommandStreamReceiver<DEFAULT_TEST_FAMILY_NAME>(*platformDevices[0], *executionEnvironment);

        memoryManager = new MockWddmMemoryManager(wddm, *executionEnvironment);
        executionEnvironment->memoryManager.reset(memoryManager);

        device.reset(MockDevice::create<MockDevice>(platformDevices[0], executionEnvironment, 0u));
        device->resetCommandStreamReceiver(csr);
        ASSERT_NE(nullptr, device);
    }

    virtual void TearDown() {
    }
};

template <typename GfxFamily>
struct MockWddmCsr : public WddmCommandStreamReceiver<GfxFamily> {
    MockWddmCsr(const HardwareInfo &hwInfoIn, ExecutionEnvironment &executionEnvironment)
        : WddmCommandStreamReceiver(hwInfoIn, executionEnvironment){};

    using CommandStreamReceiver::commandStream;
    using CommandStreamReceiver::dispatchMode;
    using CommandStreamReceiver::getCS;
    using WddmCommandStreamReceiver<GfxFamily>::commandBufferHeader;
    using WddmCommandStreamReceiver<GfxFamily>::pageTableManagerInitialized;

    void overrideDispatchPolicy(DispatchMode overrideValue) {
        this->dispatchMode = overrideValue;
    }

    SubmissionAggregator *peekSubmissionAggregator() {
        return this->submissionAggregator.get();
    }

    void overrideSubmissionAggregator(SubmissionAggregator *newSubmissionsAggregator) {
        this->submissionAggregator.reset(newSubmissionsAggregator);
    }

    void overrideRecorededCommandBuffer(Device &device) {
        recordedCommandBuffer = std::unique_ptr<CommandBuffer>(new CommandBuffer(device));
    }

    int flushCalledCount = 0;
    std::unique_ptr<CommandBuffer> recordedCommandBuffer = nullptr;
};

class WddmCommandStreamWithMockGdiFixture {
  public:
    MockWddmCsr<DEFAULT_TEST_FAMILY_NAME> *csr = nullptr;
    MemoryManager *memoryManager = nullptr;
    std::unique_ptr<MockDevice> device = nullptr;
    WddmMock *wddm = nullptr;
    MockGdi *gdi = nullptr;
    DebugManagerStateRestore stateRestore;
    GraphicsAllocation *preemptionAllocation = nullptr;

    virtual void SetUp() {
        HardwareInfo *hwInfo = nullptr;
        ExecutionEnvironment *executionEnvironment = getExecutionEnvironmentImpl(hwInfo);
        executionEnvironment->initGmm(*platformDevices);
        wddm = static_cast<WddmMock *>(executionEnvironment->osInterface->get()->getWddm());
        gdi = new MockGdi();
        wddm->gdi.reset(gdi);
        ASSERT_NE(wddm, nullptr);
        DebugManager.flags.CsrDispatchMode.set(static_cast<uint32_t>(DispatchMode::ImmediateDispatch));
        this->csr = new MockWddmCsr<DEFAULT_TEST_FAMILY_NAME>(*platformDevices[0], *executionEnvironment);
        memoryManager = csr->createMemoryManager(false, false);
        ASSERT_NE(nullptr, memoryManager);
        executionEnvironment->memoryManager.reset(memoryManager);
        device = std::unique_ptr<MockDevice>(Device::create<MockDevice>(platformDevices[0], executionEnvironment, 0u));
        device->resetCommandStreamReceiver(this->csr);
        ASSERT_NE(nullptr, device);
        this->csr->overrideRecorededCommandBuffer(*device);
        if (device->getPreemptionMode() == PreemptionMode::MidThread) {
            preemptionAllocation = memoryManager->allocateGraphicsMemoryWithProperties(MockAllocationProperties{MemoryConstants::pageSize});
        }
    }

    virtual void TearDown() {
        if (preemptionAllocation) {
            memoryManager->freeGraphicsMemory(preemptionAllocation);
        }
        wddm = nullptr;
    }
};

using WddmCommandStreamTest = ::Test<WddmCommandStreamFixture>;
using WddmCommandStreamMockGdiTest = ::Test<WddmCommandStreamWithMockGdiFixture>;
using WddmDefaultTest = ::Test<DeviceFixture>;
using DeviceCommandStreamTest = ::Test<GmmEnvironmentFixture>;

TEST_F(DeviceCommandStreamTest, CreateWddmCSR) {
    auto wddm = Wddm::createWddm();
    this->executionEnvironment.osInterface = std::make_unique<OSInterface>();
    this->executionEnvironment.osInterface->get()->setWddm(static_cast<WddmMock *>(wddm));
    std::unique_ptr<WddmCommandStreamReceiver<DEFAULT_TEST_FAMILY_NAME>> csr(static_cast<WddmCommandStreamReceiver<DEFAULT_TEST_FAMILY_NAME> *>(WddmCommandStreamReceiver<DEFAULT_TEST_FAMILY_NAME>::create(DEFAULT_TEST_PLATFORM::hwInfo, false, this->executionEnvironment)));
    EXPECT_NE(nullptr, csr);
    auto wddmFromCsr = csr->peekWddm();
    EXPECT_NE(nullptr, wddmFromCsr);
}

TEST_F(DeviceCommandStreamTest, CreateWddmCSRWithAubDump) {
    auto wddm = Wddm::createWddm();
    this->executionEnvironment.osInterface = std::make_unique<OSInterface>();
    this->executionEnvironment.osInterface->get()->setWddm(static_cast<WddmMock *>(wddm));
    std::unique_ptr<WddmCommandStreamReceiver<DEFAULT_TEST_FAMILY_NAME>> csr(static_cast<WddmCommandStreamReceiver<DEFAULT_TEST_FAMILY_NAME> *>(WddmCommandStreamReceiver<DEFAULT_TEST_FAMILY_NAME>::create(DEFAULT_TEST_PLATFORM::hwInfo, true, this->executionEnvironment)));
    EXPECT_NE(nullptr, csr);
    auto wddmFromCsr = csr->peekWddm();
    EXPECT_NE(nullptr, wddmFromCsr);
    auto aubCSR = static_cast<CommandStreamReceiverWithAUBDump<WddmCommandStreamReceiver<DEFAULT_TEST_FAMILY_NAME>> *>(csr.get())->aubCSR;
    EXPECT_NE(nullptr, aubCSR);
}

TEST_F(WddmCommandStreamTest, givenFlushStampWhenWaitCalledThenWaitForSpecifiedMonitoredFence) {
    uint64_t stampToWait = 123;
    csr->waitForFlushStamp(stampToWait);
    EXPECT_EQ(1u, wddm->waitFromCpuResult.called);
    EXPECT_TRUE(wddm->waitFromCpuResult.success);
    EXPECT_EQ(stampToWait, wddm->waitFromCpuResult.uint64ParamPassed);
}

TEST_F(WddmCommandStreamTest, Flush) {
    GraphicsAllocation *commandBuffer = memoryManager->allocateGraphicsMemoryWithProperties(MockAllocationProperties{MemoryConstants::pageSize});
    ASSERT_NE(nullptr, commandBuffer);
    LinearStream cs(commandBuffer);
    BatchBuffer batchBuffer{cs.getGraphicsAllocation(), 0, 0, nullptr, false, false, QueueThrottle::MEDIUM, cs.getUsed(), &cs};
    auto flushStamp = csr->flush(batchBuffer, csr->getResidencyAllocations());

    EXPECT_EQ(1u, wddm->submitResult.called);
    EXPECT_TRUE(wddm->submitResult.success);
    EXPECT_EQ(flushStamp, csr->getOsContext().get()->getResidencyController().getMonitoredFence().lastSubmittedFence);

    memoryManager->freeGraphicsMemory(commandBuffer);
}

TEST_F(WddmCommandStreamTest, givenGraphicsAllocationWithDifferentGpuAddressThenCpuAddressWhenSubmitIsCalledThenGpuAddressIsUsed) {
    GraphicsAllocation *commandBuffer = memoryManager->allocateGraphicsMemoryWithProperties(MockAllocationProperties{MemoryConstants::pageSize});

    auto cpuAddress = commandBuffer->getUnderlyingBuffer();
    uint64_t mockGpuAddres = 1337;
    commandBuffer->setCpuPtrAndGpuAddress(cpuAddress, mockGpuAddres);

    LinearStream cs(commandBuffer);
    BatchBuffer batchBuffer{cs.getGraphicsAllocation(), 0, 0, nullptr, false, false, QueueThrottle::MEDIUM, cs.getUsed(), &cs};
    auto flushStamp = csr->flush(batchBuffer, csr->getResidencyAllocations());
    EXPECT_EQ(mockGpuAddres, wddm->submitResult.commandBufferSubmitted);
    memoryManager->freeGraphicsMemory(commandBuffer);
}
TEST_F(WddmCommandStreamTest, FlushWithOffset) {
    auto offset = 128u;
    GraphicsAllocation *commandBuffer = memoryManager->allocateGraphicsMemoryWithProperties(MockAllocationProperties{MemoryConstants::pageSize});
    ASSERT_NE(nullptr, commandBuffer);
    LinearStream cs(commandBuffer);

    BatchBuffer batchBuffer{cs.getGraphicsAllocation(), offset, 0, nullptr, false, false, QueueThrottle::MEDIUM, cs.getUsed(), &cs};
    csr->flush(batchBuffer, csr->getResidencyAllocations());
    EXPECT_EQ(1u, wddm->submitResult.called);
    EXPECT_TRUE(wddm->submitResult.success);
    EXPECT_EQ(wddm->submitResult.commandBufferSubmitted, commandBuffer->getGpuAddress() + offset);

    memoryManager->freeGraphicsMemory(commandBuffer);
}

TEST_F(WddmCommandStreamTest, givenWdmmWhenSubmitIsCalledThenCoherencyRequiredFlagIsSetToFalse) {
    GraphicsAllocation *commandBuffer = memoryManager->allocateGraphicsMemoryWithProperties(MockAllocationProperties{MemoryConstants::pageSize});
    ASSERT_NE(nullptr, commandBuffer);
    LinearStream cs(commandBuffer);

    BatchBuffer batchBuffer{cs.getGraphicsAllocation(), 0, 0, nullptr, false, false, QueueThrottle::MEDIUM, cs.getUsed(), &cs};
    csr->flush(batchBuffer, csr->getResidencyAllocations());
    auto commandHeader = wddm->submitResult.commandHeaderSubmitted;

    COMMAND_BUFFER_HEADER *pHeader = reinterpret_cast<COMMAND_BUFFER_HEADER *>(commandHeader);

    EXPECT_FALSE(pHeader->RequiresCoherency);

    memoryManager->freeGraphicsMemory(commandBuffer);
}

TEST(WddmPreemptionHeaderTests, givenWddmCommandStreamReceiverWhenPreemptionIsOffWhenWorkloadIsSubmittedThenHeaderDoesntHavePreemptionFieldSet) {
    HardwareInfo *hwInfo = nullptr;
    std::unique_ptr<ExecutionEnvironment> executionEnvironment = std::unique_ptr<ExecutionEnvironment>(getExecutionEnvironmentImpl(hwInfo));
    hwInfo->capabilityTable.defaultPreemptionMode = PreemptionMode::Disabled;
    auto wddm = static_cast<WddmMock *>(executionEnvironment->osInterface->get()->getWddm());
    executionEnvironment->commandStreamReceivers.resize(1);
    executionEnvironment->commandStreamReceivers[0][0] =
        std::make_unique<MockWddmCsr<DEFAULT_TEST_FAMILY_NAME>>(hwInfo[0], *executionEnvironment);
    executionEnvironment->memoryManager.reset(executionEnvironment->commandStreamReceivers[0][0]->createMemoryManager(false, false));
    executionEnvironment->commandStreamReceivers[0][0]->overrideDispatchPolicy(DispatchMode::ImmediateDispatch);
    OsContext osContext(executionEnvironment->osInterface.get(), 0u, gpgpuEngineInstances[0], PreemptionHelper::getDefaultPreemptionMode(*hwInfo));
    executionEnvironment->commandStreamReceivers[0][0]->setupContext(osContext);

    auto commandBuffer = executionEnvironment->memoryManager->allocateGraphicsMemoryWithProperties(MockAllocationProperties{MemoryConstants::pageSize});
    LinearStream cs(commandBuffer);

    BatchBuffer batchBuffer{cs.getGraphicsAllocation(), 0, 0, nullptr, false, false, QueueThrottle::MEDIUM, cs.getUsed(), &cs};
    executionEnvironment->commandStreamReceivers[0][0]->flush(batchBuffer, executionEnvironment->commandStreamReceivers[0][0]->getResidencyAllocations());
    auto commandHeader = wddm->submitResult.commandHeaderSubmitted;
    COMMAND_BUFFER_HEADER *pHeader = reinterpret_cast<COMMAND_BUFFER_HEADER *>(commandHeader);

    EXPECT_FALSE(pHeader->NeedsMidBatchPreEmptionSupport);
    executionEnvironment->memoryManager->freeGraphicsMemory(commandBuffer);
}

TEST(WddmPreemptionHeaderTests, givenWddmCommandStreamReceiverWhenPreemptionIsOnWhenWorkloadIsSubmittedThenHeaderDoesHavePreemptionFieldSet) {
    HardwareInfo *hwInfo = nullptr;
    std::unique_ptr<ExecutionEnvironment> executionEnvironment = std::unique_ptr<ExecutionEnvironment>(getExecutionEnvironmentImpl(hwInfo));
    hwInfo->capabilityTable.defaultPreemptionMode = PreemptionMode::MidThread;
    auto wddm = static_cast<WddmMock *>(executionEnvironment->osInterface->get()->getWddm());
    executionEnvironment->commandStreamReceivers.resize(1);
    executionEnvironment->commandStreamReceivers[0][0] = std::make_unique<MockWddmCsr<DEFAULT_TEST_FAMILY_NAME>>(hwInfo[0],
                                                                                                                 *executionEnvironment);
    executionEnvironment->memoryManager.reset(executionEnvironment->commandStreamReceivers[0][0]->createMemoryManager(false, false));
    executionEnvironment->commandStreamReceivers[0][0]->overrideDispatchPolicy(DispatchMode::ImmediateDispatch);
    OsContext osContext(executionEnvironment->osInterface.get(), 0u, gpgpuEngineInstances[0], PreemptionHelper::getDefaultPreemptionMode(*hwInfo));
    executionEnvironment->commandStreamReceivers[0][0]->setupContext(osContext);

    auto commandBuffer = executionEnvironment->memoryManager->allocateGraphicsMemoryWithProperties(MockAllocationProperties{MemoryConstants::pageSize});
    LinearStream cs(commandBuffer);

    BatchBuffer batchBuffer{cs.getGraphicsAllocation(), 0, 0, nullptr, false, false, QueueThrottle::MEDIUM, cs.getUsed(), &cs};

    executionEnvironment->commandStreamReceivers[0][0]->flush(batchBuffer, executionEnvironment->commandStreamReceivers[0][0]->getResidencyAllocations());
    auto commandHeader = wddm->submitResult.commandHeaderSubmitted;
    COMMAND_BUFFER_HEADER *pHeader = reinterpret_cast<COMMAND_BUFFER_HEADER *>(commandHeader);

    EXPECT_TRUE(pHeader->NeedsMidBatchPreEmptionSupport);
    executionEnvironment->memoryManager->freeGraphicsMemory(commandBuffer);
}

TEST(WddmPreemptionHeaderTests, givenDeviceSupportingPreemptionWhenCommandStreamReceiverIsCreatedThenHeaderContainsPreemptionFieldSet) {
    HardwareInfo *hwInfo = nullptr;
    std::unique_ptr<ExecutionEnvironment> executionEnvironment = std::unique_ptr<ExecutionEnvironment>(getExecutionEnvironmentImpl(hwInfo));
    hwInfo->capabilityTable.defaultPreemptionMode = PreemptionMode::MidThread;
    auto commandStreamReceiver = std::make_unique<MockWddmCsr<DEFAULT_TEST_FAMILY_NAME>>(hwInfo[0], *executionEnvironment);
    auto commandHeader = commandStreamReceiver->commandBufferHeader;
    auto header = reinterpret_cast<COMMAND_BUFFER_HEADER *>(commandHeader);
    EXPECT_TRUE(header->NeedsMidBatchPreEmptionSupport);
}

TEST(WddmPreemptionHeaderTests, givenDevicenotSupportingPreemptionWhenCommandStreamReceiverIsCreatedThenHeaderPreemptionFieldIsNotSet) {
    HardwareInfo *hwInfo = nullptr;
    std::unique_ptr<ExecutionEnvironment> executionEnvironment = std::unique_ptr<ExecutionEnvironment>(getExecutionEnvironmentImpl(hwInfo));
    hwInfo->capabilityTable.defaultPreemptionMode = PreemptionMode::Disabled;
    auto commandStreamReceiver = std::make_unique<MockWddmCsr<DEFAULT_TEST_FAMILY_NAME>>(hwInfo[0], *executionEnvironment);
    auto commandHeader = commandStreamReceiver->commandBufferHeader;
    auto header = reinterpret_cast<COMMAND_BUFFER_HEADER *>(commandHeader);
    EXPECT_FALSE(header->NeedsMidBatchPreEmptionSupport);
}

TEST_F(WddmCommandStreamTest, givenWdmmWhenSubmitIsCalledAndThrottleIsToLowThenSetHeaderFieldsProperly) {
    GraphicsAllocation *commandBuffer = memoryManager->allocateGraphicsMemoryWithProperties(MockAllocationProperties{MemoryConstants::pageSize});
    ASSERT_NE(nullptr, commandBuffer);
    LinearStream cs(commandBuffer);

    BatchBuffer batchBuffer{cs.getGraphicsAllocation(), 0, 0, nullptr, false, false, QueueThrottle::LOW, cs.getUsed(), &cs};
    csr->flush(batchBuffer, csr->getResidencyAllocations());
    auto commandHeader = wddm->submitResult.commandHeaderSubmitted;

    COMMAND_BUFFER_HEADER *pHeader = reinterpret_cast<COMMAND_BUFFER_HEADER *>(commandHeader);

    EXPECT_EQ(0, pHeader->UmdRequestedSliceState);
    EXPECT_EQ(1, pHeader->UmdRequestedSubsliceCount);
    EXPECT_EQ(wddm->getGtSysInfo()->EUCount / wddm->getGtSysInfo()->SubSliceCount, pHeader->UmdRequestedEUCount);

    memoryManager->freeGraphicsMemory(commandBuffer);
}

TEST_F(WddmCommandStreamTest, givenWdmmWhenSubmitIsCalledAndThrottleIsToMediumThenSetHeaderFieldsProperly) {
    GraphicsAllocation *commandBuffer = memoryManager->allocateGraphicsMemoryWithProperties(MockAllocationProperties{MemoryConstants::pageSize});
    ASSERT_NE(nullptr, commandBuffer);
    LinearStream cs(commandBuffer);

    BatchBuffer batchBuffer{cs.getGraphicsAllocation(), 0, 0, nullptr, false, false, QueueThrottle::MEDIUM, cs.getUsed(), &cs};
    csr->flush(batchBuffer, csr->getResidencyAllocations());
    auto commandHeader = wddm->submitResult.commandHeaderSubmitted;

    COMMAND_BUFFER_HEADER *pHeader = reinterpret_cast<COMMAND_BUFFER_HEADER *>(commandHeader);

    EXPECT_EQ(0, pHeader->UmdRequestedSliceState);
    EXPECT_EQ(0, pHeader->UmdRequestedSubsliceCount);
    EXPECT_EQ(wddm->getGtSysInfo()->EUCount / wddm->getGtSysInfo()->SubSliceCount, pHeader->UmdRequestedEUCount);

    memoryManager->freeGraphicsMemory(commandBuffer);
}

TEST_F(WddmCommandStreamTest, givenWdmmWhenSubmitIsCalledAndThrottleIsToHighThenSetHeaderFieldsProperly) {
    GraphicsAllocation *commandBuffer = memoryManager->allocateGraphicsMemoryWithProperties(MockAllocationProperties{MemoryConstants::pageSize});
    ASSERT_NE(nullptr, commandBuffer);
    LinearStream cs(commandBuffer);

    BatchBuffer batchBuffer{cs.getGraphicsAllocation(), 0, 0, nullptr, false, false, QueueThrottle::HIGH, cs.getUsed(), &cs};
    csr->flush(batchBuffer, csr->getResidencyAllocations());
    auto commandHeader = wddm->submitResult.commandHeaderSubmitted;

    COMMAND_BUFFER_HEADER *pHeader = reinterpret_cast<COMMAND_BUFFER_HEADER *>(commandHeader);
    const uint32_t maxRequestedSubsliceCount = 7;
    EXPECT_EQ(0, pHeader->UmdRequestedSliceState);
    EXPECT_EQ((wddm->getGtSysInfo()->SubSliceCount <= maxRequestedSubsliceCount) ? wddm->getGtSysInfo()->SubSliceCount : 0, pHeader->UmdRequestedSubsliceCount);
    EXPECT_EQ(wddm->getGtSysInfo()->EUCount / wddm->getGtSysInfo()->SubSliceCount, pHeader->UmdRequestedEUCount);

    memoryManager->freeGraphicsMemory(commandBuffer);
}

TEST_F(WddmCommandStreamTest, givenWddmWithKmDafDisabledWhenFlushIsCalledWithAllocationsForResidencyThenNoneAllocationShouldBeKmDafLocked) {
    GraphicsAllocation *commandBuffer = memoryManager->allocateGraphicsMemoryWithProperties(MockAllocationProperties{MemoryConstants::pageSize});
    ASSERT_NE(nullptr, commandBuffer);
    LinearStream cs(commandBuffer);
    BatchBuffer batchBuffer{cs.getGraphicsAllocation(), 0, 0, nullptr, false, false, QueueThrottle::MEDIUM, cs.getUsed(), &cs};

    auto linearStreamAllocation = memoryManager->allocateGraphicsMemoryWithProperties(MockAllocationProperties{MemoryConstants::pageSize});
    ASSERT_NE(nullptr, linearStreamAllocation);
    linearStreamAllocation->setAllocationType(GraphicsAllocation::AllocationType::LINEAR_STREAM);
    ResidencyContainer allocationsForResidency = {linearStreamAllocation};

    EXPECT_FALSE(wddm->isKmDafEnabled());
    auto flushStamp = csr->flush(batchBuffer, allocationsForResidency);

    EXPECT_EQ(0u, wddm->kmDafLockResult.called);
    EXPECT_EQ(0u, wddm->kmDafLockResult.lockedAllocations.size());

    memoryManager->freeGraphicsMemory(commandBuffer);
    memoryManager->freeGraphicsMemory(linearStreamAllocation);
}

TEST_F(WddmCommandStreamTest, givenWddmWithKmDafEnabledWhenFlushIsCalledWithoutAllocationsForResidencyThenNoneAllocationShouldBeKmDafLocked) {
    GraphicsAllocation *commandBuffer = memoryManager->allocateGraphicsMemoryWithProperties(MockAllocationProperties{MemoryConstants::pageSize});
    ASSERT_NE(nullptr, commandBuffer);
    LinearStream cs(commandBuffer);
    BatchBuffer batchBuffer{cs.getGraphicsAllocation(), 0, 0, nullptr, false, false, QueueThrottle::MEDIUM, cs.getUsed(), &cs};

    wddm->setKmDafEnabled(true);
    auto flushStamp = csr->flush(batchBuffer, csr->getResidencyAllocations());

    EXPECT_EQ(0u, wddm->kmDafLockResult.called);
    EXPECT_EQ(0u, wddm->kmDafLockResult.lockedAllocations.size());

    memoryManager->freeGraphicsMemory(commandBuffer);
}

TEST_F(WddmCommandStreamTest, givenWddmWithKmDafEnabledWhenFlushIsCalledWithResidencyAllocationsInMemoryManagerThenLinearStreamAllocationsShouldBeKmDafLocked) {
    GraphicsAllocation *commandBuffer = memoryManager->allocateGraphicsMemoryWithProperties(MockAllocationProperties{MemoryConstants::pageSize});
    ASSERT_NE(nullptr, commandBuffer);
    LinearStream cs(commandBuffer);
    BatchBuffer batchBuffer{cs.getGraphicsAllocation(), 0, 0, nullptr, false, false, QueueThrottle::MEDIUM, cs.getUsed(), &cs};

    auto linearStreamAllocation = memoryManager->allocateGraphicsMemoryWithProperties(MockAllocationProperties{MemoryConstants::pageSize});
    ASSERT_NE(nullptr, linearStreamAllocation);
    linearStreamAllocation->setAllocationType(GraphicsAllocation::AllocationType::LINEAR_STREAM);

    csr->makeResident(*linearStreamAllocation);
    EXPECT_EQ(1u, csr->getResidencyAllocations().size());
    EXPECT_EQ(linearStreamAllocation, csr->getResidencyAllocations()[0]);

    wddm->setKmDafEnabled(true);
    auto flushStamp = csr->flush(batchBuffer, csr->getResidencyAllocations());

    EXPECT_EQ(1u, wddm->kmDafLockResult.called);
    EXPECT_EQ(1u, wddm->kmDafLockResult.lockedAllocations.size());
    EXPECT_EQ(linearStreamAllocation, wddm->kmDafLockResult.lockedAllocations[0]);

    memoryManager->freeGraphicsMemory(commandBuffer);
    memoryManager->freeGraphicsMemory(linearStreamAllocation);
}

TEST_F(WddmCommandStreamTest, givenWddmWithKmDafEnabledWhenFlushIsCalledWithAllocationsForResidencyThenLinearStreamAllocationsShouldBeKmDafLocked) {
    GraphicsAllocation *commandBuffer = memoryManager->allocateGraphicsMemoryWithProperties(MockAllocationProperties{MemoryConstants::pageSize});
    ASSERT_NE(nullptr, commandBuffer);
    LinearStream cs(commandBuffer);
    BatchBuffer batchBuffer{cs.getGraphicsAllocation(), 0, 0, nullptr, false, false, QueueThrottle::MEDIUM, cs.getUsed(), &cs};

    auto linearStreamAllocation = memoryManager->allocateGraphicsMemoryWithProperties(MockAllocationProperties{MemoryConstants::pageSize});
    ASSERT_NE(nullptr, linearStreamAllocation);
    linearStreamAllocation->setAllocationType(GraphicsAllocation::AllocationType::LINEAR_STREAM);
    ResidencyContainer allocationsForResidency = {linearStreamAllocation};

    wddm->setKmDafEnabled(true);
    auto flushStamp = csr->flush(batchBuffer, allocationsForResidency);

    EXPECT_EQ(1u, wddm->kmDafLockResult.called);
    EXPECT_EQ(1u, wddm->kmDafLockResult.lockedAllocations.size());
    EXPECT_EQ(linearStreamAllocation, wddm->kmDafLockResult.lockedAllocations[0]);

    memoryManager->freeGraphicsMemory(commandBuffer);
    memoryManager->freeGraphicsMemory(linearStreamAllocation);
}

TEST_F(WddmCommandStreamTest, givenWddmWithKmDafEnabledWhenFlushIsCalledWithAllocationsForResidencyThenFillPatternAllocationsShouldBeKmDafLocked) {
    GraphicsAllocation *commandBuffer = memoryManager->allocateGraphicsMemoryWithProperties(MockAllocationProperties{MemoryConstants::pageSize});
    ASSERT_NE(nullptr, commandBuffer);
    LinearStream cs(commandBuffer);
    BatchBuffer batchBuffer{cs.getGraphicsAllocation(), 0, 0, nullptr, false, false, QueueThrottle::MEDIUM, cs.getUsed(), &cs};

    auto fillPatternAllocation = memoryManager->allocateGraphicsMemoryWithProperties(MockAllocationProperties{MemoryConstants::pageSize});
    ASSERT_NE(nullptr, fillPatternAllocation);
    fillPatternAllocation->setAllocationType(GraphicsAllocation::AllocationType::FILL_PATTERN);
    ResidencyContainer allocationsForResidency = {fillPatternAllocation};

    wddm->setKmDafEnabled(true);
    auto flushStamp = csr->flush(batchBuffer, allocationsForResidency);

    EXPECT_EQ(1u, wddm->kmDafLockResult.called);
    EXPECT_EQ(1u, wddm->kmDafLockResult.lockedAllocations.size());
    EXPECT_EQ(fillPatternAllocation, wddm->kmDafLockResult.lockedAllocations[0]);

    memoryManager->freeGraphicsMemory(commandBuffer);
    memoryManager->freeGraphicsMemory(fillPatternAllocation);
}

TEST_F(WddmCommandStreamTest, givenWddmWithKmDafEnabledWhenFlushIsCalledWithAllocationsForResidencyThenNonLinearStreamAllocationShouldNotBeKmDafLocked) {
    GraphicsAllocation *commandBuffer = memoryManager->allocateGraphicsMemoryWithProperties(MockAllocationProperties{MemoryConstants::pageSize});
    ASSERT_NE(nullptr, commandBuffer);
    LinearStream cs(commandBuffer);
    BatchBuffer batchBuffer{cs.getGraphicsAllocation(), 0, 0, nullptr, false, false, QueueThrottle::MEDIUM, cs.getUsed(), &cs};

    auto nonLinearStreamAllocation = memoryManager->allocateGraphicsMemoryWithProperties(MockAllocationProperties{MemoryConstants::pageSize});
    ASSERT_NE(nullptr, nonLinearStreamAllocation);
    ResidencyContainer allocationsForResidency = {nonLinearStreamAllocation};

    wddm->setKmDafEnabled(true);
    auto flushStamp = csr->flush(batchBuffer, allocationsForResidency);

    EXPECT_EQ(0u, wddm->kmDafLockResult.called);
    EXPECT_EQ(0u, wddm->kmDafLockResult.lockedAllocations.size());

    memoryManager->freeGraphicsMemory(commandBuffer);
    memoryManager->freeGraphicsMemory(nonLinearStreamAllocation);
}

TEST_F(WddmCommandStreamTest, makeResident) {
    WddmMemoryManager *wddmMM = reinterpret_cast<WddmMemoryManager *>(memoryManager);

    GraphicsAllocation *commandBuffer = memoryManager->allocateGraphicsMemoryWithProperties(MockAllocationProperties{MemoryConstants::pageSize});
    ASSERT_NE(nullptr, commandBuffer);
    LinearStream cs(commandBuffer);

    csr->makeResident(*commandBuffer);

    EXPECT_EQ(0u, wddm->makeResidentResult.called);
    EXPECT_EQ(1u, csr->getResidencyAllocations().size());
    EXPECT_EQ(commandBuffer, csr->getResidencyAllocations()[0]);

    memoryManager->freeGraphicsMemory(commandBuffer);
}

TEST_F(WddmCommandStreamTest, makeNonResidentPutsAllocationInEvictionAllocations) {
    WddmMemoryManager *wddmMM = reinterpret_cast<WddmMemoryManager *>(memoryManager);

    GraphicsAllocation *commandBuffer = memoryManager->allocateGraphicsMemoryWithProperties(MockAllocationProperties{MemoryConstants::pageSize});
    ASSERT_NE(nullptr, commandBuffer);
    LinearStream cs(commandBuffer);

    csr->makeResident(*cs.getGraphicsAllocation());

    csr->makeNonResident(*commandBuffer);

    EXPECT_EQ(1u, csr->getEvictionAllocations().size());

    memoryManager->freeGraphicsMemory(commandBuffer);
}

TEST_F(WddmCommandStreamTest, processEvictionPlacesAllAllocationsOnTrimCandidateList) {
    WddmMemoryManager *wddmMM = reinterpret_cast<WddmMemoryManager *>(memoryManager);

    GraphicsAllocation *allocation = memoryManager->allocateGraphicsMemoryWithProperties(MockAllocationProperties{MemoryConstants::pageSize});
    GraphicsAllocation *allocation2 = memoryManager->allocateGraphicsMemoryWithProperties(MockAllocationProperties{MemoryConstants::pageSize});
    ASSERT_NE(nullptr, allocation);
    ASSERT_NE(nullptr, allocation2);

    csr->getEvictionAllocations().push_back(allocation);
    csr->getEvictionAllocations().push_back(allocation2);

    EXPECT_EQ(2u, csr->getEvictionAllocations().size());

    csr->processEviction();

    EXPECT_EQ(2u, csr->getOsContext().get()->getResidencyController().peekTrimCandidateList().size());

    memoryManager->freeGraphicsMemory(allocation);
    memoryManager->freeGraphicsMemory(allocation2);
}

TEST_F(WddmCommandStreamTest, processEvictionClearsEvictionAllocations) {
    WddmMemoryManager *wddmMM = reinterpret_cast<WddmMemoryManager *>(memoryManager);

    GraphicsAllocation *allocation = memoryManager->allocateGraphicsMemoryWithProperties(MockAllocationProperties{MemoryConstants::pageSize});
    ASSERT_NE(nullptr, allocation);

    csr->getEvictionAllocations().push_back(allocation);

    EXPECT_EQ(1u, csr->getEvictionAllocations().size());

    csr->processEviction();

    EXPECT_EQ(0u, csr->getEvictionAllocations().size());

    memoryManager->freeGraphicsMemory(allocation);
}

TEST_F(WddmCommandStreamTest, makeResidentNonResidentMemObj) {
    GraphicsAllocation *gfxAllocation = memoryManager->allocateGraphicsMemoryWithProperties(MockAllocationProperties{MemoryConstants::pageSize});
    Buffer *buffer = new AlignedBuffer(gfxAllocation);
    WddmMemoryManager *wddmMM = reinterpret_cast<WddmMemoryManager *>(memoryManager);

    csr->makeResident(*buffer->getGraphicsAllocation());
    EXPECT_EQ(0u, wddm->makeResidentResult.called);
    EXPECT_EQ(1u, csr->getResidencyAllocations().size());
    EXPECT_EQ(gfxAllocation, csr->getResidencyAllocations()[0]);

    csr->makeNonResident(*buffer->getGraphicsAllocation());
    EXPECT_EQ(gfxAllocation, csr->getEvictionAllocations()[0]);

    delete buffer;
    memoryManager->freeGraphicsMemory(gfxAllocation);
}

TEST_F(WddmCommandStreamTest, givenGraphicsAllocationWhenMakeResidentThenAllocationIsInResidencyContainer) {
    void *hostPtr = reinterpret_cast<void *>(wddm->virtualAllocAddress + 0x1234);
    auto size = 1234u;

    auto gfxAllocation = memoryManager->allocateGraphicsMemory(MockAllocationProperties{false, size}, hostPtr);

    ASSERT_NE(nullptr, gfxAllocation);

    csr->makeResidentHostPtrAllocation(gfxAllocation);

    EXPECT_EQ(1u, csr->getResidencyAllocations().size());
    EXPECT_EQ(hostPtr, gfxAllocation->getUnderlyingBuffer());

    memoryManager->freeGraphicsMemory(gfxAllocation);
}

TEST_F(WddmCommandStreamTest, givenHostPtrWhenPtrBelowRestrictionThenCreateAllocationAndMakeResident) {
    void *hostPtr = reinterpret_cast<void *>(memoryManager->getAlignedMallocRestrictions()->minAddress - 0x1000);
    auto size = 0x2000u;

    auto gfxAllocation = static_cast<WddmAllocation *>(memoryManager->allocateGraphicsMemory(MockAllocationProperties{false, size}, hostPtr));

    void *expectedReserve = reinterpret_cast<void *>(wddm->virtualAllocAddress);

    ASSERT_NE(nullptr, gfxAllocation);

    csr->makeResidentHostPtrAllocation(gfxAllocation);

    EXPECT_EQ(1u, csr->getResidencyAllocations().size());
    EXPECT_EQ(hostPtr, gfxAllocation->getUnderlyingBuffer());
    EXPECT_EQ(expectedReserve, gfxAllocation->getReservedAddress());

    memoryManager->freeGraphicsMemory(gfxAllocation);
}

TEST_F(WddmCommandStreamTest, givenTwoTemporaryAllocationsWhenCleanTemporaryAllocationListThenDestoryOnlyCompletedAllocations) {
    void *host_ptr = (void *)0x1212341;
    void *host_ptr2 = (void *)0x2212341;
    auto size = 17262u;

    GraphicsAllocation *graphicsAllocation = memoryManager->allocateGraphicsMemory(MockAllocationProperties{false, size}, host_ptr);
    GraphicsAllocation *graphicsAllocation2 = memoryManager->allocateGraphicsMemory(MockAllocationProperties{false, size}, host_ptr2);
    csr->getInternalAllocationStorage()->storeAllocation(std::unique_ptr<GraphicsAllocation>(graphicsAllocation), TEMPORARY_ALLOCATION);
    csr->getInternalAllocationStorage()->storeAllocation(std::unique_ptr<GraphicsAllocation>(graphicsAllocation2), TEMPORARY_ALLOCATION);

    graphicsAllocation->updateTaskCount(1, csr->getOsContext().getContextId());
    graphicsAllocation2->updateTaskCount(100, csr->getOsContext().getContextId());

    csr->waitForTaskCountAndCleanAllocationList(1, TEMPORARY_ALLOCATION);
    // graphicsAllocation2 still lives
    EXPECT_EQ(host_ptr2, graphicsAllocation2->getUnderlyingBuffer());

    auto hostPtrManager = memoryManager->getHostPtrManager();

    auto alignedPtr = alignDown(host_ptr, MemoryConstants::pageSize);
    auto alignedPtr2 = alignDown(host_ptr2, MemoryConstants::pageSize);

    auto fragment = hostPtrManager->getFragment(alignedPtr2);
    ASSERT_NE(nullptr, fragment);

    EXPECT_EQ(alignedPtr2, fragment->fragmentCpuPointer);

    auto fragment2 = hostPtrManager->getFragment(alignedPtr);
    EXPECT_EQ(nullptr, fragment2);
    // destroy remaining allocation
    csr->waitForTaskCountAndCleanAllocationList(100, TEMPORARY_ALLOCATION);
}

TEST_F(WddmCommandStreamMockGdiTest, FlushCallsWddmMakeResidentForResidencyAllocations) {
    GraphicsAllocation *commandBuffer = memoryManager->allocateGraphicsMemoryWithProperties(MockAllocationProperties{MemoryConstants::pageSize});
    ASSERT_NE(nullptr, commandBuffer);
    LinearStream cs(commandBuffer);

    csr->makeResident(*commandBuffer);

    EXPECT_EQ(1u, csr->getResidencyAllocations().size());

    gdi->getMakeResidentArg().NumAllocations = 0;

    BatchBuffer batchBuffer{cs.getGraphicsAllocation(), 0, 0, nullptr, false, false, QueueThrottle::MEDIUM, cs.getUsed(), &cs};
    csr->flush(batchBuffer, csr->getResidencyAllocations());

    EXPECT_NE(0u, gdi->getMakeResidentArg().NumAllocations);

    memoryManager->freeGraphicsMemory(commandBuffer);
}

TEST_F(WddmCommandStreamMockGdiTest, makeResidentClearsResidencyAllocations) {
    GraphicsAllocation *commandBuffer = memoryManager->allocateGraphicsMemoryWithProperties(MockAllocationProperties{MemoryConstants::pageSize});
    ASSERT_NE(nullptr, commandBuffer);
    LinearStream cs(commandBuffer);

    csr->makeResident(*commandBuffer);

    EXPECT_EQ(1u, csr->getResidencyAllocations().size());
    EXPECT_EQ(0u, csr->getEvictionAllocations().size());

    EXPECT_EQ(trimListUnusedPosition, ((WddmAllocation *)commandBuffer)->getTrimCandidateListPosition(csr->getOsContext().getContextId()));

    csr->processResidency(csr->getResidencyAllocations());

    csr->makeSurfacePackNonResident(csr->getResidencyAllocations());

    EXPECT_EQ(0u, csr->getResidencyAllocations().size());
    EXPECT_EQ(0u, csr->getEvictionAllocations().size());

    EXPECT_EQ(0u, ((WddmAllocation *)commandBuffer)->getTrimCandidateListPosition(csr->getOsContext().getContextId()));

    memoryManager->freeGraphicsMemory(commandBuffer);
}

HWTEST_F(WddmCommandStreamMockGdiTest, givenRecordedCommandBufferWhenItIsSubmittedThenFlushTaskIsProperlyCalled) {
    //preemption allocation + sip allocation
    size_t csrSurfaceCount = 0;
    GraphicsAllocation *tmpAllocation = nullptr;
    if (device->getPreemptionMode() == PreemptionMode::MidThread) {
        csrSurfaceCount = 2;
        tmpAllocation = GlobalMockSipProgram::sipProgram->getAllocation();
        GlobalMockSipProgram::sipProgram->resetAllocation(memoryManager->allocateGraphicsMemoryWithProperties(MockAllocationProperties{MemoryConstants::pageSize}));
    }

    csr->overrideDispatchPolicy(DispatchMode::BatchedDispatch);

    auto mockedSubmissionsAggregator = new mockSubmissionsAggregator();
    csr->overrideSubmissionAggregator(mockedSubmissionsAggregator);

    auto commandBuffer = memoryManager->allocateGraphicsMemoryWithProperties(MockAllocationProperties{MemoryConstants::pageSize});
    auto dshAlloc = memoryManager->allocateGraphicsMemoryWithProperties(MockAllocationProperties{MemoryConstants::pageSize});
    auto iohAlloc = memoryManager->allocateGraphicsMemoryWithProperties(MockAllocationProperties{MemoryConstants::pageSize});
    auto sshAlloc = memoryManager->allocateGraphicsMemoryWithProperties(MockAllocationProperties{MemoryConstants::pageSize});

    auto tagAllocation = csr->getTagAllocation();
    csr->setPreemptionCsrAllocation(preemptionAllocation);

    LinearStream cs(commandBuffer);
    IndirectHeap dsh(dshAlloc);
    IndirectHeap ioh(iohAlloc);
    IndirectHeap ssh(sshAlloc);

    DispatchFlags dispatchFlags;
    dispatchFlags.guardCommandBufferWithPipeControl = true;
    dispatchFlags.requiresCoherency = true;
    dispatchFlags.preemptionMode = PreemptionHelper::getDefaultPreemptionMode(device->getHardwareInfo());
    csr->flushTask(cs, 0u, dsh, ioh, ssh, 0u, dispatchFlags, *device);

    auto &cmdBuffers = mockedSubmissionsAggregator->peekCommandBuffers();
    auto storedCommandBuffer = cmdBuffers.peekHead();

    ResidencyContainer copyOfResidency = storedCommandBuffer->surfaces;
    copyOfResidency.push_back(storedCommandBuffer->batchBuffer.commandBufferAllocation);

    csr->flushBatchedSubmissions();

    EXPECT_TRUE(cmdBuffers.peekIsEmpty());

    EXPECT_EQ(1u, wddm->submitResult.called);
    auto csrCommandStream = csr->commandStream.getGraphicsAllocation();
    EXPECT_EQ(csrCommandStream->getGpuAddress(), wddm->submitResult.commandBufferSubmitted);
    EXPECT_TRUE(((COMMAND_BUFFER_HEADER *)wddm->submitResult.commandHeaderSubmitted)->RequiresCoherency);
    EXPECT_EQ(6u + csrSurfaceCount, wddm->makeResidentResult.handleCount);

    std::vector<D3DKMT_HANDLE> expectedHandles;
    expectedHandles.push_back(((WddmAllocation *)tagAllocation)->handle);
    expectedHandles.push_back(((WddmAllocation *)commandBuffer)->handle);
    expectedHandles.push_back(((WddmAllocation *)dshAlloc)->handle);
    expectedHandles.push_back(((WddmAllocation *)iohAlloc)->handle);
    expectedHandles.push_back(((WddmAllocation *)sshAlloc)->handle);
    expectedHandles.push_back(((WddmAllocation *)csrCommandStream)->handle);

    for (auto i = 0u; i < wddm->makeResidentResult.handleCount; i++) {
        auto handle = wddm->makeResidentResult.handlePack[i];
        auto found = false;
        for (auto &expectedHandle : expectedHandles) {
            if (expectedHandle == handle) {
                found = true;
            }
        }
        EXPECT_TRUE(found);
    }

    EXPECT_NE(trimListUnusedPosition, ((WddmAllocation *)tagAllocation)->getTrimCandidateListPosition(csr->getOsContext().getContextId()));
    EXPECT_NE(trimListUnusedPosition, ((WddmAllocation *)commandBuffer)->getTrimCandidateListPosition(csr->getOsContext().getContextId()));
    EXPECT_EQ(trimListUnusedPosition, ((WddmAllocation *)dshAlloc)->getTrimCandidateListPosition(csr->getOsContext().getContextId()));
    EXPECT_EQ(trimListUnusedPosition, ((WddmAllocation *)iohAlloc)->getTrimCandidateListPosition(csr->getOsContext().getContextId()));
    EXPECT_NE(trimListUnusedPosition, ((WddmAllocation *)sshAlloc)->getTrimCandidateListPosition(csr->getOsContext().getContextId()));
    EXPECT_NE(trimListUnusedPosition, ((WddmAllocation *)csrCommandStream)->getTrimCandidateListPosition(csr->getOsContext().getContextId()));

    memoryManager->freeGraphicsMemory(dshAlloc);
    memoryManager->freeGraphicsMemory(iohAlloc);
    memoryManager->freeGraphicsMemory(sshAlloc);
    memoryManager->freeGraphicsMemory(commandBuffer);
    if (device->getPreemptionMode() == PreemptionMode::MidThread) {
        memoryManager->freeGraphicsMemory(GlobalMockSipProgram::sipProgram->getAllocation());
        GlobalMockSipProgram::sipProgram->resetAllocation(tmpAllocation);
    }
}

using WddmSimpleTest = ::testing::Test;

HWTEST_F(WddmSimpleTest, givenDefaultWddmCsrWhenItIsCreatedThenBatchingIsTurnedOn) {
    DebugManager.flags.CsrDispatchMode.set(0);
    ExecutionEnvironment *executionEnvironment = new ExecutionEnvironment;
    std::unique_ptr<MockDevice> device(Device::create<MockDevice>(platformDevices[0], executionEnvironment, 0u));
    auto wddm = Wddm::createWddm();
    executionEnvironment->osInterface = std::make_unique<OSInterface>();
    executionEnvironment->osInterface->get()->setWddm(wddm);
    std::unique_ptr<MockWddmCsr<FamilyType>> mockCsr(new MockWddmCsr<FamilyType>(*platformDevices[0], *executionEnvironment));
    EXPECT_EQ(DispatchMode::BatchedDispatch, mockCsr->dispatchMode);
}

HWTEST_F(WddmDefaultTest, givenFtrWddmHwQueuesFlagWhenCreatingCsrThenPickWddmVersionBasingOnFtrFlag) {
    HardwareInfo myHwInfo = *platformDevices[0];
    FeatureTable myFtrTable = *myHwInfo.pSkuTable;
    myHwInfo.pSkuTable = &myFtrTable;
    auto wddm = Wddm::createWddm();
    pDevice->executionEnvironment->osInterface = std::make_unique<OSInterface>();
    pDevice->executionEnvironment->osInterface->get()->setWddm(wddm);
    WddmCommandStreamReceiver<FamilyType> wddmCsr(myHwInfo, *pDevice->executionEnvironment);

    auto wddmFromCsr = wddmCsr.peekWddm();
    EXPECT_EQ(typeid(*wddmFromCsr), typeid(WddmMock));
}

struct WddmCsrCompressionTests : ::testing::Test {
    void setCompressionEnabled(bool enableForBuffer, bool enableForImages) {
        RuntimeCapabilityTable capabilityTable = platformDevices[0]->capabilityTable;
        capabilityTable.ftrRenderCompressedBuffers = enableForBuffer;
        capabilityTable.ftrRenderCompressedImages = enableForImages;
        hwInfo->capabilityTable = capabilityTable;
    }

    HardwareInfo *hwInfo = nullptr;
    WddmMock *myMockWddm;
};

HWTEST_F(WddmCsrCompressionTests, givenEnabledCompressionWhenInitializedThenCreatePagetableMngr) {
    bool compressionEnabled[2][2] = {{true, false}, {false, true}};
    for (size_t i = 0; i < 2; i++) {
        ExecutionEnvironment *executionEnvironment = getExecutionEnvironmentImpl(hwInfo);
        std::unique_ptr<MockDevice> device(Device::create<MockDevice>(hwInfo, executionEnvironment, 0u));
        setCompressionEnabled(compressionEnabled[i][0], compressionEnabled[i][1]);
        myMockWddm = static_cast<WddmMock *>(executionEnvironment->osInterface->get()->getWddm());
        EXPECT_EQ(nullptr, myMockWddm->getPageTableManager());

        MockWddmCsr<FamilyType> mockWddmCsr(hwInfo[0], *executionEnvironment);
        mockWddmCsr.createPageTableManager();
        ASSERT_NE(nullptr, myMockWddm->getPageTableManager());

        auto mockMngr = reinterpret_cast<MockGmmPageTableMngr *>(myMockWddm->getPageTableManager());

        GMM_DEVICE_CALLBACKS_INT expectedDeviceCb = {};
        GMM_TRANSLATIONTABLE_CALLBACKS expectedTTCallbacks = {};
        unsigned int expectedFlags = (TT_TYPE::TRTT | TT_TYPE::AUXTT);
        auto myGdi = myMockWddm->getGdi();
        // clang-format off
        expectedDeviceCb.Adapter.KmtHandle = myMockWddm->getAdapter();
        expectedDeviceCb.hDevice.KmtHandle = myMockWddm->getDevice();
        expectedDeviceCb.hCsr = &mockWddmCsr;
        expectedDeviceCb.PagingQueue = myMockWddm->getPagingQueue();
        expectedDeviceCb.PagingFence = myMockWddm->getPagingQueueSyncObject();

        expectedDeviceCb.DevCbPtrs.KmtCbPtrs.pfnAllocate = myGdi->createAllocation;
        expectedDeviceCb.DevCbPtrs.KmtCbPtrs.pfnDeallocate = myGdi->destroyAllocation;
        expectedDeviceCb.DevCbPtrs.KmtCbPtrs.pfnMapGPUVA = myGdi->mapGpuVirtualAddress;
        expectedDeviceCb.DevCbPtrs.KmtCbPtrs.pfnMakeResident = myGdi->makeResident;
        expectedDeviceCb.DevCbPtrs.KmtCbPtrs.pfnEvict = myGdi->evict;
        expectedDeviceCb.DevCbPtrs.KmtCbPtrs.pfnReserveGPUVA = myGdi->reserveGpuVirtualAddress;
        expectedDeviceCb.DevCbPtrs.KmtCbPtrs.pfnUpdateGPUVA = myGdi->updateGpuVirtualAddress;
        expectedDeviceCb.DevCbPtrs.KmtCbPtrs.pfnWaitFromCpu = myGdi->waitForSynchronizationObjectFromCpu;
        expectedDeviceCb.DevCbPtrs.KmtCbPtrs.pfnLock = myGdi->lock2;
        expectedDeviceCb.DevCbPtrs.KmtCbPtrs.pfnUnLock = myGdi->unlock2;
        expectedDeviceCb.DevCbPtrs.KmtCbPtrs.pfnEscape = myGdi->escape;
        expectedDeviceCb.DevCbPtrs.KmtCbPtrs.pfnNotifyAubCapture = DeviceCallbacks<FamilyType>::notifyAubCapture;

        expectedTTCallbacks.pfWriteL3Adr = TTCallbacks<FamilyType>::writeL3Address;
        // clang-format on

        EXPECT_TRUE(memcmp(&expectedDeviceCb, &mockMngr->deviceCb, sizeof(GMM_DEVICE_CALLBACKS_INT)) == 0);
        EXPECT_TRUE(memcmp(&expectedDeviceCb.Adapter, &mockMngr->deviceCb.Adapter, sizeof(GMM_HANDLE_EXT)) == 0);
        EXPECT_TRUE(memcmp(&expectedDeviceCb.hDevice, &mockMngr->deviceCb.hDevice, sizeof(GMM_HANDLE_EXT)) == 0);
        EXPECT_TRUE(memcmp(&expectedDeviceCb.DevCbPtrs.KmtCbPtrs, &mockMngr->deviceCb.DevCbPtrs.KmtCbPtrs, sizeof(GMM_DEVICE_CB_PTRS::KmtCbPtrs)) == 0);
        EXPECT_TRUE(memcmp(&expectedTTCallbacks, &mockMngr->translationTableCb, sizeof(GMM_TRANSLATIONTABLE_CALLBACKS)) == 0);
        EXPECT_TRUE(memcmp(&expectedFlags, &mockMngr->translationTableFlags, sizeof(unsigned int)) == 0);
    }
}

HWTEST_F(WddmCsrCompressionTests, givenDisabledCompressionWhenInitializedThenDontCreatePagetableMngr) {
    ExecutionEnvironment *executionEnvironment = getExecutionEnvironmentImpl(hwInfo);
    std::unique_ptr<MockDevice> device(Device::create<MockDevice>(hwInfo, executionEnvironment, 0u));
    setCompressionEnabled(false, false);
    myMockWddm = static_cast<WddmMock *>(executionEnvironment->osInterface->get()->getWddm());
    MockWddmCsr<FamilyType> mockWddmCsr(hwInfo[0], *executionEnvironment);
    EXPECT_EQ(nullptr, myMockWddm->getPageTableManager());
}

HWTEST_F(WddmCsrCompressionTests, givenEnabledCompressionWhenFlushingThenInitTranslationTableOnce) {
    bool compressionEnabled[2][2] = {{true, false}, {false, true}};
    for (size_t i = 0; i < 2; i++) {
        ExecutionEnvironment *executionEnvironment = getExecutionEnvironmentImpl(hwInfo);
        setCompressionEnabled(compressionEnabled[i][0], compressionEnabled[i][1]);
        myMockWddm = static_cast<WddmMock *>(executionEnvironment->osInterface->get()->getWddm());
        auto mockWddmCsr = new MockWddmCsr<FamilyType>(hwInfo[0], *executionEnvironment);
        mockWddmCsr->createPageTableManager();
        mockWddmCsr->overrideDispatchPolicy(DispatchMode::BatchedDispatch);
        executionEnvironment->memoryManager.reset(mockWddmCsr->createMemoryManager(false, false));

        auto mockMngr = reinterpret_cast<MockGmmPageTableMngr *>(myMockWddm->getPageTableManager());
        std::unique_ptr<MockDevice> device(Device::create<MockDevice>(hwInfo, executionEnvironment, 0u));
        device->resetCommandStreamReceiver(mockWddmCsr);

        auto memoryManager = executionEnvironment->memoryManager.get();

        auto &csrCS = mockWddmCsr->getCS();

        auto graphicsAllocation = memoryManager->allocateGraphicsMemoryWithProperties(MockAllocationProperties{MemoryConstants::pageSize});
        IndirectHeap cs(graphicsAllocation);

        EXPECT_FALSE(mockWddmCsr->pageTableManagerInitialized);

        EXPECT_CALL(*mockMngr, initContextAuxTableRegister(mockWddmCsr, GMM_ENGINE_TYPE::ENGINE_TYPE_RCS))
            .Times(1)
            .WillOnce(Return(GMM_SUCCESS));
        EXPECT_CALL(*mockMngr, initContextTRTableRegister(mockWddmCsr, GMM_ENGINE_TYPE::ENGINE_TYPE_RCS))
            .Times(1)
            .WillOnce(Return(GMM_SUCCESS));

        DispatchFlags dispatchFlags;
        mockWddmCsr->flushTask(cs, 0u, cs, cs, cs, 0u, dispatchFlags, *device);

        EXPECT_TRUE(mockWddmCsr->pageTableManagerInitialized);

        // flush again to check if PT manager was initialized once
        mockWddmCsr->flushTask(cs, 0u, cs, cs, cs, 0u, dispatchFlags, *device);

        mockWddmCsr->flushBatchedSubmissions();
        memoryManager->freeGraphicsMemory(graphicsAllocation);
    }
}

HWTEST_F(WddmCsrCompressionTests, givenDisabledCompressionWhenFlushingThenDontInitTranslationTable) {
    ExecutionEnvironment *executionEnvironment = getExecutionEnvironmentImpl(hwInfo);
    setCompressionEnabled(false, false);
    myMockWddm = static_cast<WddmMock *>(executionEnvironment->osInterface->get()->getWddm());

    auto mockWddmCsr = new MockWddmCsr<FamilyType>(hwInfo[0], *executionEnvironment);
    mockWddmCsr->overrideDispatchPolicy(DispatchMode::BatchedDispatch);
    executionEnvironment->memoryManager.reset(mockWddmCsr->createMemoryManager(false, false));

    std::unique_ptr<MockDevice> device(Device::create<MockDevice>(hwInfo, executionEnvironment, 0u));
    device->resetCommandStreamReceiver(mockWddmCsr);

    auto memoryManager = executionEnvironment->memoryManager.get();

    EXPECT_EQ(nullptr, myMockWddm->getPageTableManager());

    auto graphicsAllocation = memoryManager->allocateGraphicsMemoryWithProperties(MockAllocationProperties{MemoryConstants::pageSize});
    IndirectHeap cs(graphicsAllocation);

    EXPECT_FALSE(mockWddmCsr->pageTableManagerInitialized);

    DispatchFlags dispatchFlags;
    mockWddmCsr->flushTask(cs, 0u, cs, cs, cs, 0u, dispatchFlags, *device);

    EXPECT_FALSE(mockWddmCsr->pageTableManagerInitialized);

    mockWddmCsr->flushBatchedSubmissions();
    memoryManager->freeGraphicsMemory(graphicsAllocation);
}
