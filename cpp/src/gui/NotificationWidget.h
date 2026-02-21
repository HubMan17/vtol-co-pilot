#pragma once

#include <QFrame>
#include <QLabel>
#include <QPushButton>
#include <QProgressBar>
#include <QTimer>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QObject>

#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace vtol {

// ─── Data types ──────────────────────────────────────────────

enum class NotificationLevel { Info, Warning, Critical };

struct Notification {
    NotificationLevel level = NotificationLevel::Info;
    QString           title;
    QString           message;
    int               durationSec = 60;
    // (label, callback) pairs
    std::vector<std::pair<QString, std::function<void()>>> actions;
    QString           tag;       // for deduplication
};

// ─── Widget ──────────────────────────────────────────────────

class NotificationWidget : public QFrame {
    Q_OBJECT
public:
    explicit NotificationWidget(QWidget* parent = nullptr);

    void showNotification(const Notification& n);
    void dismiss();
    bool isShowing() const { return m_current.has_value(); }

signals:
    void notificationClosed();

private:
    void setupUi();
    void tick();
    void clearButtons();
    void stopTimer();

    QFrame*       m_inner       = nullptr;
    QLabel*       m_lblTitle    = nullptr;
    QLabel*       m_lblMessage  = nullptr;
    QWidget*      m_btnContainer= nullptr;
    QHBoxLayout*  m_btnLay      = nullptr;
    QProgressBar* m_progress    = nullptr;
    QTimer        m_timer;

    std::optional<Notification> m_current;
    int m_elapsedMs  = 0;
    int m_durationMs = 0;

    std::vector<QPushButton*> m_actionButtons;
};

// ─── Manager (queue) ─────────────────────────────────────────

class NotificationManager : public QObject {
    Q_OBJECT
public:
    explicit NotificationManager(NotificationWidget* widget, QObject* parent = nullptr);

    void push(const Notification& n);
    void pushOrReplace(Notification n, const QString& tag);
    void clear();

private:
    void insertByPriority(const Notification& n);
    void showNext();

    NotificationWidget*                 m_widget;
    std::deque<Notification>            m_queue;
    std::unordered_map<std::string, int> m_tags; // tag → index hint (approx)
};

} // namespace vtol
