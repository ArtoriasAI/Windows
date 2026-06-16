#include "LapesEye/ui/GlRenderThread.h"
#if LEYE_HAS_GL

#include "LapesEye/core/PerfTimer.h"
#include <QPainter>
#include <QGuiApplication>

// Vertex shader — identyczny jak ThumbnailCanvas
static const char* TC_VERT = R"GLSL(
#version 330 core
layout(location=0) in vec2 a_pos;
layout(location=1) in vec2 a_uv;
uniform mat4 u_mvp;
out vec2 v_uv;
void main() {
    gl_Position = u_mvp * vec4(a_pos, 0.0, 1.0);
    v_uv = a_uv;
}
)GLSL";

static const char* TC_FRAG = R"GLSL(
#version 330 core
in vec2 v_uv;
uniform sampler2D u_tex;
uniform vec4  u_color;
uniform float u_use_tex;
uniform float u_alpha;
uniform float u_flip_y;
out vec4 frag;
void main() {
    vec2 uv = (u_flip_y > 0.5) ? vec2(v_uv.x, 1.0 - v_uv.y) : v_uv;
    vec4 c = (u_use_tex > 0.5) ? texture(u_tex, uv) : u_color;
    c.a *= u_alpha;
    frag = c;
}
)GLSL";

static constexpr int CELL_GAP   = 4;
static constexpr int TC_PAD     = 6;
static constexpr int TC_NAME_H  = 16;
static constexpr int TC_STARS_H = 12;
static constexpr int MAX_UPLOADS_PER_FRAME = 4;

