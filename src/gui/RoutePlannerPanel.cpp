#include "RoutePlannerPanel.h"
#include "Theme.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFrame>
#include <QListWidgetItem>
#include <spdlog/spdlog.h>

namespace vtol {

// ─── Helpers ──────────────────────────────────────────────────────────────

QString RoutePlannerPanel::actionLabel(const QString& action, int orbitTurns)
{
    if (action == "FLYTHROUGH")     return QStringLiteral("Пролёт");
    if (action == "ORBIT_TURNS")    return QStringLiteral("Кружение ×%1").arg(orbitTurns);
    if (action == "ORBIT_INFINITE") return QStringLiteral("∞ Кружение");
    if (action == "ALTITUDE")       return QStringLiteral("Высота");
    return action;
}

// ─── Construction ─────────────────────────────────────────────────────────

RoutePlannerPanel::RoutePlannerPanel(QWidget* parent)
    : QWidget(parent)
{
    setStyleSheet(QStringLiteral("background-color: %1;").arg(theme::BG_SIDEBAR));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // ── Header row ──
    auto* header = new QWidget;
    header->setFixedHeight(34);
    header->setStyleSheet(QStringLiteral(
        "background-color: %1; border-bottom: 1px solid %2;")
        .arg(theme::BG_CARD, theme::BORDER));
    auto* hlay = new QHBoxLayout(header);
    hlay->setContentsMargins(10, 0, 6, 0);
    hlay->setSpacing(6);

    m_lblTitle = new QLabel(QStringLiteral("МАРШРУТ"));
    m_lblTitle->setStyleSheet(QStringLiteral(
        "color: %1; font-size: 10px; font-weight: 700; letter-spacing: 1px;")
        .arg(theme::TEXT_TERTIARY));
    hlay->addWidget(m_lblTitle);
    hlay->addStretch();

    // Load / Save buttons
    auto btnStyle = QStringLiteral(
        "QPushButton { background: transparent; color: %1; border: none; "
        "font-size: 11px; padding: 0 6px; }"
        "QPushButton:hover { color: %2; }").arg(theme::TEXT_TERTIARY, theme::TEXT_PRIMARY);

    auto* btnLoad = new QPushButton(QStringLiteral("📂"));
    btnLoad->setFixedSize(26, 26);
    btnLoad->setToolTip(QStringLiteral("Загрузить маршрут"));
    btnLoad->setStyleSheet(btnStyle);
    connect(btnLoad, &QPushButton::clicked, this, &RoutePlannerPanel::loadRouteRequested);
    hlay->addWidget(btnLoad);

    auto* btnSave = new QPushButton(QStringLiteral("💾"));
    btnSave->setFixedSize(26, 26);
    btnSave->setToolTip(QStringLiteral("Сохранить маршрут"));
    btnSave->setStyleSheet(btnStyle);
    connect(btnSave, &QPushButton::clicked, this, &RoutePlannerPanel::saveRouteRequested);
    hlay->addWidget(btnSave);

    m_btnToggle = new QPushButton(QStringLiteral("▲"));
    m_btnToggle->setFixedSize(26, 26);
    m_btnToggle->setStyleSheet(btnStyle);
    m_btnToggle->setToolTip(QStringLiteral("Свернуть / развернуть"));
    connect(m_btnToggle, &QPushButton::clicked, this, [this] {
        setExpanded(!m_expanded);
    });
    hlay->addWidget(m_btnToggle);

    root->addWidget(header);

    // ── Collapsible content ──
    m_content = new QWidget;
    auto* clay = new QVBoxLayout(m_content);
    clay->setContentsMargins(6, 4, 6, 6);
    clay->setSpacing(4);

    // Waypoints list
    m_list = new QListWidget;
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setDragDropMode(QAbstractItemView::InternalMove);
    m_list->setDefaultDropAction(Qt::MoveAction);
    m_list->setMinimumHeight(80);
    m_list->setMaximumHeight(260);
    m_list->setStyleSheet(QStringLiteral(
        "QListWidget { background: %1; border: 1px solid %2; border-radius: 4px; "
        "color: %3; font-size: 12px; outline: none; }"
        "QListWidget::item { padding: 5px 8px; border-bottom: 1px solid %2; }"
        "QListWidget::item:selected { background: %4; color: %3; }"
        "QListWidget::item:hover { background: %5; }")
        .arg(theme::BG_CARD, theme::BORDER_SUBTLE, theme::TEXT_SECONDARY,
             theme::BG_HOVER, theme::BG_INPUT));
    connect(m_list, &QListWidget::itemDoubleClicked, this, &RoutePlannerPanel::onItemDoubleClicked);
    connect(m_list, &QListWidget::itemSelectionChanged, this, &RoutePlannerPanel::onSelectionChanged);
    // Detect drag-drop reorder
    connect(m_list->model(), &QAbstractItemModel::rowsMoved,
            this, [this](const QModelIndex&, int srcRow, int /*srcLast*/,
                         const QModelIndex&, int dstRow) {
        // QListWidget rowsMoved: when moving DOWN, dstRow is one extra
        int to = (dstRow > srcRow) ? dstRow - 1 : dstRow;
        if (srcRow == to) return;
        spdlog::info("[RoutePlannerPanel] reorder drag: {} → {}", srcRow, to);
        emit reorderWaypointRequested(srcRow, to);
    });
    clay->addWidget(m_list);

    // Action buttons row
    auto* brow = new QHBoxLayout;
    brow->setSpacing(4);

    m_btnAdd = new QPushButton(QStringLiteral("+ Добавить"));
    m_btnAdd->setFixedHeight(26);
    m_btnAdd->setStyleSheet(QStringLiteral(
        "QPushButton { background: %1; color: #fff; border: none; border-radius: 5px; "
        "font-size: 11px; font-weight: 600; padding: 0 8px; }"
        "QPushButton:hover { background: %2; }"
        "QPushButton:checked { background: %3; }")
        .arg(theme::PRIMARY, theme::PRIMARY_HOVER, theme::SUCCESS));
    m_btnAdd->setCheckable(true);
    m_btnAdd->setToolTip(QStringLiteral("Активировать режим добавления точки на карте"));
    connect(m_btnAdd, &QPushButton::clicked, this, [this](bool checked) {
        spdlog::info("[RoutePlannerPanel] addWaypointRequested checked={}", checked);
        emit addWaypointRequested();
    });
    brow->addWidget(m_btnAdd);

    m_btnEdit = new QPushButton(QStringLiteral("Изменить"));
    m_btnEdit->setFixedHeight(26);
    m_btnEdit->setEnabled(false);
    m_btnEdit->setStyleSheet(QStringLiteral(
        "QPushButton { background: %1; color: %2; border: 1px solid %3; "
        "border-radius: 5px; font-size: 11px; padding: 0 8px; }"
        "QPushButton:hover { background: %4; }"
        "QPushButton:disabled { color: %5; border-color: %3; }")
        .arg(theme::BG_INPUT, theme::TEXT_SECONDARY, theme::BORDER,
             theme::BG_HOVER, theme::TEXT_DIM));
    connect(m_btnEdit, &QPushButton::clicked, this, &RoutePlannerPanel::onEditClicked);
    brow->addWidget(m_btnEdit);

    m_btnDelete = new QPushButton(QStringLiteral("Удалить"));
    m_btnDelete->setFixedHeight(26);
    m_btnDelete->setEnabled(false);
    m_btnDelete->setStyleSheet(QStringLiteral(
        "QPushButton { background: %1; color: %2; border: 1px solid %3; "
        "border-radius: 5px; font-size: 11px; padding: 0 8px; }"
        "QPushButton:hover { background: %4; color: #fff; }"
        "QPushButton:disabled { color: %5; border-color: %3; }")
        .arg(theme::BG_INPUT, theme::ERROR_CLR, theme::BORDER,
             theme::ERROR_BG, theme::TEXT_DIM));
    connect(m_btnDelete, &QPushButton::clicked, this, &RoutePlannerPanel::onDeleteClicked);
    brow->addWidget(m_btnDelete);

    auto* btnClear = new QPushButton(QStringLiteral("Очистить"));
    btnClear->setFixedHeight(26);
    btnClear->setStyleSheet(QStringLiteral(
        "QPushButton { background: %1; color: %2; border: 1px solid %3; "
        "border-radius: 5px; font-size: 11px; padding: 0 8px; }"
        "QPushButton:hover { background: %4; }")
        .arg(theme::BG_INPUT, theme::TEXT_TERTIARY, theme::BORDER, theme::BG_HOVER));
    connect(btnClear, &QPushButton::clicked, this, &RoutePlannerPanel::clearRouteRequested);
    brow->addWidget(btnClear);

    clay->addLayout(brow);

    root->addWidget(m_content);
}

// ─── Public methods ────────────────────────────────────────────────────────

void RoutePlannerPanel::refresh(const QVector<QVariantMap>& waypoints, int activeIdx)
{
    spdlog::info("[RoutePlannerPanel] refresh: count={} activeIdx={}", waypoints.size(), activeIdx);

    // Preserve selection
    int selIdx = -1;
    auto sel = m_list->selectedItems();
    if (!sel.isEmpty())
        selIdx = m_list->row(sel.first());

    m_list->blockSignals(true);
    m_list->clear();

    for (int i = 0; i < waypoints.size(); ++i) {
        const auto& wp = waypoints[i];
        QString action  = wp.value("action").toString();
        int orbitTurns  = wp.value("orbitTurns", 1).toInt();
        int alt         = static_cast<int>(wp.value("altitude").toDouble());
        QString typeStr = actionLabel(action, orbitTurns);

        // State indicator
        QString stateIcon;
        if (i < activeIdx)       stateIcon = "✓";
        else if (i == activeIdx) stateIcon = "►";
        else                     stateIcon = "○";

        QString text = QStringLiteral("%1 %2.  %3   %4 м")
            .arg(stateIcon).arg(i + 1).arg(typeStr).arg(alt);

        auto* item = new QListWidgetItem(text);
        item->setData(Qt::UserRole, i);  // store 0-based index
        item->setFlags(item->flags() | Qt::ItemIsDragEnabled);

        // Colour by state
        if (i < activeIdx)
            item->setForeground(QColor(theme::TEXT_DIM));
        else if (i == activeIdx)
            item->setForeground(QColor(theme::SUCCESS));

        m_list->addItem(item);
    }

    // Restore selection
    if (selIdx >= 0 && selIdx < m_list->count())
        m_list->setCurrentRow(selIdx);

    m_list->blockSignals(false);
    onSelectionChanged();

    m_lblTitle->setText(waypoints.isEmpty()
        ? QStringLiteral("МАРШРУТ — нет точек")
        : QStringLiteral("МАРШРУТ  (%1)").arg(waypoints.size()));
}

void RoutePlannerPanel::setExpanded(bool expanded)
{
    m_expanded = expanded;
    m_content->setVisible(expanded);
    m_btnToggle->setText(expanded ? QStringLiteral("▲") : QStringLiteral("▼"));
}

// ─── Private slots ─────────────────────────────────────────────────────────

void RoutePlannerPanel::onItemDoubleClicked(QListWidgetItem* item)
{
    int idx = item->data(Qt::UserRole).toInt();
    emit editWaypointRequested(idx);
}

void RoutePlannerPanel::onSelectionChanged()
{
    auto sel = m_list->selectedItems();
    bool hasSel = !sel.isEmpty();
    m_btnEdit->setEnabled(hasSel);
    m_btnDelete->setEnabled(hasSel);

    if (hasSel) {
        int idx = sel.first()->data(Qt::UserRole).toInt();
        emit centerOnWaypointRequested(idx);
    }
}

void RoutePlannerPanel::onEditClicked()
{
    auto sel = m_list->selectedItems();
    if (sel.isEmpty()) return;
    emit editWaypointRequested(sel.first()->data(Qt::UserRole).toInt());
}

void RoutePlannerPanel::onDeleteClicked()
{
    auto sel = m_list->selectedItems();
    if (sel.isEmpty()) return;
    emit deleteWaypointRequested(sel.first()->data(Qt::UserRole).toInt());
}

} // namespace vtol
