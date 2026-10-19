// Temporary placeholder until the formats module is integrated.
#include <QMessageBox>

#include "gui/formats_actions.h"
#include "gui/items_model.h"

namespace fp::gui {

static void notYet(QWidget* parent)
{
    QMessageBox::information(parent, "freePen", "This feature is not available in this build.");
}

void exportItemsDialog(QWidget* p, const Scope&, const QStringList&) { notYet(p); }
void importFileDialog(QWidget* p, int64_t, const std::string&) { notYet(p); }
void bibliographyDialog(QWidget* p, const Scope&, const QStringList&) { notYet(p); }
void copyCitationToClipboard(QWidget* p, int64_t, const QStringList&, bool) { notYet(p); }
void addByIdentifierDialog(QWidget* p, int64_t, const std::string&) { notYet(p); }
void retrieveMetadata(QWidget*, int64_t, const QStringList&) {}

} // namespace fp::gui
