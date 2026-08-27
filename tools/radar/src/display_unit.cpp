#include "display_unit.h"

#include <QVBoxLayout>

namespace simpit::radar {

DisplayUnit::DisplayUnit(DisplayUnitId id, QWidget* radarPage, QWidget* parent)
    : QWidget(parent), m_id(id)
{
    setObjectName(QStringLiteral("MFD-L"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(radarPage);
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

} // namespace simpit::radar
