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

} // namespace on::gui
