#include "gui/NotificationWidget.h"
#include "gui/Theme.h"

#include <spdlog/spdlog.h>

#include <QSizePolicy>
#include <algorithm>

namespace vtol {

// ─── helpers ─────────────────────────────────────────────────

static const char* levelColor(NotificationLevel lvl)
{
    switch (lvl) {
    case NotificationLevel::Critical: return theme::ERROR_CLR;
    case NotificationLevel::Warning:  return theme::WARNING;
    case NotificationLevel::Info:     return theme::SUCCESS;
    }
    return theme::SUCCESS;
}

static const char* levelBg(NotificationLevel lvl)
{
    switch (lvl) {
    case NotificationLevel::Critical: return theme::ERROR_BG;
    case NotificationLevel::Warning:  return theme::WARNING_BG;
    case NotificationLevel::Info:     return theme::SUCCESS_BG;
    }
    return theme::SUCCESS_BG;
}

static int levelPriority(NotificationLevel lvl)
{
    switch (lvl) {
    case NotificationLevel::Critical: return 0;
    case NotificationLevel::Warning:  return 1;
    case NotificationLevel::Info:     return 2;
    }
    return 2;
}

static const char* levelName(NotificationLevel lvl)
{
    switch (lvl) {
    case NotificationLevel::Critical: return "critical";
    case NotificationLevel::Warning:  return "warning";
    case NotificationLevel::Info:     return "info";
    }
    return "info";
}

// ═════════════════════════════════════════════════════════════
//  NotificationWidget
// ═════════════════════════════════════════════════════════════

NotificationWidget::NotificationWidget(QWidget* parent)
    : QFrame(parent)
{
    m_timer.setInterval(100);
    connect(&m_timer, &QTimer::timeout, this, &NotificationWidget::tick);
    setupUi();
    hide();
}

void NotificationWidget::setupUi()
{
    setObjectName(QStringLiteral("notif_card"));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(10, 6, 10, 6);
    root->setSpacing(0);

    // Inner frame with left-border accent
    m_inner = new QFrame;
    m_inner->setObjectName(QStringLiteral("notif_inner"));
    auto* innerLay = new QVBoxLayout(m_inner);
    innerLay->setContentsMargins(12, 10, 12, 6);
    innerLay->setSpacing(4);

    // Title
    m_lblTitle = new QLabel;
    m_lblTitle->setWordWrap(true);
    m_lblTitle->setStyleSheet(QStringLiteral(
        "font-family: \"%1\"; font-size: 13px; font-weight: 700; "
        "border: none; background: transparent;")
        .arg(theme::FONT_FAMILY));
    innerLay->addWidget(m_lblTitle);

    // Message
    m_lblMessage = new QLabel;
    m_lblMessage->setWordWrap(true);
    m_lblMessage->setStyleSheet(QStringLiteral(
        "color: %1; font-size: 12px; border: none; background: transparent;")
        .arg(theme::TEXT_SECONDARY));
    innerLay->addWidget(m_lblMessage);

    // Action buttons row
    m_btnContainer = new QWidget;
    m_btnContainer->setStyleSheet(QStringLiteral("background: transparent;"));
    m_btnLay = new QHBoxLayout(m_btnContainer);
    m_btnLay->setContentsMargins(0, 4, 0, 2);
    m_btnLay->setSpacing(6);
    m_btnLay->addStretch();
    innerLay->addWidget(m_btnContainer);
    m_btnContainer->hide();

    root->addWidget(m_inner);

    // Progress bar (bottom edge)
    m_progress = new QProgressBar;
    m_progress->setTextVisible(false);
    m_progress->setFixedHeight(3);
    m_progress->setRange(0, 1000);
    m_progress->setValue(1000);
    m_progress->setStyleSheet(QStringLiteral(
        "QProgressBar { background-color: %1; border: none; border-radius: 0px; }"
        "QProgressBar::chunk { background-color: %2; border-radius: 0px; }")
        .arg(theme::BG_INPUT, theme::PRIMARY));
    root->addWidget(m_progress);
}

void NotificationWidget::showNotification(const Notification& n)
{
    stopTimer();
    m_current = n;
    m_elapsedMs = 0;
    m_durationMs = n.durationSec * 1000;

    auto color = levelColor(n.level);
    auto bg    = levelBg(n.level);

    // Card style
    m_inner->setStyleSheet(QStringLiteral(
        "#notif_inner { background-color: %1; border: 1px solid %2; "
        "border-left: 3px solid %3; border-radius: 8px; }")
        .arg(bg, theme::BORDER, color));

    // Title
    m_lblTitle->setText(n.title);
    m_lblTitle->setStyleSheet(QStringLiteral(
        "color: %1; font-family: \"%2\"; font-size: 13px; font-weight: 700; "
        "border: none; background: transparent;")
        .arg(color, theme::FONT_FAMILY));

    // Message
    m_lblMessage->setText(n.message);

    // Progress bar color
    m_progress->setStyleSheet(QStringLiteral(
        "QProgressBar { background-color: %1; border: none; border-radius: 0px; }"
        "QProgressBar::chunk { background-color: %2; border-radius: 0px; }")
        .arg(theme::BG_INPUT, color));
    m_progress->setValue(1000);

    // Action buttons
    clearButtons();
    if (!n.actions.empty()) {
        for (const auto& [label, callback] : n.actions) {
            auto* btn = new QPushButton(label);
            btn->setCursor(Qt::PointingHandCursor);
            btn->setFixedHeight(28);
            btn->setStyleSheet(QStringLiteral(
                "QPushButton { background-color: %1; color: %2; border: 1px solid %3; "
                "border-radius: 6px; padding: 0 12px; font-size: 11px; font-weight: 600; }"
                "QPushButton:hover { background-color: %4; color: %5; border-color: %6; }")
                .arg(theme::BG_INPUT, theme::TEXT_SECONDARY, theme::BORDER,
                     theme::BG_HOVER, theme::TEXT_PRIMARY, theme::BORDER_LIGHT));

            // capture by value for the closure
            auto cb = callback;
            auto lbl = label;
            connect(btn, &QPushButton::clicked, this, [this, cb, lbl] {
                auto title = m_current ? m_current->title : QStringLiteral("?");
                SPDLOG_INFO("[Notification] action '{}' on '{}'",
                            lbl.toStdString(), title.toStdString());
                cb();
                dismiss();
            });

            m_btnLay->insertWidget(m_btnLay->count() - 1, btn); // before stretch
            m_actionButtons.push_back(btn);
        }
        m_btnContainer->show();
    } else {
        m_btnContainer->hide();
    }

    show();
    m_timer.start();

    SPDLOG_INFO("[Notification] [{}] {} — {}",
                levelName(n.level), n.title.toStdString(), n.message.toStdString());
}

void NotificationWidget::dismiss()
{
    if (!m_current) return;
    auto title = m_current->title;
    stopTimer();
    m_current.reset();
    hide();
    SPDLOG_DEBUG("[Notification] dismissed '{}'", title.toStdString());
    emit notificationClosed();
}

void NotificationWidget::tick()
{
    m_elapsedMs += 100;
    int remaining = (std::max)(0, m_durationMs - m_elapsedMs);
    m_progress->setValue(m_durationMs > 0 ? (remaining * 1000 / m_durationMs) : 0);
    if (remaining <= 0) {
        auto title = m_current ? m_current->title : QStringLiteral("?");
        stopTimer();
        m_current.reset();
        hide();
        SPDLOG_DEBUG("[Notification] auto-closed '{}'", title.toStdString());
        emit notificationClosed();
    }
}

void NotificationWidget::stopTimer()
{
    m_timer.stop();
    clearButtons();
}

void NotificationWidget::clearButtons()
{
    for (auto* btn : m_actionButtons) {
        m_btnLay->removeWidget(btn);
        btn->deleteLater();
    }
    m_actionButtons.clear();
}

// ═════════════════════════════════════════════════════════════
//  NotificationManager
// ═════════════════════════════════════════════════════════════

NotificationManager::NotificationManager(NotificationWidget* widget, QObject* parent)
    : QObject(parent), m_widget(widget)
{
    connect(widget, &NotificationWidget::notificationClosed,
            this, &NotificationManager::showNext);
}

void NotificationManager::push(const Notification& n)
{
    insertByPriority(n);
    if (!n.tag.isEmpty())
        m_tags[n.tag.toStdString()] = 1;

    SPDLOG_DEBUG("[NotificationQueue] push '{}' [{}], queue_size={}",
                 n.title.toStdString(), levelName(n.level),
                 m_queue.size());

    if (!m_widget->isShowing())
        showNext();
}

void NotificationManager::pushOrReplace(Notification n, const QString& tag)
{
    n.tag = tag;
    auto key = tag.toStdString();
    if (m_tags.count(key)) {
        // Remove old from queue
        auto it = std::find_if(m_queue.begin(), m_queue.end(),
            [&tag](const Notification& x) { return x.tag == tag; });
        if (it != m_queue.end())
            m_queue.erase(it);

        // If currently showing the old one — dismiss it
        if (m_widget->isShowing()) {
            // We can't directly check tag of current, so just proceed —
            // the old one will be replaced in queue, new one pushed
        }
    }
    push(n);
}

void NotificationManager::clear()
{
    m_queue.clear();
    m_tags.clear();
    m_widget->dismiss();
    SPDLOG_INFO("[NotificationQueue] cleared");
}

void NotificationManager::insertByPriority(const Notification& n)
{
    int prio = levelPriority(n.level);
    for (auto it = m_queue.begin(); it != m_queue.end(); ++it) {
        if (levelPriority(it->level) > prio) {
            m_queue.insert(it, n);
            return;
        }
    }
    m_queue.push_back(n);
}

void NotificationManager::showNext()
{
    if (!m_queue.empty()) {
        auto notif = std::move(m_queue.front());
        m_queue.pop_front();
        if (!notif.tag.isEmpty())
            m_tags.erase(notif.tag.toStdString());
        SPDLOG_DEBUG("[NotificationQueue] showing next, remaining={}",
                     m_queue.size());
        m_widget->showNotification(notif);
    } else {
        m_widget->hide();
    }
}

} // namespace vtol
