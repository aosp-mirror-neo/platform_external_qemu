// Copyright (C) 2026 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "android/emulation/control/notifications/NotificationStream.h"

#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>
#include <chrono>
#include <cstdint>
#include <memory>

#include "android/cmdline-definitions.h"
#include "android/console.h"
#include "android/emulation/control/AndroidAgentFactory.h"
#include "android/emulation/control/camera/VirtualSceneCamera.h"
#include "android/emulation/control/globals_agent.h"
#include "android/emulation/control/hw_control_agent.h"
#include "android/emulation/control/hw_xr_led_agent.h"
#include "emulator_controller.grpc.pb.h"
#include "emulator_controller.pb.h"
#include "google/protobuf/empty.pb.h"
#include "host-common/MultiDisplay.h"
#include "host-common/hw-config.h"
#include "host-common/multi_display_agent.h"
#include "host-common/vm_operations.h"
#include "host-common/window_agent.h"

extern "C" void skin_winsys_show_virtual_scene_controls(bool /*show*/) {}

namespace android {
namespace emulation {
namespace control {
namespace {

AndroidHwConfig sHwConfig = {};
AndroidOptions sCmdLineOptions = {};

const QAndroidGlobalVarsAgent kTestGlobalVarsAgent = {
        .avdInfo = []() -> AvdInfo* { return nullptr; },
        .hw = []() -> AndroidHwConfig* { return &sHwConfig; },
        .guest_boot_completed = []() -> bool { return false; },
        .android_cmdLineOptions =
                []() -> AndroidOptions* { return &sCmdLineOptions; },
        .has_cmdLineOptions = []() -> bool { return true; },
};

const QAndroidEmulatorWindowAgent kTestWindowAgent = {
        .isFolded = []() -> bool { return false; },
        .setUIDisplayRegion = [](int, int, int, int, bool) {},
        .setNoSkin = []() {},
        .restoreSkin = []() {},
        .updateUIMultiDisplayPage = [](uint32_t) {},
        .addMultiDisplayWindow = [](uint32_t, bool, uint32_t, uint32_t) {
            return true;
        },
        .getLayout = []() -> void* { return nullptr; },
};

const QAndroidHwControlAgent kTestHwControlAgent = {
        .setCallbacks = [](void*, const AndroidHwControlFuncs*) {},
};

const QAndroidHwXrLedAgent kTestHwXrLedAgent = {
        .setCallbacks = [](void*, const AndroidHwXrLedFuncs*) {},
};

const QAndroidVmOperations kTestVmOperations = {
        .getRealAudioEventListener = []() -> void* { return nullptr; },
        .isRealAudioAllowed = []() { return false; },
};

const QAndroidMultiDisplayAgent kForwardingMultiDisplayAgent = {
        .getMultiDisplay = [](uint32_t id,
                              int32_t* x,
                              int32_t* y,
                              uint32_t* w,
                              uint32_t* h,
                              uint32_t* dpi,
                              uint32_t* flag,
                              bool* enabled) -> bool {
            return MultiDisplay::getInstance()->getMultiDisplay(
                    id, x, y, w, h, dpi, flag, enabled);
        },
        .getDisplayColorBuffer = [](uint32_t displayId,
                                    uint32_t* colorBuffer) -> int {
            return MultiDisplay::getInstance()->getDisplayColorBuffer(
                    displayId, colorBuffer);
        },
};

class TestConsoleFactory : public AndroidConsoleFactory {
public:
    const QAndroidGlobalVarsAgent* android_get_QAndroidGlobalVarsAgent()
            const override {
        return &kTestGlobalVarsAgent;
    }
    const QAndroidEmulatorWindowAgent* android_get_QAndroidEmulatorWindowAgent()
            const override {
        return &kTestWindowAgent;
    }
    const QAndroidHwControlAgent* android_get_QAndroidHwControlAgent()
            const override {
        return &kTestHwControlAgent;
    }
    const QAndroidHwXrLedAgent* android_get_QAndroidHwXrLedAgent()
            const override {
        return &kTestHwXrLedAgent;
    }
    const QAndroidVmOperations* android_get_QAndroidVmOperations()
            const override {
        return &kTestVmOperations;
    }
    const QAndroidMultiDisplayAgent* android_get_QAndroidMultiDisplayAgent()
            const override {
        return &kForwardingMultiDisplayAgent;
    }
};

class TestNotificationService final
    : public EmulatorController::WithCallbackMethod_streamNotification<
              EmulatorController::Service> {
public:
    explicit TestNotificationService(NotificationStream* stream)
        : mStream(stream) {}

    ::grpc::ServerWriteReactor<Notification>* streamNotification(
            ::grpc::CallbackServerContext* /*context*/,
            const ::google::protobuf::Empty* /*request*/) override {
        return mStream->notificationStream();
    }

private:
    NotificationStream* mStream;
};

Notification readNextDisplayNotification(
        grpc::ClientReaderInterface<Notification>* reader) {
    Notification note;
    while (reader->Read(&note)) {
        if (note.has_displayconfigurationschangednotification()) {
            return note;
        }
    }
    ADD_FAILURE() << "Stream ended before receiving "
                     "DisplayConfigurationsChangedNotification";
    return note;
}

TEST(NotificationStreamTest,
     EmitsDisplayConfigurationsChangedWithoutColorBuffer) {
    injectConsoleAgents(TestConsoleFactory());
    android_init_multi_display(getConsoleAgents()->emu, nullptr,
                               getConsoleAgents()->vm, false);

    auto* multiDisplay = MultiDisplay::getInstance();
    ASSERT_NE(multiDisplay, nullptr);

    // Configure primary display 0.
    uint32_t primaryId = 0;
    ASSERT_EQ(multiDisplay->createDisplay(&primaryId), 0);
    ASSERT_EQ(multiDisplay->setDisplayPose(primaryId, 0, 0, 1080, 1920, 420, 0),
              0);

    VirtualSceneCamera camera(nullptr);
    NotificationStream notificationStream(&camera, getConsoleAgents());
    TestNotificationService service(&notificationStream);

    grpc::ServerBuilder builder;
    builder.RegisterService(&service);
    std::unique_ptr<grpc::Server> server = builder.BuildAndStart();
    ASSERT_NE(server, nullptr);

    auto channel = server->InProcessChannel(grpc::ChannelArguments());
    auto stub = EmulatorController::NewStub(channel);

    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() +
                     std::chrono::seconds(5));
    google::protobuf::Empty request;
    auto reader = stub->streamNotification(&ctx, request);

