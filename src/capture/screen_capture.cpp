#include "capture/screen_capture.h"

#include <QDebug>

#include <cstring>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace cm {

QString ScreenCapture::s_lastError;

void *ScreenCapture::s_memDC = nullptr;
void *ScreenCapture::s_bitmap = nullptr;
void *ScreenCapture::s_oldBitmap = nullptr;
void *ScreenCapture::s_bits = nullptr;
int ScreenCapture::s_w = 0;
int ScreenCapture::s_h = 0;

QString ScreenCapture::lastError()
{
    return s_lastError;
}

void ScreenCapture::release()
{
    if (s_memDC) {
        if (s_oldBitmap) {
            SelectObject(static_cast<HDC>(s_memDC), static_cast<HGDIOBJ>(s_oldBitmap));
            s_oldBitmap = nullptr;
        }
        DeleteDC(static_cast<HDC>(s_memDC));
        s_memDC = nullptr;
    }
    if (s_bitmap) {
        DeleteObject(static_cast<HBITMAP>(s_bitmap));
        s_bitmap = nullptr;
    }
    s_bits = nullptr;
    s_w = 0;
    s_h = 0;
}

bool ScreenCapture::ensureBuffers(int w, int h)
{
    if (s_memDC && s_w == w && s_h == h) {
        return true;    // 尺寸没变，直接复用
    }
    release();

    HDC hdcScreen = GetDC(nullptr);
    if (!hdcScreen) {
        s_lastError = QStringLiteral("GetDC 失败");
        return false;
    }

    HDC memDC = CreateCompatibleDC(hdcScreen);

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;        // 负数 = 自上而下，省一次翻转
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void *bits = nullptr;
    HBITMAP bmp = CreateDIBSection(hdcScreen, &bmi, DIB_RGB_COLORS,
                                   &bits, nullptr, 0);
    ReleaseDC(nullptr, hdcScreen);

    if (!memDC || !bmp || !bits) {
        if (bmp) {
            DeleteObject(bmp);
        }
        if (memDC) {
            DeleteDC(memDC);
        }
        s_lastError = QStringLiteral("创建 DIB 段失败");
        return false;
    }

    s_memDC = memDC;
    s_bitmap = bmp;
    s_bits = bits;
    s_oldBitmap = SelectObject(memDC, bmp);
    s_w = w;
    s_h = h;
    return true;
}

QImage ScreenCapture::grab(const QRect &screenRect)
{
    s_lastError.clear();

    const int x = screenRect.x();
    const int y = screenRect.y();
    const int w = screenRect.width();
    const int h = screenRect.height();

    if (w <= 0 || h <= 0) {
        s_lastError = QStringLiteral("非法矩形 %1x%2").arg(w).arg(h);
        return QImage();
    }
    if (!ensureBuffers(w, h)) {
        return QImage();
    }

    HDC hdcScreen = GetDC(nullptr);
    if (!hdcScreen) {
        s_lastError = QStringLiteral("GetDC 失败");
        return QImage();
    }

    const BOOL ok = BitBlt(static_cast<HDC>(s_memDC), 0, 0, w, h,
                           hdcScreen, x, y, SRCCOPY);
    ReleaseDC(nullptr, hdcScreen);

    if (!ok) {
        s_lastError = QStringLiteral("BitBlt 失败，错误码 %1").arg(GetLastError());
        return QImage();
    }

    // DIB 段的像素现在就在 s_bits 里，一次 memcpy 拷进 QImage 即可
    QImage img(w, h, QImage::Format_RGB32);
    if (img.isNull()) {
        s_lastError = QStringLiteral("QImage 分配失败");
        return QImage();
    }
    std::memcpy(img.bits(), s_bits,
                static_cast<size_t>(w) * static_cast<size_t>(h) * 4u);
    return img;
}

QImage ScreenCapture::grabFullScreen()
{
    return grab(QRect(GetSystemMetrics(SM_XVIRTUALSCREEN),
                      GetSystemMetrics(SM_YVIRTUALSCREEN),
                      GetSystemMetrics(SM_CXVIRTUALSCREEN),
                      GetSystemMetrics(SM_CYVIRTUALSCREEN)));
}

} // namespace cm
