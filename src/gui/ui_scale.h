#pragma once

#include "core/config.h"

namespace on::gui {

constexpr int kDefaultInterfaceScale = 150;  // percent

// Interface scale in percent from the configuration ("/ui/scale"), clamped to 50-300.
int interfaceScale(const Config& config);

// Applies the configured scale through QT_SCALE_FACTOR. Must run before the
// QApplication is created; a QT_SCALE_FACTOR already set in the environment wins.
void applyInterfaceScale(const Config& config);

// True when QT_SCALE_FACTOR came from the environment rather than the configuration.
bool interfaceScaleFromEnvironment();

// Once the QApplication exists (Qt has read it): QT_SCALE_FACTOR set here is taken out of the
// environment again, so programs started from here don't take it for theirs - OurNotes
// restarted for a new scale kept the old one (it seemed set by the user), and a file opened
// in another Qt program made that program larger too.
void forgetInterfaceScale();

} // namespace on::gui
