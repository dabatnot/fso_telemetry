#pragma once

#include <QWidget>

#include <array>

namespace simpit::radar {

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

private:
    DisplayUnitId m_id;
    PageId m_activePage = PageId::Radar;
    std::array<PageId, 1> m_catalog{PageId::Radar};
};

} // namespace simpit::radar
