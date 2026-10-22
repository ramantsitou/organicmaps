#include "testing/testing.hpp"

#include "qt/qt_common/map_widget_container.hpp"
#include "qt/qt_common/qtoglcontext.hpp"

#include "qt_tstfrm/test_main_loop.hpp"

#include "drape/gl_functions.hpp"

#include <QtCore/QEventLoop>
#include <QtCore/QTimer>
#include <QtGui/QOffscreenSurface>
#include <QtGui/QOpenGLContext>
#include <QtGui/QOpenGLFunctions>
#include <QtGui/QPainter>
#include <QtOpenGLWidgets/QOpenGLWidget>
#include <QtWidgets/QApplication>
#include <QtWidgets/QDockWidget>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QStatusBar>
#include <QtWidgets/QToolBar>

#include <array>
#include <functional>
#include <string>

namespace
{
void RunContextTest(std::string const & name, std::function<void(qt::common::QtRenderOGLContext &)> const & fn)
{
  RunTestInOpenGLOffscreenEnvironment(name, [&]
  {
    auto * root = QOpenGLContext::currentContext();
    QOffscreenSurface surface;
    surface.setFormat(root->format());
    surface.create();
    qt::common::QtRenderOGLContext context(root, &surface, QSize(1000, 600));
    context.MakeCurrent();
    GLFunctions::Init(dp::ApiVersion::OpenGLES3);
    fn(context);
  });
}

void PresentFrame(qt::common::QtRenderOGLContext & context, dp::Color color = dp::Color(255, 0, 255, 255))
{
  TEST(context.BeginRendering(), ());
  context.SetFramebuffer(nullptr);
  context.SetClearColor(color);
  context.Clear(dp::ClearBits::ColorBit, dp::ClearBits::ColorBit);
  GLFunctions::glBindFramebuffer(0);
  context.EndRendering();
  context.Present();
}

std::array<uint8_t, 4> ReadPixel(qt::common::QtRenderOGLContext const & context)
{
  auto * functions = QOpenGLContext::currentContext()->functions();
  GLuint framebuffer = 0;
  auto const texture = context.GetTextureHandle();
  functions->glGenFramebuffers(1, &framebuffer);
  functions->glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
  functions->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
  TEST_EQUAL(functions->glCheckFramebufferStatus(GL_FRAMEBUFFER), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE), ());
  std::array<uint8_t, 4> pixel{};
  functions->glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
  functions->glBindFramebuffer(GL_FRAMEBUFFER, 0);
  functions->glDeleteFramebuffers(1, &framebuffer);
  return pixel;
}

class MapSurface : public QOpenGLWidget
{
public:
  using QOpenGLWidget::QOpenGLWidget;
  int m_resizes = 0;
  QPoint const m_landmark{700, 300};

protected:
  void resizeGL(int, int) override { ++m_resizes; }
  void paintGL() override
  {
    QPainter painter(this);
    painter.fillRect(rect(), Qt::blue);
    painter.fillRect(QRect(m_landmark - QPoint(4, 4), QSize(9, 9)), Qt::red);
  }
};
}  // namespace

UNIT_TEST(QtFrame_CanRenderBeforeResizeMessage)
{
  RunContextTest("Initial frame", [](qt::common::QtRenderOGLContext & context)
  {
    PresentFrame(context);
    TEST(context.AcquireFrame(), ());
    TEST(context.GetTexRect() == QRectF(0, 0, 1000.0 / 1024, 600.0 / 1024), ());
  });
}

UNIT_TEST(QtFrame_PendingResizeKeepsPublishedGeometry)
{
  RunContextTest("Pending resize", [](qt::common::QtRenderOGLContext & context)
  {
    context.Resize(1000, 600);
    PresentFrame(context, dp::Color(255, 0, 0, 255));

    context.Resize(700, 600);
    TEST(context.AcquireFrame(), ());
    TEST(context.GetTexRect() == QRectF(0, 0, 1000.0 / 1024, 600.0 / 1024), ());
    TEST_EQUAL(ReadPixel(context), (std::array<uint8_t, 4>{255, 0, 0, 255}), ());

    PresentFrame(context, dp::Color(0, 0, 255, 255));
    TEST(context.AcquireFrame(), ());
    TEST(context.GetTexRect() == QRectF(0, 0, 700.0 / 1024, 600.0 / 1024), ());
    TEST_EQUAL(ReadPixel(context), (std::array<uint8_t, 4>{0, 0, 255, 255}), ());
  });
}