namespace LapesEye {

// ────────────────────────────────────────────────────────────────────────────
GlRenderThread::GlRenderThread(QObject* parent) : QThread(parent) {}

GlRenderThread::~GlRenderThread() {
    stop();
    wait();
}

// ── UI wątek: utwórz surface (musi być w UI wątku) ───────────────────────────
void GlRenderThread::init_surface() {
    // QOffscreenSurface wymaga tworzenia w UI wątku (głównym wątku aplikacji)
    // Używamy formatu z domyślnego kontekstu Qt żeby mieć OpenGL 4.5 Core
    QSurfaceFormat fmt = QSurfaceFormat::defaultFormat();
    if (fmt.majorVersion() < 4) {
        fmt.setVersion(4, 5);
        fmt.setProfile(QSurfaceFormat::CoreProfile);
    }
    m_surf = new QOffscreenSurface(nullptr, this);
    m_surf->setFormat(fmt);
    m_surf->create();
    if (!m_surf->isValid())
        qWarning() << "[GlRenderThread] init_surface: QOffscreenSurface invalid";
    else
        qDebug() << "[GlRenderThread] surface OK, format:" << m_surf->format();
}

// ── UI wątek: przekaż stan do renderowania ───────────────────────────────────
void GlRenderThread::request_frame(const GlRenderState& state) {
    QMutexLocker lock(&m_mutex);
    m_pending_state = state;
    m_dirty = true;
    m_cond.wakeOne();
}

void GlRenderThread::queue_upload(const UploadRequest& req) {
    QMutexLocker lock(&m_mutex);
    // Zastąp istniejące żądanie dla tej samej ścieżki
    for (auto& r : m_pending_uploads) {
        if (r.path == req.path) { r = req; return; }
    }
    m_pending_uploads.append(req);
    m_dirty = true;
    m_cond.wakeOne();
}

void GlRenderThread::stop() {
    QMutexLocker lock(&m_mutex);
    m_stopping = true;
    m_cond.wakeOne();
}

// ── GL wątek: pętla główna ───────────────────────────────────────────────────
void GlRenderThread::run() {
    if (!gl_init()) {
        qWarning() << "[GlRenderThread] gl_init() failed";
        return;
    }

    qDebug() << "[GlRenderThread] start — OpenGL 4.5 DSA, dedykowany wątek";

    for (;;) {
        GlRenderState       state;
        QVector<UploadRequest> uploads;
        bool do_render = false;

        {
            QMutexLocker lock(&m_mutex);
            // Czekaj na pracę lub zatrzymanie
            while (!m_dirty && !m_stopping)
                m_cond.wait(&m_mutex);

            if (m_stopping) break;

            state   = m_pending_state;
            uploads = std::move(m_pending_uploads);
            m_pending_uploads.clear();
            do_render = m_dirty;
            m_dirty   = false;
        }

        if (!do_render) continue;

        if (!m_ctx->makeCurrent(m_surf)) {
            qWarning() << "[GlRenderThread] makeCurrent failed in loop";
            continue;
        }
        auto* f = gl();
        if (!f) { m_ctx->doneCurrent(); continue; }

        // Uploaduj tekstury z kolejki
        for (auto& req : uploads) {
            auto& e = m_gpu[req.path];
            if (!req.image.isNull())
                gpu_upload_thumb(e, req.image);
        }

        // Renderuj klatkę
        render_frame(state);

        m_ctx->doneCurrent();
    }

    // Cleanup w GL wątku
    m_ctx->makeCurrent(m_surf);
    gpu_delete_all();
    gl_cleanup();
    m_ctx->doneCurrent();
}

// ── Inicjalizacja GL ─────────────────────────────────────────────────────────
bool GlRenderThread::gl_init() {
    // QOffscreenSurface MUSI być tworzone w UI wątku
    // Tworzymy je przed run() przez init_surface() wywoływane z UI wątku
    if (!m_surf || !m_surf->isValid()) {
        qWarning() << "[GlRenderThread] surface nieprawidłowa — wywołaj init_surface() w UI wątku przed start()";
        return false;
    }

    m_ctx = new QOpenGLContext();
    m_ctx->setFormat(m_surf->requestedFormat());
    // KLUCZOWE: współdziel kontekst z głównym Qt context
    // Bez tego glCreateTextures/glNamedFramebufferTexture nie działają prawidłowo
    if (QOpenGLContext::globalShareContext())
        m_ctx->setShareContext(QOpenGLContext::globalShareContext());

    if (!m_ctx->create()) {
        qWarning() << "[GlRenderThread] QOpenGLContext::create() failed";
        return false;
    }

    if (!m_ctx->makeCurrent(m_surf)) {
        qWarning() << "[GlRenderThread] makeCurrent failed";
        return false;
    }


    if (!init_shader()) {
        m_ctx->doneCurrent();
        return false;
    }
    init_vao();

    m_gl_ok = true;
    m_ctx->doneCurrent();
    return true;
}

void GlRenderThread::gl_upload_folder_icon(GL45* f) {
    // Renderuj ikonę folderu przez QPainter na QImage 128×128
    QImage img(128, 128, QImage::Format_RGBA8888);
    img.fill(Qt::transparent);
    {
        QPainter p(&img);
        p.setRenderHint(QPainter::Antialiasing);
        // Tło folderu (dolna część)
        p.setBrush(QColor(0xf0, 0xb4, 0x2e));
        p.setPen(Qt::NoPen);
        p.drawRoundedRect(8, 40, 112, 76, 8, 8);
        // Zakładka folderu (góra)
        p.drawRoundedRect(8, 28, 52, 24, 6, 6);
        // Jaśniejszy highlight
        p.setBrush(QColor(0xff, 0xcc, 0x44, 120));
        p.drawRoundedRect(8, 40, 112, 30, 8, 8);
    }
    f->glGenTextures(1, &m_folder_icon_id);
    f->glBindTexture(GL_TEXTURE_2D, m_folder_icon_id);
    f->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 128, 128, 0,
                    GL_RGBA, GL_UNSIGNED_BYTE, img.constBits());
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    f->glBindTexture(GL_TEXTURE_2D, 0);
}

