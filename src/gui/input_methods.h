#pragma once

namespace on::gui {

// Input methods on Linux and the BSDs, before the QApplication is created. The Qt that comes
// with OurNotes has only Qt's own input-method plugins, "compose" and "ibus": the usual
// QT_IM_MODULE=fcitx found no plugin, Qt fell back to compose, and no input method worked at
// all (Ctrl+Space switched nothing). fcitx5 answers Qt's ibus plugin as IBus does, so with
// QT_IM_MODULE=fcitx (or fcitx5) and no QT_IM_MODULES of the user's, Qt 6.8+ is asked for
// "fcitx;ibus": a fcitx plugin if there is one, else fcitx5 through IBus (which needs the ibus
// program installed). Any other choice is left as it is; nothing on macOS or Windows, where
// any value would replace the system's input method.
void chooseInputMethods();

} // namespace on::gui
