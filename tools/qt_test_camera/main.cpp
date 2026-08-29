#include <QApplication>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QWidget>
#include <QImage>
#include <QPixmap>
#include <QScreen>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <algorithm>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

#define REQ_BUFS 4
#define V4L2_META_FMT_ZZH_MLX90640 v4l2_fourcc(0x5A, 0x4D, 0x4C, 0x58) /* ZMLX */

static int fd = -1;
static int sfd = -1;
static int mfd = -1;
static void *buffers[REQ_BUFS];
static size_t buflen[REQ_BUFS];
static void *mbufs[REQ_BUFS];
static size_t mlen[REQ_BUFS];
static int snap_flag = 0;
static int frame_count = 0;

static inline unsigned char clamp255(int v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (unsigned char)v;
}

// 显示方向校正：IMX415 竖装，横向 NV12 需旋转 90 度后正立显示。
// 0 = 逆时针 90（旧行为）
// 1 = 顺时针 90（默认，校正“倒置”）
// 2 = 逆时针 90 + 上下翻转
// 3 = 顺时针 90 + 上下翻转
// 若画面方向仍不对，改这里重新编译即可。
#define ROT_MODE 1

static void nv12_to_rgb_rot90(const unsigned char *nv12, int w, int h,
                              int dw, int dh, unsigned char *rgb) {
    const unsigned char *y = nv12;
    const unsigned char *uv = nv12 + w * h;
    for (int dy = 0; dy < dh; dy++) {
        unsigned char *dst = rgb + dy * dw * 3;
        for (int dx = 0; dx < dw; dx++) {
            int sx, sy;
            if (ROT_MODE == 1 || ROT_MODE == 3) {
                // 顺时针 90
                sx = w - 1 - dy * w / dh;
                sy = dx * h / dw;
            } else {
                // 逆时针 90
                sx = dy * w / dh;
                sy = h - 1 - dx * h / dw;
            }
            if (ROT_MODE == 2 || ROT_MODE == 3) sy = h - 1 - sy;  // 上下翻转
            int Y = y[sy * w + sx];
            int U = uv[(sy / 2) * w + (sx & ~1)] - 128;
            int V = uv[(sy / 2) * w + (sx & ~1) + 1] - 128;
            dst[0] = clamp255(Y + ((359 * V) >> 8));
            dst[1] = clamp255(Y - ((88 * U + 183 * V) >> 8));
            dst[2] = clamp255(Y + ((454 * U) >> 8));
            dst += 3;
        }
    }
}

static void copy_rgb888_to_qimage(QImage &image, const unsigned char *rgb,
                                  int width, int height) {
    const size_t row_bytes = (size_t)width * 3;
    for (int y = 0; y < height; y++) {
        memcpy(image.scanLine(y), rgb + (size_t)y * row_bytes, row_bytes);
    }
}

static void heat_lut(int t, unsigned char *dst) {
    if (t < 64) { dst[0] = 0; dst[1] = t * 4; dst[2] = 255; }
    else if (t < 128) { dst[0] = 0; dst[1] = 255; dst[2] = 255 - (t - 64) * 4; }
    else if (t < 192) { dst[0] = (t - 128) * 4; dst[1] = 255; dst[2] = 0; }
    else { dst[0] = 255; dst[1] = 255 - (t - 192) * 4; dst[2] = 0; }
}

static void meta_to_heatmap(const unsigned char *meta, int W, int H, unsigned char *rgb) {
    const unsigned short *pix = (const unsigned short *)(meta + 48);
    std::vector<int> vals(W * H);
    for (int i = 0; i < W * H; i++) vals[i] = pix[i];
    std::sort(vals.begin(), vals.end());
    int lo = vals[W * H * 5 / 100];
    int hi = vals[W * H * 95 / 100];
    static int dbg = 0;
    if (dbg < 3) { fprintf(stderr, "raw min=%d p5=%d p95=%d max=%d\n", vals[0], lo, hi, vals[W*H-1]); dbg++; }
    if (hi <= lo) hi = lo + 1;
    for (int i = 0; i < W * H; i++) {
        int v = pix[i];
        if (v < lo) v = lo;
        if (v > hi) v = hi;
        int t = (v - lo) * 255 / (hi - lo);
        heat_lut(t, rgb + i * 3);
    }
}

