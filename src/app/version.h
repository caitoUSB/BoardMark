#pragma once

// 版本号。真实数值来自 CMakeLists.txt 的 project(... VERSION x.y.z)，
// 由编译期宏 CM_VERSION 注入 —— 改版本号只改 CMakeLists.txt 那一处，
// 不要在这里再写一遍，否则两边迟早对不上。
//
// 这里的 #ifndef 是为了让 IDE 单独解析这个文件时不报错。

#ifndef CM_VERSION
#  define CM_VERSION "0.0.0-dev"
#endif

namespace cm {

/// 例如 "0.9.0"
inline const char *versionString()
{
    return CM_VERSION;
}

/// 例如 "BoardMark 0.9.0"
inline const char *versionBanner()
{
    return "BoardMark " CM_VERSION;
}

} // namespace cm
