#pragma once

#include "radar_client.h"

#include <QWidget>

namespace simpit::radar {

class RadarWidget final : public QWidget {
    Q_OBJECT
public:
    explicit RadarWidget(QWidget* parent = nullptr);

    QRectF radarCircleRect() const noexcept;
    std::shared_ptr<const RadarImage> image() const noexcept { return m_image; }
    ClientStatus status() const noexcept { return m_status; }

public slots:
    void setImage(std::shared_ptr<const RadarImage> image);
    void setStatus(ClientStatus status, const QString& detail = {});

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    void drawContact(QPainter& painter, const RadarContact& contact,
                     const QPointF& position) const;

    std::shared_ptr<const RadarImage> m_image;
    ClientStatus m_status = ClientStatus::Disconnected;
    QString m_detail;
};

} // namespace simpit::radar

