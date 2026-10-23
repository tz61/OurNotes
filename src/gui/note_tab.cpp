#include "gui/note_tab.h"

#include <QVBoxLayout>

#include "gui/app_context.h"
#include "gui/common.h"
#include "gui/note_editor.h"

namespace on::gui {

NoteTab::NoteTab(int64_t libraryId, const QString& noteKey, QWidget* parent)
    : QWidget(parent), libraryId_(libraryId), key_(noteKey)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    editor_ = new NoteEditor(this);
    layout->addWidget(editor_);
    editor_->setNote(libraryId_, key_);
}

QString NoteTab::titleFor(int64_t libraryId, const QString& noteKey)
{
    auto note = AppContext::get().library().item(libraryId, ss(noteKey));
    if (!note)
        return {};
    auto title = qs(noteTitle(jstr(*note, "note")));
    return title.isEmpty() ? tr("(empty note)") : title;
}

void NoteTab::focusEditor() { editor_->focusEditor(); }

void NoteTab::focusFind() { editor_->openFind(); }

} // namespace on::gui
