#pragma once

// 屏幕采集。
//
// 两个关键决定
// ------------
// 1) 用「屏幕 DC + 窗口矩形」而不是 GetWindowDC / PrintWindow：
//    希沃这类 .NET/WPF 窗口是硬件渲染的，PrintWindow 很可能抓到全黑，
//    而屏幕 DC 拿到的是屏幕上真实显示的内容 —— 所见即所得。
//    代价：如果有别的窗口盖在上面，抓到的就是别的窗口。
//    所以调用方**必须先检查 isForeground()**，否则会把通知弹窗写进板书。
//
// 2) 用 CreateDIBSection 而不是 CreateCompatibleBitmap + GetDIBits：
//    后者要把设备相关位图转成 DIB，实测 1920x1080 一次要 25~40ms；
//    前者让 BitBlt **直接写进我们自己的内存**，省掉整个转换，快一个数量级。
//    这个差别直接决定采样率能开到多少。

#include <QImage>
#include <QRect>
#include <QString>

namespace cm {

class ScreenCapture {
public:
    /// 抓屏幕上的一块矩形，返回 RGB32 的 QImage。失败返回空 QImage。
    static QImage grab(const QRect &screenRect);

    /// 抓整个虚拟桌面（多屏也能覆盖）。
    static QImage grabFullScreen();

    /// 全局最后错误信息，抓取失败时用来诊断。
    static QString lastError();

    /// 释放内部复用的 GDI 资源。正常退出时调用即可（不调也无妨）。
    static void release();

private:
    /// 按需要建立/重建可复用的内存 DC 与 DIB 段
    static bool ensureBuffers(int w, int h);

    static QString s_lastError;

    // 复用的 GDI 资源。抓屏只在采集线程里发生，不需要加锁。
    static void *s_memDC;      // HDC
    static void *s_bitmap;     // HBITMAP
    static void *s_oldBitmap;  // HGDIOBJ
    static void *s_bits;       // 直接可写的像素内存
    static int s_w;
    static int s_h;
};

} // namespace cm
