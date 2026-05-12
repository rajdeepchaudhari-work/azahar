// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <memory>
#include "common/common_types.h"

namespace Frontend {
class EmuWindow;
} // namespace Frontend

namespace InputCommon {
class Keyboard;
} // namespace InputCommon

namespace InputCommon::WebInput {

// Runs an HTTP server (default port 9753) that serves a 3DS virtual controller page.
// Open the URL shown in the status bar on your phone's browser over local Wi-Fi.
// Button presses map through the current input profile's keyboard bindings.
// Touch input calls EmuWindow::TouchPressed/Moved/Released.
class Server {
public:
    Server();
    ~Server();

    // Start listening on 0.0.0.0. Returns false if bind fails.
    bool Start(Keyboard* keyboard, Frontend::EmuWindow* window, u16 port = 9753);
    void Stop();
    bool IsRunning() const;

    // Returns the bound port (useful if port 0 was requested).
    u16 GetPort() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace InputCommon::WebInput