void GlRenderThread::gl_cleanup() {
    auto* f = gl();
    if (f) {
        if (m_vao) { f->glDeleteVertexArrays(1, &m_vao); m_vao = 0; }
        if (m_vbo) { f->glDeleteBuffers(1, &m_vbo);      m_vbo = 0; }
    }
    auto* fc = gl();
    if (fc) {
        if (m_fbo_id)         { fc->glDeleteFramebuffers(1, &m_fbo_id);         m_fbo_id  = 0; }
        if (m_fbo_tex)        { fc->glDeleteTextures(1,     &m_fbo_tex);        m_fbo_tex = 0; }
        if (m_folder_icon_id) { fc->glDeleteTextures(1,     &m_folder_icon_id); m_folder_icon_id = 0; }
    }
    delete m_prog; m_prog = nullptr;
}

bool GlRenderThread::init_shader() {
    m_prog = new QOpenGLShaderProgram();
    if (!m_prog->addShaderFromSourceCode(QOpenGLShader::Vertex,   TC_VERT) ||
        !m_prog->addShaderFromSourceCode(QOpenGLShader::Fragment, TC_FRAG) ||
        !m_prog->link()) {
        qWarning() << "[GlRenderThread] shader error:" << m_prog->log();
        delete m_prog; m_prog = nullptr;
        return false;
    }
    m_u_mvp     = m_prog->uniformLocation("u_mvp");
    m_u_color   = m_prog->uniformLocation("u_color");
    m_u_use_tex = m_prog->uniformLocation("u_use_tex");
    m_u_alpha   = m_prog->uniformLocation("u_alpha");
    m_u_flip_y  = m_prog->uniformLocation("u_flip_y");
    return true;
}

void GlRenderThread::init_vao() {
    auto* f = gl(); if (!f) return;
    static const float Q[] = {
        0.f, 0.f,  0.f, 0.f,
        1.f, 0.f,  1.f, 0.f,
        1.f, 1.f,  1.f, 1.f,
        0.f, 0.f,  0.f, 0.f,
        1.f, 1.f,  1.f, 1.f,
        0.f, 1.f,  0.f, 1.f,
    };
    f->glGenVertexArrays(1, &m_vao);
    f->glGenBuffers(1, &m_vbo);
    f->glBindVertexArray(m_vao);
    f->glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    f->glBufferData(GL_ARRAY_BUFFER, sizeof(Q), Q, GL_STATIC_DRAW);
    f->glEnableVertexAttribArray(0);
    f->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float),
                             (void*)0);
    f->glEnableVertexAttribArray(1);
    f->glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float),
                             (void*)(2*sizeof(float)));
    f->glBindVertexArray(0);
    f->glBindBuffer(GL_ARRAY_BUFFER, 0);
}

GlRenderThread::GL45* GlRenderThread::gl() const {
    return QOpenGLVersionFunctionsFactory::get<GL45>(m_ctx);
}