static int v4l2_open(const char *dev, int *w, int *h, size_t *sizeimg) {
    fd = open(dev, O_RDWR);
    if (fd < 0) { perror("open"); return -1; }
    struct v4l2_capability cap; memset(&cap, 0, sizeof(cap));
    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) { perror("QUERYCAP"); return -1; }
    printf("driver=%s card=%s\n", cap.driver, cap.card);

    struct v4l2_format fmt; memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    fmt.fmt.pix_mp.width = *w; fmt.fmt.pix_mp.height = *h;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
    fmt.fmt.pix_mp.num_planes = 1;
    if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) { perror("S_FMT"); return -1; }
    *w = fmt.fmt.pix_mp.width; *h = fmt.fmt.pix_mp.height;
    *sizeimg = fmt.fmt.pix_mp.plane_fmt[0].sizeimage;
    printf("fmt=%ux%u NV12 sizeimage=%zu\n", *w, *h, *sizeimg);

    struct v4l2_requestbuffers req; memset(&req, 0, sizeof(req));
    req.count = REQ_BUFS; req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(fd, VIDIOC_REQBUFS, &req) < 0) { perror("REQBUFS"); return -1; }

    for (unsigned int i = 0; i < req.count; i++) {
        struct v4l2_buffer buf; struct v4l2_plane plane;
        memset(&buf, 0, sizeof(buf)); memset(&plane, 0, sizeof(plane));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i; buf.length = 1; buf.m.planes = &plane;
        if (ioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) { perror("QUERYBUF"); return -1; }
        buflen[i] = plane.length;
        buffers[i] = mmap(NULL, plane.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, plane.m.mem_offset);
        if (buffers[i] == MAP_FAILED) { perror("mmap"); return -1; }
        if (ioctl(fd, VIDIOC_QBUF, &buf) < 0) { perror("QBUF"); return -1; }
    }
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (ioctl(fd, VIDIOC_STREAMON, &type) < 0) { perror("STREAMON"); return -1; }
    printf("streaming ok\n");
    return 0;
}

static int v4l2_dqbuf(void **data, size_t *len, unsigned int *seq) {
    struct v4l2_buffer buf; struct v4l2_plane plane;
    memset(&buf, 0, sizeof(buf)); memset(&plane, 0, sizeof(plane));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; buf.memory = V4L2_MEMORY_MMAP;
    buf.length = 1; buf.m.planes = &plane;
    if (ioctl(fd, VIDIOC_DQBUF, &buf) < 0) { perror("DQBUF"); return -1; }
    *data = buffers[buf.index]; *len = plane.bytesused; *seq = buf.sequence;
    return (int)buf.index;
}

static void v4l2_qbuf(int index) {
    struct v4l2_buffer buf; struct v4l2_plane plane;
    memset(&buf, 0, sizeof(buf)); memset(&plane, 0, sizeof(plane));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; buf.memory = V4L2_MEMORY_MMAP;
    buf.index = index; buf.length = 1; buf.m.planes = &plane;
    if (ioctl(fd, VIDIOC_QBUF, &buf) < 0) perror("QBUF");
}

