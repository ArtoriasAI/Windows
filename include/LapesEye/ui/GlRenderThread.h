#pragma once
#if LEYE_HAS_GL

#include <QThread>
#include <QMutex>
#include <QWaitCondition>
#include <QImage>
#include <QPixmap>
#include <QHash>
#include <QSet>
#include <QVector>
#include <QMatrix4x4>
#include <QOpenGLContext>
#include <QOpenGLFunctions_4_5_Core>
#include <QOpenGLVersionFunctionsFactory>
#include <QOpenGLShaderProgram>
#include <QOffscreenSurface>
#include "LapesEye/core/FileScanner.h"
#include "LapesEye/core/MetaStore.h"
#include "LapesEye/ui/ThumbnailCanvasItem.h"

namespace LapesEye {

// ── Żądanie uploadu tekstury ────────────────────────────────────────────────
struct UploadRequest {
    QString  path;
    QImage   image;   // dane do uploadu (QImage thread-safe do kopiowania)
    bool     is_overlay = false;
    QImage   overlay_image;
    int      ov_w = 0, ov_h = 0;
};

// ── Stan renderowania przekazywany do GL wątku ──────────────────────────────
struct GlRenderState {
    QVector<ThumbnailCanvasItem> items;
    QSet<QString>  selected;
    QSet<QString>  cut;
    int            thumb_size    = 160;
    int            hovered_idx   = -1;
    bool           drag_active   = false;
    QRect          clip;          // obszar do renderowania (viewport)
    int            canvas_width  = 0;  // pełna szerokość canvasa (do cols())
    bool           is_scrolling  = false;
    int            total_count   = 0;
};

// ────────────────────────────────────────────────────────────────────────────
//  GlRenderThread — renderuje ThumbnailCanvas w osobnym wątku GL
//  UI wątek przekazuje stan przez set_render_state(), odbiera gotowe klatki
//  przez sygnał frame_ready().
// ────────────────────────────────────────────────────────────────────────────
class GlRenderThread : public QThread {
    Q_OBJECT
public:
    explicit GlRenderThread(QObject* parent = nullptr);
    ~GlRenderThread() override;

    // Wywołaj z UI wątku PRZED start() — tworzy QOffscreenSurface w UI wątku
    // (Qt wymaga żeby surface było tworzone w wątku gdzie będzie używane jako "native")
    void init_surface();

    // Wywołaj z UI wątku — przekazuje stan do renderowania
    void request_frame(const GlRenderState& state);

    // Wywołaj z UI wątku — kolejkuje upload tekstury do GL wątku
    void queue_upload(const UploadRequest& req);

    // Wywołaj z UI wątku — sygnalizuje zamknięcie
    void stop();

signals:
    // Emitowany z GL wątku gdy klatka gotowa — UI wątek odbiera i rysuje
    void frame_ready(QImage image, QRect clip);

protected:
    void run() override;

private:
    using GL45 = QOpenGLFunctions_4_5_Core;

    // ── GL zasoby (dostępne tylko z GL wątku) ──────────────────────────────
    QOpenGLContext*           m_ctx     = nullptr;
    QOffscreenSurface*        m_surf    = nullptr;
    GLuint                    m_fbo_id  = 0;   // ręczny FBO (DSA)
    GLuint                    m_fbo_tex = 0;
    GLuint                    m_folder_icon_id = 0;
    QSize                     m_fbo_size;
    QOpenGLShaderProgram*     m_prog   = nullptr;
    GLuint m_vao = 0, m_vbo = 0;
    bool   m_gl_ok = false;

    int m_u_mvp     = -1;
    int m_u_color   = -1;
    int m_u_use_tex = -1;
    int m_u_alpha   = -1;
    int m_u_flip_y  = -1;

    QMatrix4x4 m_proj;

    struct GpuEntry {
        GLuint thumb_id     = 0;
        GLuint overlay_id   = 0;
        bool   thumb_dirty  = true;
        bool   overlay_dirty= true;
        QSize  thumb_src;
        int    ov_w = 0, ov_h = 0;
        // Cache do wykrywania zmian — overlay regenerowany tylko gdy zmiana
        int    cached_rating   = -1;
        int    cached_pick     = -1;
        int    cached_label    = -1;
        bool   cached_selected = false;
    };
    QHash<QString, GpuEntry> m_gpu;

    // ── Synchronizacja UI↔GL ───────────────────────────────────────────────
    QMutex         m_mutex;
    QWaitCondition m_cond;
    bool           m_dirty    = false;   // jest nowy stan do renderowania
    bool           m_stopping = false;

    GlRenderState         m_pending_state;
    QVector<UploadRequest> m_pending_uploads;

    // ── Metody GL wątku ────────────────────────────────────────────────────
    bool  gl_init();
    void  gl_cleanup();
    void  gl_upload_folder_icon(GL45* f);
    void  gl_ensure_fbo(int w, int h);
    bool  init_shader();
    void  init_vao();
    GL45* gl() const;

    void  process_uploads(GL45* f);
    void  render_frame(const GlRenderState& state);

    void  gpu_upload_thumb(GpuEntry& e, const QImage& img);
    void  gpu_render_overlay(const QString& path, GpuEntry& e,
                              int idx, int cw, int ch,
                              const GlRenderState& state);
    void  gpu_delete_entry(GpuEntry& e);
    void  gpu_delete_all();

    void  gl_draw_quad_color(GL45* f,
                              float x, float y, float w, float h,
                              float r, float g, float b, float a = 1.f);
    void  gl_draw_quad_tex(GL45* f,
                            float x, float y, float w, float h,
                            GLuint tex, float alpha = 1.f, float flip_y = 0.f);
    void  gl_draw_item(GL45* f, int idx, const GlRenderState& state);
    void  gl_paint_region(GL45* f, const GlRenderState& state);
};

} // namespace LapesEye
#endif // LEYE_HAS_GL
