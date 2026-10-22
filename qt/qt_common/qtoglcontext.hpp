#pragma once

#include "drape/oglcontext.hpp"

#include <QOpenGLFramebufferObject>

#include <QtGui/QOffscreenSurface>
#include <QtGui/QOpenGLContext>

#include <atomic>
#include <memory>
#include <mutex>

namespace qt
{
namespace common
{
class QtRenderOGLContext : public dp::OGLContext
{
public:
  QtRenderOGLContext(QOpenGLContext * rootContext, QOffscreenSurface * surface, QSize const & size);

  bool BeginRendering() override;
  void Present() override;
  void MakeCurrent() override;
  void DoneCurrent() override;
  void SetFramebuffer(ref_ptr<dp::BaseFramebuffer> framebuffer) override;
  void Resize(uint32_t w, uint32_t h) override;

  bool AcquireFrame();
  GLuint GetTextureHandle() const;
  QRectF const & GetTexRect() const;

private:
  QOffscreenSurface * m_surface = nullptr;
  std::unique_ptr<QOpenGLContext> m_ctx;

  struct Frame
  {
    std::unique_ptr<QOpenGLFramebufferObject> m_buffer;
    QRectF m_texRect;
  };
  // The render thread owns back, the GUI thread owns acquired; front is exchanged under the mutex.
  Frame m_frontFrame;
  Frame m_backFrame;
  Frame m_acquiredFrame;
  QSize m_size;
  bool m_frameUpdated = false;

  std::atomic<bool> m_isContextAvailable;

  std::mutex m_frameMutex;
};

class QtUploadOGLContext : public dp::OGLContext
{
public:
  QtUploadOGLContext(QOpenGLContext * rootContext, QOffscreenSurface * surface);

  void Present() override;
  void MakeCurrent() override;
  void DoneCurrent() override;
  void SetFramebuffer(ref_ptr<dp::BaseFramebuffer> framebuffer) override;

private:
  QOffscreenSurface * m_surface = nullptr;  // non-owning ptr
  std::unique_ptr<QOpenGLContext> m_ctx;
};
}  // namespace common
}  // namespace qt