void GlRenderThread::gl_ensure_fbo(int w, int h) {
    // Nigdy nie zmniejszaj FBO — eliminuje mruganie gdy scrollbar
    // pojawia/znika i zmienia wysokość viewportu o kilkadziesiąt pikseli.
    // FBO jest co najwyżej kilka MB — rozrzutność jest pomijalnie mała.
    if (m_fbo_id && w <= m_fbo_size.width() && h <= m_fbo_size.height()) return;
    // Powiększ do max z bieżącego i poprzedniego rozmiaru
    w = qMax(w, m_fbo_size.width());
    h = qMax(h, m_fbo_size.height());

    auto* f = gl(); if (!f) return;

    // Usuń stary FBO/teksturę
    if (m_fbo_id)  { f->glDeleteFramebuffers(1, &m_fbo_id);  m_fbo_id  = 0; }
    if (m_fbo_tex) { f->glDeleteTextures(1,      &m_fbo_tex); m_fbo_tex = 0; }

    // Utwórz teksturę przez klasyczne API (nie DSA) — bezpieczniejsze
    // w kontekście cross-thread bez pełnego share group
    f->glGenTextures(1, &m_fbo_tex);
    f->glBindTexture(GL_TEXTURE_2D, m_fbo_tex);
    f->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0,
                    GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    f->glBindTexture(GL_TEXTURE_2D, 0);

    // Utwórz FBO przez klasyczne API i podłącz teksturę
    f->glGenFramebuffers(1, &m_fbo_id);
    f->glBindFramebuffer(GL_FRAMEBUFFER, m_fbo_id);
    f->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, m_fbo_tex, 0);

    GLenum status = f->glCheckFramebufferStatus(GL_FRAMEBUFFER);
    f->glBindFramebuffer(GL_FRAMEBUFFER, 0);

    if (status != GL_FRAMEBUFFER_COMPLETE) {
        // Pobierz szczegółowy błąd GL
        GLenum err = f->glGetError();
        qWarning() << "[GlRenderThread] FBO incomplete:" << status
                   << "GL error:" << err << "size:" << w << "x" << h;
        f->glDeleteFramebuffers(1, &m_fbo_id);  m_fbo_id  = 0;
        f->glDeleteTextures(1,      &m_fbo_tex); m_fbo_tex = 0;
        return;
    }
    m_fbo_size = QSize(w, h);
    qDebug() << "[GlRenderThread] FBO OK:" << w << "x" << h;
}

// ── Renderowanie klatki ──────────────────────────────────────────────────────
void GlRenderThread::render_frame(const GlRenderState& state) {
    PERF_SCOPE("paintGL_canvas");
    if (!m_gl_ok || !m_prog) return;

    const QRect& clip = state.clip;
    const int vw = clip.width(), vh = clip.height();
    if (vw <= 0 || vh <= 0) return;

    auto* f = gl(); if (!f) return;

    gl_ensure_fbo(vw, vh);
    if (!m_fbo_id) return;

    // Bind FBO — render do tekstury
    f->glBindFramebuffer(GL_FRAMEBUFFER, m_fbo_id);
    m_proj.setToIdentity();
    m_proj.ortho(clip.left(), clip.right(), clip.top() + vh, clip.top(), -1.f, 1.f);
    f->glViewport(0, 0, vw, vh);
    f->glClearColor(0x1e/255.f, 0x1e/255.f, 0x1e/255.f, 1.f);
    f->glClear(GL_COLOR_BUFFER_BIT);

    gl_paint_region(f, state);

    // Odczyt pikseli — czytaj tylko tyle ile faktycznie zajmują miniatury
    // (content_h ≤ vh gdy mało zdjęć, lub vh gdy normalny scroll)
    const int c   = qMax(1, (state.canvas_width - CELL_GAP) / (state.thumb_size + CELL_GAP));
    const int cw2 = (state.canvas_width - (c+1)*CELL_GAP) / c;
    const int ch2 = cw2 + TC_NAME_H + TC_STARS_H + TC_PAD * 2;
    const int n_rows = state.items.isEmpty() ? 0 : (state.items.size() + c - 1) / c;
    const int total_content_h = n_rows * (ch2 + CELL_GAP) + CELL_GAP;
    // Wysokość do odczytu = min(viewport, faktyczna zawartość powyżej scrollu+vh)
    int read_h = vh;
    if (total_content_h < clip.top() + vh)
        read_h = qMax(0, total_content_h - clip.top());
    read_h = qBound(1, read_h, vh);

    QImage img(vw, read_h, QImage::Format_RGBA8888);
    // Czytamy od góry FBO (y=vh-read_h bo OpenGL Y jest od dołu)
    f->glReadPixels(0, vh - read_h, vw, read_h, GL_RGBA, GL_UNSIGNED_BYTE, img.bits());
    f->glBindFramebuffer(GL_FRAMEBUFFER, 0);

    if (!img.isNull()) {
        // flip Y + wyznacz clip odpowiadający odczytanemu obszarowi
        QRect read_clip(clip.left(), clip.top(), vw, read_h);
        emit frame_ready(img.flipped(Qt::Vertical), read_clip);
    }
}

