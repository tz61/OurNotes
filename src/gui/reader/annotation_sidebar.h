#pragma once

#include <QFrame>
#include <QMap>
#include <QWidget>

#include <functional>
#include <vector>

#include "core/json.h"

class QLabel;
class QLineEdit;
class QScrollArea;
class QToolButton;
class QVBoxLayout;

namespace fp::gui {

class AutoTextEdit;

class AnnotationCard : public QFrame {
    Q_OBJECT
public:
    AnnotationCard(const Json& data, bool editable, QWidget* parent = nullptr);
    void update(const Json& data, bool editable);
    void setSelected(bool selected);
    const Json& data() const { return data_; }
    bool isEditing() const;
    void focusComment();
    std::function<QImage(const Json&)> imageProvider;

signals:
    void activated(const QString& key);
    void changed(const fp::Json& data);
    void deleted(const QString& key);
    void addToNote(const fp::Json& data);

protected:
    void mousePressEvent(QMouseEvent* e) override;

private:
    void refresh();
    Json data_;
    bool editable_ = true;
    bool selected_ = false;
    QLabel* header_;
    QToolButton* menu_;
    QLabel* text_;
    QLabel* image_;
    AutoTextEdit* comment_;
    QLabel* tags_;
};

class AnnotationSidebar : public QWidget {
    Q_OBJECT
public:
    explicit AnnotationSidebar(QWidget* parent = nullptr);

    void setAnnotations(const std::vector<Json>& annotations, bool editable);
    void setSelected(const QString& key, bool scroll);
    void focusComment(const QString& key);
    std::function<QImage(const Json&)> imageProvider;

signals:
    void activated(const QString& key);
    void changed(const fp::Json& data);
    void deleted(const QString& key);
    void addToNote(const fp::Json& data);

private:
    void applyFilter();

    QLineEdit* search_;
    QWidget* colorBar_;
    QString colorFilter_;
    QScrollArea* scroll_;
    QWidget* container_;
    QVBoxLayout* list_;
    QLabel* empty_;
    QMap<QString, AnnotationCard*> cards_;
    QString selected_;
};

} // namespace fp::gui