static int meta_open(const char *dev, size_t *bs) {
    mfd = open(dev, O_RDWR | O_NONBLOCK);
    if (mfd < 0) { perror("open meta"); return -1; }
    struct v4l2_format fmt; memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_META_CAPTURE;
    fmt.fmt.meta.dataformat = V4L2_META_FMT_ZZH_MLX90640;
    if (ioctl(mfd, VIDIOC_S_FMT, &fmt) < 0) { perror("meta S_FMT"); return -1; }
    *bs = fmt.fmt.meta.buffersize;
    printf("meta ZMLX buffersize=%zu\n", *bs);

    struct v4l2_requestbuffers req; memset(&req, 0, sizeof(req));
    req.count = REQ_BUFS; req.type = V4L2_BUF_TYPE_META_CAPTURE; req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(mfd, VIDIOC_REQBUFS, &req) < 0) { perror("meta REQBUFS"); return -1; }
    for (unsigned int i = 0; i < req.count; i++) {
        struct v4l2_buffer buf; memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_META_CAPTURE; buf.memory = V4L2_MEMORY_MMAP; buf.index = i;
        if (ioctl(mfd, VIDIOC_QUERYBUF, &buf) < 0) { perror("meta QUERYBUF"); return -1; }
        mlen[i] = buf.length;
        mbufs[i] = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, buf.m.offset);
        if (mbufs[i] == MAP_FAILED) { perror("meta mmap"); return -1; }
        if (ioctl(mfd, VIDIOC_QBUF, &buf) < 0) { perror("meta QBUF"); return -1; }
    }
    enum v4l2_buf_type type = V4L2_BUF_TYPE_META_CAPTURE;
    if (ioctl(mfd, VIDIOC_STREAMON, &type) < 0) { perror("meta STREAMON"); return -1; }
    return 0;
}

static int meta_dqbuf(void **data, size_t *len) {
    struct v4l2_buffer buf; memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_META_CAPTURE; buf.memory = V4L2_MEMORY_MMAP;
    if (ioctl(mfd, VIDIOC_DQBUF, &buf) < 0) return -1;
    *data = mbufs[buf.index]; *len = buf.bytesused;
    return (int)buf.index;
}

static void meta_qbuf(int index) {
    struct v4l2_buffer buf; memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_META_CAPTURE; buf.memory = V4L2_MEMORY_MMAP; buf.index = index;
    if (ioctl(mfd, VIDIOC_QBUF, &buf) < 0) perror("meta QBUF");
}

static void v4l2_close() {
    if (fd >= 0) {
        enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        ioctl(fd, VIDIOC_STREAMOFF, &t);
        for (int i = 0; i < REQ_BUFS; i++) if (buffers[i]) munmap(buffers[i], buflen[i]);
        close(fd); fd = -1;
    }
    if (mfd >= 0) {
        enum v4l2_buf_type t = V4L2_BUF_TYPE_META_CAPTURE;
        ioctl(mfd, VIDIOC_STREAMOFF, &t);
        for (int i = 0; i < REQ_BUFS; i++) if (mbufs[i]) munmap(mbufs[i], mlen[i]);
        close(mfd); mfd = -1;
    }
    if (sfd >= 0) { close(sfd); sfd = -1; }
}

static int ctrl_set(int id, int val) {
    struct v4l2_control c; memset(&c, 0, sizeof(c));
    c.id = id; c.value = val;
    return ioctl(sfd, VIDIOC_S_CTRL, &c);
}

static int ctrl_get(int id, int *val) {
    struct v4l2_control c; memset(&c, 0, sizeof(c));
    c.id = id;
    if (ioctl(sfd, VIDIOC_G_CTRL, &c) < 0) return -1;
    *val = c.value;
    return 0;
}

