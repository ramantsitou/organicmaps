#include "qt/qt_common/qtoglcontext.hpp"

#include "base/assert.hpp"

#include "drape/gl_functions.hpp"

#include <bit>
#include <memory>

namespace qt
{
namespace common
{
QtRenderOGLContext::QtRenderOGLContext(QOpenGLContext * rootContext, QOffscreenSurface * surface, QSize const & size)
  : m_surface(surface)
  , m_ctx(std::make_unique<QOpenGLContext>())
  , m_size(size)
  , m_isContextAvailable(false)
{
  CHECK(!size.isEmpty(), ());
  m_ctx->setFormat(rootContext->format());
  m_ctx->setShareContext(rootContext);
  m_ctx->create();
  CHECK(m_ctx->isValid(), ());
}

bool QtRenderOGLContext::BeginRendering()
{
  ASSERT(!m_size.isEmpty(), ());
  QSize size(static_cast<int>(std::bit_ceil(static_cast<uint32_t>(m_size.width()))),
             static_cast<int>(std::bit_ceil(static_cast<uint32_t>(m_size.height()))));
  if (m_backFrame.m_buffer)
    size = size.expandedTo(m_backFrame.m_buffer->size());
  if (!m_backFrame.m_buffer || m_backFrame.m_buffer->size() != size)
    m_backFrame.m_buffer = std::make_unique<QOpenGLFramebufferObject>(size, QOpenGLFramebufferObject::Depth);
  return true;
}

void QtRenderOGLContext::Present()
{
  ASSERT(m_backFrame.m_buffer, ());
  GLFunctions::glFinish();

  auto const size = m_backFrame.m_buffer->size();
  m_backFrame.m_texRect = {0.0, 0.0, static_cast<double>(m_size.width()) / size.width(),
                           static_cast<double>(m_size.height()) / size.height()};
  std::lock_guard lock(m_frameMutex);
  std::swap(m_frontFrame, m_backFrame);
  m_frameUpdated = true;
}

void QtRenderOGLContext::MakeCurrent()
{
  CHECK(m_ctx->makeCurrent(m_surface), ());
  m_isContextAvailable = true;
}

void QtRenderOGLContext::DoneCurrent()
{
  m_isContextAvailable = false;
  m_ctx->doneCurrent();
}

void QtRenderOGLContext::SetFramebuffer(ref_ptr<dp::BaseFramebuffer> framebuffer)
{
  if (framebuffer)
    framebuffer->Bind();
  else if (m_backFrame.m_buffer)
    m_backFrame.m_buffer->bind();
}

void QtRenderOGLContext::Resize(uint32_t w, uint32_t h)
{
  CHECK(m_isContextAvailable, ());

  // Only prepare the next render target; published and acquired images stay valid until replaced.
  ASSERT_GREATER(w, 0, ());
  ASSERT_GREATER(h, 0, ());
  m_size = QSize(w, h);
}

bool QtRenderOGLContext::AcquireFrame()
{
  if (!m_isContextAvailable)
    return false;

  std::lock_guard lock(m_frameMutex);
  if (m_frameUpdated)
  {
    std::swap(m_acquiredFrame, m_frontFrame);
    m_frameUpdated = false;
  }
  return m_acquiredFrame.m_buffer != nullptr;
}

QRectF const & QtRenderOGLContext::GetTexRect() const
{
  ASSERT(m_acquiredFrame.m_buffer, ());
  return m_acquiredFrame.m_texRect;
}

GLuint QtRenderOGLContext::GetTextureHandle() const
{
  if (!m_acquiredFrame.m_buffer)
    return 0;

  CHECK(!m_acquiredFrame.m_buffer->isBound(), ());
  return m_acquiredFrame.m_buffer->texture();
}

QtUploadOGLContext::QtUploadOGLContext(QOpenGLContext * rootContext, QOffscreenSurface * surface)
  : m_surface(surface)
  , m_ctx(std::make_unique<QOpenGLContext>())
{
  m_ctx->setFormat(rootContext->format());
  m_ctx->setShareContext(rootContext);
  m_ctx->create();
  CHECK(m_ctx->isValid(), ());
}

void QtUploadOGLContext::MakeCurrent()
{
  m_ctx->makeCurrent(m_surface);
}

void QtUploadOGLContext::DoneCurrent()
{
  m_ctx->doneCurrent();
}

void QtUploadOGLContext::Present()
{
  CHECK(false, ());
}

void QtUploadOGLContext::SetFramebuffer(ref_ptr<dp::BaseFramebuffer>)
{
  CHECK(false, ());
}
}  // namespace common
}  // namespace qt