// ── GPU upload ───────────────────────────────────────────────────────────────
void GlRenderThread::gpu_upload_thumb(GpuEntry& e, const QImage& img) {
    auto* f = gl(); if (!f) return;

    QImage rgba = img.convertToFormat(QImage::Format_RGBA8888);
    const int w = rgba.width(), h = rgba.height();

    if (!e.thumb_id || e.thumb_src != QSize(w, h)) {
        if (e.thumb_id) f->glDeleteTextures(1, &e.thumb_id);
        f->glGenTextures(1, &e.thumb_id);
        f->glBindTexture(GL_TEXTURE_2D, e.thumb_id);
        f->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0,
                        GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        f->glBindTexture(GL_TEXTURE_2D, 0);
        e.thumb_src = QSize(w, h);
    }
    f->glBindTexture(GL_TEXTURE_2D, e.thumb_id);
    f->glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h,
                       GL_RGBA, GL_UNSIGNED_BYTE, rgba.constBits());
    f->glBindTexture(GL_TEXTURE_2D, 0);
    e.thumb_dirty = false;
}

void GlRenderThread::gpu_render_overlay(const QString& path, GpuEntry& e,
                                         int idx, int cw, int ch,
                                         const GlRenderState& state) {
    auto* f = gl(); if (!f) return;

    bool selected = state.selected.contains(path) && !state.drag_active;
    bool cut      = state.cut.contains(path);

    // Renderuj overlay przez QPainter na QImage — identycznie jak ThumbnailCanvas
    QImage ov(cw, ch, QImage::Format_ARGB32_Premultiplied);
    ov.fill(Qt::transparent);
    {
        QPainter p(&ov);
        p.setRenderHint(QPainter::Antialiasing);

        // Ramka zaznaczenia
        if (selected) {
            p.setPen(QPen(QColor(0x2d, 0x9c, 0xff), 2));
            p.drawRect(1, 1, cw - 2, ch - 2);
        }

        // Overlay cut
        if (cut) {
            p.fillRect(0, 0, cw, ch, QColor(0, 0, 0, 120));
        }

        // Nazwa pliku
        if (idx >= 0 && idx < state.items.size()) {
            const auto& item = state.items[idx];
            // Tło dla nazwy — zakrywa starą nazwę przy zmianie
            QRect name_bg(0, ch - TC_NAME_H - TC_PAD - 1, cw, TC_NAME_H + 2);
            p.fillRect(name_bg, QColor(0x28, 0x28, 0x28, 200));
            p.setPen(selected ? Qt::white : QColor(0xcc, 0xcc, 0xcc));
            p.setFont(QFont("sans", 8));
            QRect name_r(TC_PAD, ch - TC_NAME_H - TC_PAD,
                         cw - 2 * TC_PAD, TC_NAME_H);
            p.drawText(name_r, Qt::AlignCenter | Qt::TextSingleLine,
                       item.file.name);

            // Gwiazdki
            if (item.meta_loaded && item.meta.rating > 0) {
                QString stars(item.meta.rating, QChar(0x2605));
                QRect stars_r(TC_PAD, ch - TC_NAME_H - TC_STARS_H - TC_PAD - 2,
                               cw - 2 * TC_PAD, TC_STARS_H);
                p.setFont(QFont("sans", 7));
                p.setPen(QColor(0xff, 0xd7, 0x00));
                p.drawText(stars_r, Qt::AlignCenter, stars);
            }

            // Etykieta koloru
            if (item.meta_loaded && item.meta.color_label != ColorLabel::None) {
                QColor lc = [](ColorLabel c) -> QColor {
                    switch(c) {
                        case ColorLabel::Red:    return QColor(0xdd,0x44,0x44);
                        case ColorLabel::Green:  return QColor(0x44,0xbb,0x44);
                        case ColorLabel::Blue:   return QColor(0x44,0x88,0xdd);
                        case ColorLabel::Yellow: return QColor(0xdd,0xbb,0x22);
                        case ColorLabel::Purple: return QColor(0xaa,0x44,0xcc);
                        default: return Qt::transparent;
                    }
                }(item.meta.color_label);
                // Ramka wokół obszaru miniatury — identycznie jak CPU path
                int ir_x = TC_PAD + 1;
                int ir_y = TC_PAD + 1;
                int ir_w = cw - 2*TC_PAD - 2;
                int ir_h = ch - TC_NAME_H - TC_STARS_H - TC_PAD*2 - TC_PAD - 2;
                p.setPen(QPen(lc, 2));
                p.setBrush(Qt::NoBrush);
                p.drawRoundedRect(ir_x, ir_y, ir_w, ir_h, 4, 4);
            }

            // Oznaczenie (pick/reject)
            if (item.meta_loaded) {
                if (item.meta.pick_flag == PickFlag::Pick) {
                    p.setFont(QFont("sans", 9, QFont::Bold));
                    p.setPen(QColor(0x00, 0xff, 0x88));
                    p.drawText(QRect(2, 2, 20, 16), Qt::AlignLeft, "✓");
                } else if (item.meta.pick_flag == PickFlag::Reject) {
                    p.setFont(QFont("sans", 9, QFont::Bold));
                    p.setPen(QColor(0xff, 0x44, 0x44));
                    p.drawText(QRect(2, 2, 20, 16), Qt::AlignLeft, "✗");
                }
            }
        }
    }

    // Upload overlay jako tekstura GL
    QImage rgba = ov.convertToFormat(QImage::Format_RGBA8888);
    if (!e.overlay_id || e.ov_w != cw || e.ov_h != ch) {
        if (e.overlay_id) f->glDeleteTextures(1, &e.overlay_id);
        f->glGenTextures(1, &e.overlay_id);
        f->glBindTexture(GL_TEXTURE_2D, e.overlay_id);
        f->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, cw, ch, 0,
                        GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        f->glBindTexture(GL_TEXTURE_2D, 0);
        e.ov_w = cw; e.ov_h = ch;
    }
    f->glBindTexture(GL_TEXTURE_2D, e.overlay_id);
    f->glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, cw, ch,
                       GL_RGBA, GL_UNSIGNED_BYTE, rgba.constBits());
    f->glBindTexture(GL_TEXTURE_2D, 0);
    e.overlay_dirty = false;
}

