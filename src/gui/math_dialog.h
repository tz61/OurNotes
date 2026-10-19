#pragma once

#include <QDialog>
#include <QTimer>

class QCheckBox;
class QLabel;
class QPlainTextEdit;

namespace on::gui {

// Edits a LaTeX formula with a live preview.
class MathDialog : public QDialog {
    Q_OBJECT
public:
    MathDialog(const QString& latex, bool display, QWidget* parent = nullptr);
    QString latex() const;
    bool isDisplay() const;

protected:
    bool eventFilter(QObject* watched, QEvent* e) override;

private:
    void updatePreview();

    QPlainTextEdit* source_;
    QCheckBox* display_;
    QLabel* preview_;
    QLabel* status_;
    QTimer timer_;
};

} // namespace on::gui
