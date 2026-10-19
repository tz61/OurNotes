#pragma once

#include <QFrame>

namespace on::gui {

class NoteTextEdit;

// An annotation's comment over the PDF: shown, as small as its text, while the
// mouse is on the annotation, and edited in place after a click. Formulas are
// typed and shown as in notes ($…$, $$…$$); the comment stays plain text.
class CommentBubble : public QFrame {
    Q_OBJECT
public:
    explicit CommentBubble(QWidget* viewport);

    QString key() const { return key_; }
    bool isEditing() const { return editing_; }
    bool isChanged() const;
    // `anchor`: the annotation's box in the viewport; the bubble goes below it, or above.
    void preview(const QString& key, const QString& comment, const QRectF& anchor);
    void edit(const QString& key, const QString& comment, const QRectF& anchor);
    void place(const QRectF& anchor);
    // Hides the bubble; with commit, an edited comment is reported first.
    void finish(bool commit);
    NoteTextEdit* editor() const { return text_; }

signals:
    void edited(const QString& key, const QString& comment);
    void editRequested(const QString& key);  // a click into a shown comment
    void left();                             // the mouse left a shown comment

protected:
    bool eventFilter(QObject* watched, QEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void leaveEvent(QEvent* e) override;

private:
    void load(const QString& key, const QString& comment);
    void fit();

    NoteTextEdit* text_;
    QString key_;
    QString original_;
    QRectF anchor_;
    bool editing_ = false;
};

} // namespace on::gui