static int ctrl_query(int id, int *minv, int *maxv, int *defv) {
    struct v4l2_queryctrl q; memset(&q, 0, sizeof(q));
    q.id = id;
    if (ioctl(sfd, VIDIOC_QUERYCTRL, &q) < 0) return -1;
    *minv = q.minimum; *maxv = q.maximum; *defv = q.default_value;
    return 0;
}

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    const char *dev = argc > 1 ? argv[1] : "/dev/video45";
    const char *sdev = argc > 2 ? argv[2] : "/dev/v4l-subdev2";
    const char *mdev = argc > 3 ? argv[3] : "/dev/video0";
    int w = argc > 4 ? atoi(argv[4]) : 3840;
    int h = argc > 5 ? atoi(argv[5]) : 2160;
    size_t sizeimg = 0, mbs = 0;

    app.setStyleSheet(
        "QWidget { background-color: #111111; }"
        "QLabel { color: #ffffff; font-size: 26px; }"
        "QPushButton { min-height: 64px; font-size: 24px; background: #2b6cb0; "
        "color: #ffffff; border-radius: 10px; padding: 8px 18px; }"
        "QSlider::groove:horizontal { height: 26px; background: #444444; border-radius: 8px; }"
        "QSlider::sub-page:horizontal { background: #4da3ff; border-radius: 8px; }"
        "QSlider::handle:horizontal { width: 92px; height: 92px; margin: -33px 0; "
        "background: #ffffff; border: 3px solid #4da3ff; border-radius: 46px; }");

    if (v4l2_open(dev, &w, &h, &sizeimg) < 0) { fprintf(stderr, "capture open failed\n"); return 1; }
    sfd = open(sdev, O_RDWR);
    if (sfd < 0) { perror("open subdev"); return 1; }
    int meta_ok = (meta_open(mdev, &mbs) == 0);

    int exp_min = 4, exp_max = 2242, exp_def = 2242;
    int gain_min = 0, gain_max = 240, gain_def = 0;
    if (ctrl_query(V4L2_CID_EXPOSURE, &exp_min, &exp_max, &exp_def) < 0) printf("no exposure\n");
    if (ctrl_query(V4L2_CID_ANALOGUE_GAIN, &gain_min, &gain_max, &gain_def) < 0) printf("no gain\n");
    int exp_cur = exp_def;
    int gain_cur = gain_def;
    if (ctrl_get(V4L2_CID_EXPOSURE, &exp_cur) < 0) perror("get exposure");
    if (ctrl_get(V4L2_CID_ANALOGUE_GAIN, &gain_cur) < 0) perror("get gain");
    exp_cur = std::max(exp_min, std::min(exp_cur, exp_max));
    gain_cur = std::max(gain_min, std::min(gain_cur, gain_max));

    QScreen *screen = QGuiApplication::primaryScreen();
    int sw = screen->geometry().width();
    int sh = screen->geometry().height();
    int barH = 360;
    int availW = sw;
    int availH = sh - barH;
    int paneW = availW / 2;
    int paneH = availH;
    // 相机画面横向 16:9，在左半 pane 内等比适配
    int dw = paneW;
    int dh = paneW * 9 / 16;
    if (dh > paneH) { dh = paneH; dw = paneH * 16 / 9; }
    printf("screen=%dx%d pane=%dx%d disp=%dx%d\n", sw, sh, paneW, paneH, dw, dh);

    QWidget win;
    win.setWindowTitle("QT V4L2 test camera");
    QLabel *view = new QLabel(&win);
    view->setAlignment(Qt::AlignCenter);
    view->setScaledContents(true);
    view->setStyleSheet("background-color:black;");

    QLabel *heat = new QLabel(&win);
    heat->setAlignment(Qt::AlignCenter);
    heat->setScaledContents(true);
    heat->setStyleSheet("background-color:black; border: 3px solid #ff8800;");

    QSlider *exp = new QSlider(Qt::Horizontal, &win);
    exp->setRange(exp_min, exp_max); exp->setValue(exp_cur); exp->setFixedHeight(120);
    QLabel *expLabel = new QLabel(&win);
    QSlider *gain = new QSlider(Qt::Horizontal, &win);
    gain->setRange(gain_min, gain_max); gain->setValue(gain_cur); gain->setFixedHeight(120);
    QLabel *gainLabel = new QLabel(&win);
    QPushButton *snap = new QPushButton("抓帧", &win);
    QPushButton *quit = new QPushButton("退出", &win);

    auto updLabels = [&]() {
        expLabel->setText(QString("曝光 %1").arg(exp->value()));
        gainLabel->setText(QString("增益 %1").arg(gain->value()));
    };
    updLabels();
    QObject::connect(exp, &QSlider::valueChanged, [&](int v) {
        if (ctrl_set(V4L2_CID_EXPOSURE, v) < 0) perror("set exposure");
        expLabel->setText(QString("曝光 %1").arg(v));
    });
    QObject::connect(gain, &QSlider::valueChanged, [&](int v) {
        if (ctrl_set(V4L2_CID_ANALOGUE_GAIN, v) < 0) perror("set gain");
        gainLabel->setText(QString("增益 %1").arg(v));
    });
    QObject::connect(snap, &QPushButton::clicked, [&](){ snap_flag = 1; });
    QObject::connect(quit, &QPushButton::clicked, [&](){ win.close(); });

    QWidget *bar = new QWidget(&win);
    bar->setFixedHeight(360);
    bar->setStyleSheet("background-color:#1e1e1e;");
    QHBoxLayout *row1 = new QHBoxLayout(); row1->addWidget(expLabel); row1->addWidget(exp, 1);
    QHBoxLayout *row2 = new QHBoxLayout(); row2->addWidget(gainLabel); row2->addWidget(gain, 1);
    QHBoxLayout *row3 = new QHBoxLayout(); row3->addStretch(1); row3->addWidget(snap); row3->addWidget(quit); row3->addStretch(1);
    QVBoxLayout *barLay = new QVBoxLayout(bar);
    barLay->setContentsMargins(12, 8, 12, 8); barLay->setSpacing(28);
    barLay->addLayout(row1); barLay->addLayout(row2); barLay->addLayout(row3);

    QWidget *displayRow = new QWidget(&win);
    QHBoxLayout *dispLay = new QHBoxLayout(displayRow);
    dispLay->setContentsMargins(0, 0, 0, 0); dispLay->setSpacing(0);
    dispLay->addWidget(view, 1); dispLay->addWidget(heat, 1);

    QVBoxLayout *lay = new QVBoxLayout(&win);
    lay->setContentsMargins(0, 0, 0, 0); lay->setSpacing(0);
    lay->addWidget(displayRow, 1); lay->addWidget(bar);
    win.showFullScreen();

    QImage img(dw, dh, QImage::Format_RGB888);
    std::vector<unsigned char> rgb((size_t)dw * dh * 3);
    QImage hmap(32, 24, QImage::Format_RGB888);
    std::vector<unsigned char> hrgb(32 * 24 * 3);

    while (win.isVisible()) {
        void *data = NULL; size_t len = 0; unsigned int seq = 0;
        int idx = v4l2_dqbuf(&data, &len, &seq);
        if (idx < 0) break;

        nv12_to_rgb_rot90((const unsigned char*)data, w, h, dw, dh, rgb.data());
        copy_rgb888_to_qimage(img, rgb.data(), dw, dh);
        view->setPixmap(QPixmap::fromImage(img));

        if (meta_ok) {
            void *mdata = NULL; size_t mlenv = 0;
            int midx = meta_dqbuf(&mdata, &mlenv);
            if (midx >= 0) {
                meta_to_heatmap((const unsigned char*)mdata, 32, 24, hrgb.data());
                copy_rgb888_to_qimage(hmap, hrgb.data(), 32, 24);
                heat->setPixmap(QPixmap::fromImage(hmap));
                meta_qbuf(midx);
            }
        }

        frame_count++;
        win.setWindowTitle(QString("frame %1 seq %2").arg(frame_count).arg(seq));
        if (snap_flag) {
            snap_flag = 0;
            QImage s = img.copy();
            if (s.save("/tmp/frame.bmp")) printf("saved /tmp/frame.bmp\n");
        }
        v4l2_qbuf(idx);
        app.processEvents();
    }

    v4l2_close();
    return 0;
}
