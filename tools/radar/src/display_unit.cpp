#include "display_unit.h"

#include "av_ds_message_overlay.h"

#include <QResizeEvent>
#include <QVBoxLayout>

namespace simpit::radar {

DisplayUnit::DisplayUnit(DisplayUnitId id, QWidget* radarPage, QWidget* parent)
    : QWidget(parent), m_id(id)
{
    setObjectName(QStringLiteral("MFD-L"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(radarPage);
    m_messageOverlay = new AvDsMessageOverlay(this);
    m_messageOverlay->setGeometry(rect());
}

DisplayUnitId DisplayUnit::id() const noexcept
{
    return m_id;
}

const std::array<PageId, 1>& DisplayUnit::pageCatalog() const noexcept
{
    return m_catalog;
}

PageId DisplayUnit::activePage() const noexcept
{
    return m_activePage;
}

bool DisplayUnit::setActivePage(PageId page) noexcept
{
    if (page != PageId::Radar) return false;
    m_activePage = page;
    return true;
}

AvDsMessage DisplayUnit::message() const
{
    return m_messageOverlay->message();
}

void DisplayUnit::setMessage(const AvDsMessage& message)
{
    m_messageOverlay->setMessage(message);
}

void DisplayUnit::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    m_messageOverlay->setGeometry(rect());
    m_messageOverlay->raise();
}

} // namespace simpit::radar
