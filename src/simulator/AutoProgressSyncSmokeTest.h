#pragma once

// Exercises AutoProgressSync::pushPending() against a loopback sync server,
// using the reading-upload fixtures. Runs only when the harness sets
// CROSSINK_AUTOSYNC_TEST_SERVER; the simulator cannot enter deep sleep, so this
// drives the library entry point the sleep hook calls rather than the hook.
void verifyAutoProgressSyncContract();