UNIT_TEST(QtFrame_GrowingSurfaceKeepsAcquiredImage)
{
  RunContextTest("Growing surface", [](qt::common::QtRenderOGLContext & context)
  {
    context.Resize(1000, 600);
    PresentFrame(context);
    TEST(context.AcquireFrame(), ());
    auto const texture = context.GetTextureHandle();
    auto const rect = context.GetTexRect();

    context.Resize(2500, 1600);
    TEST(context.BeginRendering(), ());
    TEST(context.AcquireFrame(), ());
    TEST_EQUAL(context.GetTextureHandle(), texture, ());
    TEST(context.GetTexRect() == rect, ());
    TEST_EQUAL(ReadPixel(context), (std::array<uint8_t, 4>{255, 0, 255, 255}), ());
    context.EndRendering();

    PresentFrame(context);
    TEST(context.AcquireFrame(), ());
    TEST(context.GetTexRect() == QRectF(0, 0, 2500.0 / 4096, 1600.0 / 2048), ());
  });
}

UNIT_TEST(QtFrame_RepeatedResizesKeepLatestImageAndGeometryTogether)
{
  RunContextTest("Repeated resizes", [](qt::common::QtRenderOGLContext & context)
  {
    for (int i = 0; i < 12; ++i)
    {
      int const width = i % 2 == 0 ? 1000 : 700;
      context.Resize(width, 600);
      PresentFrame(context, dp::Color(i, 0, 0, 255));
      if (i % 3 != 0)
        continue;

      TEST(context.AcquireFrame(), ());
      TEST(context.GetTexRect() == QRectF(0, 0, width / 1024.0, 600.0 / 1024), ());
      TEST_EQUAL(ReadPixel(context), (std::array<uint8_t, 4>{static_cast<uint8_t>(i), 0, 0, 255}), ());
    }
  });
}

UNIT_TEST(QtFrame_SharedConsumerKeepsPublishedImageDuringResize)
{
  RunContextTest("Shared consumer", [](qt::common::QtRenderOGLContext & context)
  {
    auto * consumer = QOpenGLContext::currentContext()->shareContext();
    QOffscreenSurface surface;
    surface.setFormat(consumer->format());
    surface.create();
    PresentFrame(context, dp::Color(255, 0, 0, 255));

    TEST(consumer->makeCurrent(&surface), ());
    {
      QOpenGLFramebufferObject target(QSize(16, 16));
      target.bind();
      TEST(context.AcquireFrame(), ());
      TEST_EQUAL(ReadPixel(context), (std::array<uint8_t, 4>{255, 0, 0, 255}), ());

      context.MakeCurrent();
      context.Resize(2500, 1600);
      TEST(context.BeginRendering(), ());
      context.EndRendering();

      TEST(consumer->makeCurrent(&surface), ());
      TEST(context.AcquireFrame(), ());
      TEST(context.GetTexRect() == QRectF(0, 0, 1000.0 / 1024, 600.0 / 1024), ());
      TEST_EQUAL(ReadPixel(context), (std::array<uint8_t, 4>{255, 0, 0, 255}), ());
    }
    context.MakeCurrent();
  });
}

UNIT_TEST(QtFrame_DockChangesKeepClippedOpenGLImageStable)
{
  RunTestInOpenGLOffscreenEnvironment("Clipped map surface", []
  {
    QMainWindow window;
    window.resize(1000, 700);
    window.setAnimated(false);
    window.addToolBar("Toolbar")->addAction("Action");
    window.statusBar()->showMessage("Status");
    auto * map = new MapSurface(&window);
    auto * container = new qt::common::MapWidgetContainer(window, *map);
    window.setCentralWidget(container);
    auto * left = new QDockWidget("Place page", &window);
    auto * right = new QDockWidget("Search", &window);
    left->setWidget(new QLabel("Place"));
    right->setWidget(new QLabel("Results"));
    window.addDockWidget(Qt::LeftDockWidgetArea, left);
    window.addDockWidget(Qt::RightDockWidgetArea, right);
    left->hide();
    right->hide();
    window.show();
    auto const processLayout = []
    {
      for (int i = 0; i < 4; ++i)
        QApplication::processEvents();
    };
    processLayout();
    TEST(map->isValid(), ());
    auto const size = map->size();
    auto const resizes = map->m_resizes;
    QPoint const position = map->mapTo(&window, map->m_landmark);
    window.setAnimated(true);
    for (int i = 0; i < 12; ++i)
    {
      left->setVisible(i % 2 == 0);
      right->setVisible(i % 3 == 0);
      window.resizeDocks({left, right}, {300, 200}, Qt::Horizontal);
      processLayout();
      QEventLoop loop;
      QTimer::singleShot(250, &loop, &QEventLoop::quit);
      loop.exec();
      processLayout();
      TEST(map->size() == size, ());
      TEST_EQUAL(map->m_resizes, resizes, ());
      TEST(map->mapTo(&window, map->m_landmark) == position, ());
      // macOS CI's software renderer supports FBOs but cannot composite OpenGL widgets.
      QImage const image = map->grabFramebuffer();
      TEST(!image.isNull(), ());
      TEST(image.pixelColor(map->m_landmark * image.devicePixelRatio()) == QColor(Qt::red), ());
    }
  });
}
