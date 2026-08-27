#pragma once

#include "av_ds_message.h"
#include "vfnt_font.h"

#include <QWidget>

namespace simpit::radar {

class AvDsMessageOverlay final : public QWidget {
    Q_OBJECT
public:
    explicit AvDsMessageOverlay(QWidget* parent = nullptr);

    AvDsMessage message() const { return m_message; }

public slots:
    void setMessage(const AvDsMessage& message);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    AvDsMessage m_message;
    VfntFont m_titleFont{QStringLiteral(":/radar/fonts/font02.vf")};
    VfntFont m_detailFont{QStringLiteral(":/radar/fonts/font01.vf")};
};

} // namespace simpit::radar
