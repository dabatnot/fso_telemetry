#pragma once

#include "av_ds_message.h"

#include <QWidget>

#include <array>

namespace simpit::radar {

class AvDsMessageOverlay;

enum class DisplayUnitId { MfdLeft };
enum class PageId { Radar };

class DisplayUnit final : public QWidget {
    Q_OBJECT
public:
    explicit DisplayUnit(DisplayUnitId id, QWidget* radarPage, QWidget* parent = nullptr);

    DisplayUnitId id() const noexcept;
    const std::array<PageId, 1>& pageCatalog() const noexcept;
    PageId activePage() const noexcept;
    bool setActivePage(PageId page) noexcept;
    AvDsMessage message() const;

public slots:
    void setMessage(const AvDsMessage& message);

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    DisplayUnitId m_id;
    PageId m_activePage = PageId::Radar;
    std::array<PageId, 1> m_catalog{PageId::Radar};
    AvDsMessageOverlay* m_messageOverlay = nullptr;
};

} // namespace simpit::radar
