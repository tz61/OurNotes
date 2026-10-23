#pragma once

namespace on::gui {

// Every window but the main one (where Ctrl+W closes a tab) closes with Ctrl+W, ⌘W on
// macOS, and with the platform's Close (Ctrl+F4 on Windows): the windows of the Tools
// menu, Preferences, dialogs. Each gets the shortcut when first shown. Call once, after
// the QApplication exists.
void installCloseShortcuts();

} // namespace on::gui