    // 1. Initial notification should include primary display 0.
    Notification initial = readNextDisplayNotification(reader.get());
    const auto& initialConfigs =
            initial.displayconfigurationschangednotification()
                    .displayconfigurations();
    ASSERT_EQ(initialConfigs.displays_size(), 1);
    EXPECT_EQ(initialConfigs.displays(0).display(), 0u);
    EXPECT_EQ(initialConfigs.displays(0).width(), 1080u);
    EXPECT_EQ(initialConfigs.displays(0).height(), 1920u);
    EXPECT_EQ(initialConfigs.displays(0).dpi(), 420u);

    // 2. Simulate setDisplayConfigurations adding display 1:
    // createDisplay creates an unconfigured display (0x0, 0 dpi, cb=0), which
    // must not emit a partially created display notification, followed by
    // setDisplayPose configuring dimensions while color buffer is still 0.
    uint32_t secondId = 1;
    ASSERT_EQ(multiDisplay->createDisplay(&secondId), 0);
    ASSERT_EQ(multiDisplay->setDisplayPose(secondId, -1, -1, 1920, 1080, 320, 0),
              0);
    multiDisplay->notifyDisplayChanges();

    Notification added = readNextDisplayNotification(reader.get());
    const auto& addedConfigs =
            added.displayconfigurationschangednotification()
                    .displayconfigurations();
    ASSERT_EQ(addedConfigs.displays_size(), 2);
    EXPECT_EQ(addedConfigs.displays(0).display(), 0u);
    EXPECT_EQ(addedConfigs.displays(1).display(), 1u);
    EXPECT_EQ(addedConfigs.displays(1).width(), 1920u);
    EXPECT_EQ(addedConfigs.displays(1).height(), 1080u);
    EXPECT_EQ(addedConfigs.displays(1).dpi(), 320u);

    // 3. Simulate modifying display 1 while color buffer is still 0.
    ASSERT_EQ(multiDisplay->setDisplayPose(secondId, -1, -1, 1280, 720, 240, 0),
              0);
    multiDisplay->notifyDisplayChanges();

    Notification updated = readNextDisplayNotification(reader.get());
    const auto& updatedConfigs =
            updated.displayconfigurationschangednotification()
                    .displayconfigurations();
    ASSERT_EQ(updatedConfigs.displays_size(), 2);
    EXPECT_EQ(updatedConfigs.displays(1).display(), 1u);
    EXPECT_EQ(updatedConfigs.displays(1).width(), 1280u);
    EXPECT_EQ(updatedConfigs.displays(1).height(), 720u);
    EXPECT_EQ(updatedConfigs.displays(1).dpi(), 240u);

    // 4. Simulate removing display 1 while color buffer is 0.
    ASSERT_EQ(multiDisplay->destroyDisplay(secondId), 0);
    multiDisplay->notifyDisplayChanges();

    Notification removed = readNextDisplayNotification(reader.get());
    const auto& removedConfigs =
            removed.displayconfigurationschangednotification()
                    .displayconfigurations();
    ASSERT_EQ(removedConfigs.displays_size(), 1);
    EXPECT_EQ(removedConfigs.displays(0).display(), 0u);

    ctx.TryCancel();
    server->Shutdown();
}

}  // namespace
}  // namespace control
}  // namespace emulation
}  // namespace android