void GlRenderThread::gpu_delete_entry(GpuEntry& e) {
    auto* f = gl(); if (!f) return;
    if (e.thumb_id)   { f->glDeleteTextures(1, &e.thumb_id);   e.thumb_id   = 0; }
    if (e.overlay_id) { f->glDeleteTextures(1, &e.overlay_id); e.overlay_id = 0; }
}

void GlRenderThread::gpu_delete_all() {
    for (auto& e : m_gpu) gpu_delete_entry(e);
    m_gpu.clear();
}

// ── Draw helpers ─────────────────────────────────────────────────────────────
void GlRenderThread::gl_draw_quad_color(GL45* f,
    float x, float y, float w, float h,
    float r, float g, float b, float a) {
    QMatrix4x4 m; m.translate(x, y); m.scale(w, h);
    m_prog->setUniformValue(m_u_mvp,     m_proj * m);
    m_prog->setUniformValue(m_u_use_tex, 0.f);
    m_prog->setUniformValue(m_u_color,   QVector4D(r, g, b, a));
    m_prog->setUniformValue(m_u_alpha,   1.f);
    m_prog->setUniformValue(m_u_flip_y,  0.f);
    f->glDrawArrays(GL_TRIANGLES, 0, 6);
}

void GlRenderThread::gl_draw_quad_tex(GL45* f,
    float x, float y, float w, float h,
    GLuint tex, float alpha, float flip_y) {
    QMatrix4x4 m; m.translate(x, y); m.scale(w, h);
    m_prog->setUniformValue(m_u_mvp,     m_proj * m);
    m_prog->setUniformValue(m_u_use_tex, 1.f);
    m_prog->setUniformValue(m_u_alpha,   alpha);
    m_prog->setUniformValue(m_u_flip_y,  flip_y);
    m_prog->setUniformValue("u_tex", 0);
    f->glActiveTexture(GL_TEXTURE0);
    f->glBindTexture(GL_TEXTURE_2D, tex);
    f->glDrawArrays(GL_TRIANGLES, 0, 6);
    f->glBindTexture(GL_TEXTURE_2D, 0);
}

