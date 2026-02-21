#include "MapLegend.h"
#include "MapBackend.h"
#include <QVBoxLayout>
#include <QPainter>
#include <QPainterPath>

namespace vtol {

MapLegend::MapLegend(MapBackend* backend, QWidget* parent)
    : QWidget(parent)
{
    // No WA_TranslucentBackground — we draw the dark background ourselves in paintEvent
    setAutoFillBackground(false);

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(10, 8, 10, 8);
    lay->setSpacing(4);

    // Track
    auto* chkTrack = makeCheck("Трек", "#00E676", backend->showTrack());
    connect(chkTrack, &QCheckBox::toggled, backend, [backend](bool v) {
        backend->setShowTrack(v);
    });
    lay->addWidget(chkTrack);

    // Waypoints
    auto* chkWp = makeCheck("Маршрут", "#00C853", backend->showWaypoints());
    connect(chkWp, &QCheckBox::toggled, backend, [backend](bool v) {
        backend->setShowWaypoints(v);
    });
    lay->addWidget(chkWp);

    // Zones
    auto* chkZones = makeCheck("Зоны", "#FF0000", backend->showZones());
    connect(chkZones, &QCheckBox::toggled, backend, [backend](bool v) {
        backend->setShowZones(v);
    });
    lay->addWidget(chkZones);

    // Settlements
    auto* chkStl = makeCheck("Нас. пункты", "#CC0000", backend->showSettlements());
    connect(chkStl, &QCheckBox::toggled, backend, [backend](bool v) {
        backend->setShowSettlements(v);
    });
    lay->addWidget(chkStl);

    adjustSize();
}

QCheckBox* MapLegend::makeCheck(const QString& label, const QString& color, bool checked)
{
    auto* cb = new QCheckBox(label, this);
    cb->setChecked(checked);
    cb->setStyleSheet(QStringLiteral(
        "QCheckBox { color: #E2E8F0; font-size: 12px; spacing: 5px; background: transparent; }"
        "QCheckBox::indicator { width: 14px; height: 14px; border-radius: 3px; "
        "  border: 1.5px solid %1; background: transparent; }"
        "QCheckBox::indicator:checked { background: %1; }"
    ).arg(color));
    return cb;
}

void MapLegend::paintEvent(QPaintEvent* /*event*/)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    QPainterPath path;
    path.addRoundedRect(rect(), 6, 6);
    p.fillPath(path, QColor(11, 15, 26, 180));
}

} // namespace vtol
