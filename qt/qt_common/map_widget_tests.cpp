#include "testing/testing.hpp"

#include "qt/qt_common/helpers.hpp"
#include "qt/qt_common/map_widget.hpp"
#include "qt/qt_common/map_widget_container.hpp"

#include "qt_tstfrm/test_main_loop.hpp"

#include "map/framework.hpp"
#include "map/place_page_info.hpp"
#include "map/search_api.hpp"

#include "drape_frontend/visual_params.hpp"

#include <QtCore/QDeadlineTimer>
#include <QtCore/QThread>
#include <QtWidgets/QApplication>
#include <QtWidgets/QDockWidget>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMainWindow>

#include <cmath>
#include <utility>

namespace
{
class TestFramework final : public Framework
{
public:
  TestFramework() : Framework({}, false /* loadMaps */) {}
  ScreenBase const & GetScreen() const { return m_currentModelView; }
};

template <typename Predicate>
bool WaitUntil(Predicate const & ready)
{
  QDeadlineTimer deadline(5000);
  do
  {
    QApplication::processEvents();
    if (ready())
      return true;
    QThread::msleep(1);
  }
  while (!deadline.hasExpired());
  return false;
}

}  // namespace

UNIT_TEST(QtMap_DocksKeepSurfaceAndRefreshViewportSearch)
{
  Q_INIT_RESOURCE(resources_common);
  RunTestInOpenGLOffscreenEnvironment("Map dock integration", []
  {
    qt::common::SetDefaultSurfaceFormat(QApplication::platformName());
    int searchesCompleted = 0;
    TestFramework framework;
    auto const & screen = framework.GetScreen();
    framework.AddString("core_placepage_unknown_place", "Unknown place");
    TEST(!framework.GetSearchAPI().IsViewportInitialized(), ());

    QMainWindow window;
    window.resize(1000, 700);
    window.setAnimated(false);
    auto * map = new qt::common::MapWidget(framework, false /* isScreenshotMode */, &window);
    auto * container = new qt::common::MapWidgetContainer(window, *map);
    window.setCentralWidget(container);
    QObject::connect(container, &qt::common::MapWidgetContainer::VisibleViewportChanged, map,
                     &qt::common::MapWidget::SetVisibleViewport);
    auto * dock = new QDockWidget("Place page", &window);
    dock->setWidget(new QLabel("Selected place"));
    dock->setMinimumWidth(300);
    window.addDockWidget(Qt::LeftDockWidgetArea, dock);
    dock->hide();
    framework.SetPlacePageListeners([&] { dock->show(); }, [&] { dock->hide(); }, {}, {});
    window.show();
    TEST(WaitUntil([&] { return map->isValid() && framework.GetSearchAPI().IsViewportInitialized(); }), ());

    framework.SetViewportCenter(m2::PointD(10, 20), 14, false /* isAnim */);
    TEST(WaitUntil(
             [&]
    {
      return screen.GetOrg().Length(m2::PointD(10, 20)) < 1e-8 &&
             std::abs(screen.GetScale() - df::GetScreenScale(14)) < 1e-12;
    }),
         ());
    auto const initial = screen;
    auto const surface = framework.GetCurrentViewport();
    auto const mapSize = map->size();
    double const ratio = map->devicePixelRatioF();

    search::ViewportSearchParams params;
    params.m_query = "cafe ";
    params.m_inputLocale = "en";
    params.m_onCompleted = [&](search::Results const & results)
    {
      if (results.IsEndedNormal())
        ++searchesCompleted;
    };
    framework.GetSearchAPI().SearchInViewport(std::move(params));
    TEST(WaitUntil([&] { return searchesCompleted > 0; }), ());
    auto const beforeDock = searchesCompleted;
    dock->show();
    TEST(WaitUntil([&] { return searchesCompleted > beforeDock; }), ("Dock did not refresh viewport search"));
    TEST(map->size() == mapSize, ());
    TEST_EQUAL(framework.GetCurrentViewport(), surface, ());
    auto const covered = initial.PtoG(initial.P3dtoP(m2::PointD(100 * ratio, 350 * ratio)));
    TEST(!framework.GetVisibleViewport().IsPointInside(covered), ());

    auto const beforeResize = searchesCompleted;
    window.resizeDocks({dock}, {450}, Qt::Horizontal);
    TEST(WaitUntil([&] { return searchesCompleted > beforeResize; }), ("Dock resize did not refresh search"));
    TEST_EQUAL(framework.GetCurrentViewport(), surface, ());
    auto const beforeClose = searchesCompleted;
    dock->hide();
    TEST(WaitUntil([&] { return searchesCompleted > beforeClose; }), ("Closing dock did not refresh search"));
    TEST_EQUAL(framework.GetVisibleViewport(), surface, ());
    framework.GetSearchAPI().CancelSearch(search::Mode::Viewport);

    m2::PointD const visiblePixel(750 * ratio, 350 * ratio);
    auto const visiblePoint = initial.PtoG(initial.P3dtoP(visiblePixel));
    place_page::BuildInfo info;
    info.m_source = place_page::BuildInfo::Source::User;
    info.m_match = place_page::BuildInfo::Match::Nothing;
    info.m_mercator = covered;
    framework.BuildAndSetPlacePageInfo(info);
    TEST(WaitUntil(
             [&]
    {
      auto const pixel = screen.PtoP3d(screen.GtoP(covered));
      auto const origin = map->mapFrom(container, QPoint());
      return dock->isVisible() && origin.x() > 0 && pixel.x > origin.x() * ratio &&
             pixel.x < (origin.x() + container->width()) * ratio;
    }),
         ("Covered selection stayed behind the dock"));
    TEST_ALMOST_EQUAL_ABS(screen.GetScale(), initial.GetScale(), 1e-12, ());

    framework.SetViewportCenter(visiblePoint, 16, true /* isAnim */, true /* trackVisibleViewport */);
    dock->hide();
    TEST(WaitUntil(
             [&]
    {
      return screen.PtoP3d(screen.GtoP(visiblePoint)).Length(framework.GetVisiblePixelCenter()) < 1e-3 &&
             std::abs(screen.GetScale() - df::GetScreenScale(16)) < 1e-12;
    }),
         ("Dock change lost the search target or zoom"));

    auto const center = screen.GetOrg();
    auto const scale = screen.GetScale();
    framework.SetViewportCenter(center, df::kDoNotChangeZoom, false /* isAnim */);
    auto const resized = mapSize + QSize(200, 100);
    window.resize(window.width() + 200, window.height() + 100);
    TEST(WaitUntil(
             [&]
    {
      return map->size() == resized && screen.PixelRectIn3d().SizeX() == static_cast<int>(resized.width() * ratio) &&
             screen.PixelRectIn3d().SizeY() == static_cast<int>(resized.height() * ratio);
    }),
         ("Window resize did not reach the map renderer"));
    TEST_ALMOST_EQUAL_ABS(screen.GetOrg().Length(center), 0.0, 1e-8, ());
    TEST_ALMOST_EQUAL_ABS(screen.GetScale(), scale, 1e-12, ());
  });
}
