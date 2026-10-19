#pragma once

class QSplitter;

namespace on::gui {

// The pane on the right of the library (the item pane) and of every PDF (Info and
// Notes): one width for all of them, kept in the settings. Dragging the edge of one
// moves the others; a tab shown later takes it too. `split`'s last widget is the pane.
void shareSidePaneWidth(QSplitter* split);
// Gives `split`'s pane the shared width now (e.g. after restoring a saved layout).
void applySidePaneWidth(QSplitter* split);

} // namespace on::gui