// ── Rysowanie itemu ──────────────────────────────────────────────────────────
void GlRenderThread::gl_draw_item(GL45* f, int idx, const GlRenderState& state) {
    if (idx < 0 || idx >= state.items.size()) return;
    const auto& item = state.items[idx];

    // Oblicz item_rect — używaj canvas_width (pełna szerokość), nie clip.width()
    int canvas_w = state.canvas_width > 0 ? state.canvas_width : state.clip.width();
    int c  = qMax(1, (canvas_w - CELL_GAP) / (state.thumb_size + CELL_GAP));
    int cw = (canvas_w - (c + 1) * CELL_GAP) / c;
    int ch = cw + TC_NAME_H + TC_STARS_H + TC_PAD * 2;

    int col = idx % c;
    int row = idx / c;
    int ix  = CELL_GAP + col * (cw + CELL_GAP);
    int iy  = CELL_GAP + row * (ch + CELL_GAP);
    QRect r(ix, iy, cw, ch);

    bool selected = state.selected.contains(item.file.path) && !state.drag_active;
    bool hovered  = (idx == state.hovered_idx);

    // Tło
    QColor bg(0x28, 0x28, 0x28);
    if (selected) bg = QColor(0x1a, 0x55, 0x9e);
    else if (hovered) bg = QColor(0x3a, 0x3a, 0x3a);
    gl_draw_quad_color(f, r.x(), r.y(), cw, ch,
                       bg.redF(), bg.greenF(), bg.blueF());

    // Obszar obrazu — kwadratowy (cw - 2*PAD) × (cw - 2*PAD)
    // Zapewnia że miniatury pionowe i poziome są jednakowo przycięte
    int aw = cw - 2 * TC_PAD;
    int ah = aw;  // kwadrat — miniatura skaluje się proporcjonalnie przez KeepAspectRatio
    int ix2 = r.x() + TC_PAD;
    int iy2 = r.y() + TC_PAD;

    auto& e = m_gpu[item.file.path];

    // Miniatura lub ikona folderu
    if (!item.thumb.isNull()) {
        if (e.thumb_dirty || e.thumb_src != item.thumb.size()) {
            gpu_upload_thumb(e, item.thumb.toImage());
        }
    }
    if (e.thumb_id) {
        QSize ts = item.thumb.size().scaled(QSize(aw, ah), Qt::KeepAspectRatio);
        float ox = ix2 + (aw - ts.width())  / 2.f;
        float oy = iy2 + (ah - ts.height()) / 2.f;
        gl_draw_quad_tex(f, ox, oy, ts.width(), ts.height(), e.thumb_id, 1.f, 0.f);
    } else if (!item.file.is_dir) {
        // Placeholder dla zwykłego pliku bez jeszcze załadowanej miniatury
        // Ciemniejszy prostokąt żeby nie było widać "doczytywania"
        float px = ix2, py = iy2;
        float pw = aw, ph = ah;
        gl_draw_quad_color(f, px, py, pw, ph, 0.20f, 0.20f, 0.20f);
    } else if (item.file.is_dir) {
        // Ikona folderu — renderuj przez overlay jeśli brak miniatury
        if (e.overlay_dirty || !e.overlay_id || e.ov_w != cw || e.ov_h != ch)
            gpu_render_overlay(item.file.path, e, idx, cw, ch, state);
        // Rysuj ikonę folderu wycentrowaną w obszarze obrazu
        if (!m_folder_icon_id) gl_upload_folder_icon(f);
        if (m_folder_icon_id) {
            int fsz = qMin(aw, ah) * 2 / 3;
            float fx = ix2 + (aw - fsz) / 2.f;
            float fy = iy2 + (ah - fsz) / 2.f;
            gl_draw_quad_tex(f, fx, fy, fsz, fsz, m_folder_icon_id, 0.85f, 0.f);
        }
    }

    // Overlay (nazwa, gwiazdki, zaznaczenie, etykieta koloru)
    // Sprawdź czy meta się zmieniło — porównaj hash
    bool meta_changed = (e.cached_rating    != (item.meta_loaded ? item.meta.rating : -1)) ||
                        (e.cached_pick      != (item.meta_loaded ? (int)item.meta.pick_flag : -1)) ||
                        (e.cached_label     != (item.meta_loaded ? (int)item.meta.color_label : -1));
    if (meta_changed) {
        e.cached_rating = item.meta_loaded ? item.meta.rating : -1;
        e.cached_pick   = item.meta_loaded ? (int)item.meta.pick_flag : -1;
        e.cached_label  = item.meta_loaded ? (int)item.meta.color_label : -1;
        e.overlay_dirty = true;
    }
    bool sel_changed = (e.cached_selected != selected);
    if (sel_changed) { e.cached_selected = selected; e.overlay_dirty = true; }

    if (e.overlay_dirty || !e.overlay_id || e.ov_w != cw || e.ov_h != ch)
        gpu_render_overlay(item.file.path, e, idx, cw, ch, state);
    if (e.overlay_id)
        gl_draw_quad_tex(f, r.x(), r.y(), cw, ch, e.overlay_id, 1.f, 0.f);
}

