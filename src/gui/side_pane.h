#pragma once

class QSplitter;
class QTabWidget;

namespace on::gui {

// The pane on the right of the library (the item pane) and of every PDF (Info and
// Notes): one width for all of them, kept in the settings. Dragging the edge of one
// moves the others; a tab shown later takes it too. `split`'s last widget is the pane.
void shareSidePaneWidth(QSplitter* split);
// Gives `split`'s pane the shared width now (e.g. after restoring a saved layout).
void applySidePaneWidth(QSplitter* split);

// Info or Notes: one choice for the item pane and every PDF's pane, whatever the item
// (also one without notes), kept in the settings. Clicking a tab in one shows it in all.
// `tabs` has Info first and Notes second; a disabled tab (Info of a PDF filed under no
// item) gives way to the other.
void shareInfoNotesTab(QTabWidget* tabs);
// Shows Info (0) or Notes (1) in `tabs` and everywhere else, as a click on it does.
void chooseInfoNotesTab(QTabWidget* tabs, int index);

} // namespace on::gui
