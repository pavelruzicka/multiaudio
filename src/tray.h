// The notification-area (system tray) front end.
#pragma once

namespace ma {

// Runs the tray application until the user exits. Returns the process exit
// code. If another copy is already running, it brings that one to the user's
// attention and returns straight away.
int RunTrayApp();

}  // namespace ma