// ── Rysowanie widocznego regionu ────────────────────────────────────────────
void GlRenderThread::gl_paint_region(GL45* f, const GlRenderState& state) {
    if (!f || !m_gl_ok || !m_prog || !m_vao) return;
    if (state.items.isEmpty()) return;

    // Oblicz liczbę kolumn — canvas_width, nie clip (clip = tylko widoczny obszar)
    int canvas_w = state.canvas_width > 0 ? state.canvas_width : state.clip.width();
    int c = qMax(1, (canvas_w - CELL_GAP) / (state.thumb_size + CELL_GAP));
    int cw = (canvas_w - (c + 1) * CELL_GAP) / c;
    int ch = cw + TC_NAME_H + TC_STARS_H + TC_PAD * 2;
    if (c == 0 || ch == 0) return;

    const QRect& clip = state.clip;
    int row_h    = ch + CELL_GAP;
    int first_r  = qMax(0, (clip.top()    - CELL_GAP) / row_h);
    int last_r   = (clip.bottom()) / row_h + 1;
    int n_rows   = (state.items.size() + c - 1) / c;
    last_r       = qMin(last_r, n_rows - 1);

    m_prog->bind();
    f->glBindVertexArray(m_vao);
    f->glEnable(GL_BLEND);
    f->glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    int uploads_this_frame = 0;
    for (int row = first_r; row <= last_r; ++row)
        for (int col = 0; col < c; ++col) {
            int idx = row * c + col;
            if (idx >= state.items.size()) break;
            // Ogranicz uploady tekstur per klatka — zapobiega spike 35ms przy scrollu
            const auto& item = state.items[idx];
            auto& e = m_gpu[item.file.path];
            if (!item.thumb.isNull() && (e.thumb_dirty || e.thumb_src != item.thumb.size())) {
                if (uploads_this_frame >= MAX_UPLOADS_PER_FRAME) continue;
                ++uploads_this_frame;
            }
            gl_draw_item(f, idx, state);
        }

    f->glBindVertexArray(0);
    m_prog->release();
}

} // namespace LapesEye
#endif // LEYE_HAS_GL
