#pragma once

#include <QWidget>
#include <QCheckBox>

namespace vtol {

class MapBackend;

/// Semi-transparent overlay at the bottom of the map with layer visibility toggles.
class MapLegend : public QWidget {
    Q_OBJECT
public:
    explicit MapLegend(MapBackend* backend, QWidget* parent = nullptr);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QCheckBox* makeCheck(const QString& label, const QString& color, bool checked);
};

} // namespace vtol
