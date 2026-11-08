#pragma once

#include <QFrame>

#include <functional>
#include <string>

#include "formats/translate.h"

class QComboBox;
class QHBoxLayout;
class QLabel;
class QTextBrowser;
class QToolButton;

namespace on::gui {

// Text chosen in a PDF and its translation by Google Translate, small beside it (see
// PdfView::translateSelection). Asked for on a thread of its own; until the answer comes,
// "Translating…". The language it is translated into is chosen in its corner, and kept.
class TranslationPopup : public QFrame {
    Q_OBJECT
public:
    explicit TranslationPopup(QWidget* parent);

    using Translator = std::function<formats::Translation(const std::string& text, const std::string& target)>;
    static Translator& translator();  // formats::translate (tests put one of their own)

    void translate(const QString& text);
    void cancel();  // hidden, and an answer still to come is not shown
    QString text() const { return text_; }  // as sent
    QString translation() const { return translation_; }  // as shown, or empty
    QString error() const { return error_; }
    bool busy() const { return busy_; }

    static QString targetLanguage();  // as chosen, or the system's language
    static void setTargetLanguage(const QString& code);
    static bool translateAtOnce();    // the text translated as soon as it is selected
    static void fillLanguages(QComboBox* box, const QString& current);
    // Lines joined (a word broken at a line's end, mended), spaces made single, and at
    // most kMaxCharacters (cut is set when there was more).
    static QString cleanText(const QString& text, bool* cut = nullptr);
    static constexpr int kMaxCharacters = 5000;

signals:
    void sizeChanged();  // (to be placed again)

protected:
    void keyPressEvent(QKeyEvent* e) override;  // Esc: closed (from its text too)
    // a click in it is its own, not the page's under it
    void mousePressEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void contextMenuEvent(QContextMenuEvent* e) override;

private:
    void request();
    void showResult(const formats::Translation& t, bool intoEnglish);
    void showError(const QString& message);
    void showBody(const QString& html);

    QString text_;
    bool cut_ = false;
    quint64 generation_ = 0;
    bool busy_ = false;
    QString translation_;
    QString error_;
    QHBoxLayout* head_ = nullptr;
    QLabel* from_ = nullptr;
    QComboBox* to_ = nullptr;
    QToolButton* copy_ = nullptr;
    QToolButton* open_ = nullptr;
    QTextBrowser* body_ = nullptr;
};

} // namespace on::gui
