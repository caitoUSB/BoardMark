#pragma once

// AI 用量账本：把**真正花掉**的 token 记下来，供设置界面显示。
//
// 为什么只记账、不显示"套餐还剩多少"
// ----------------------------------
// OpenAI 兼容的 /chat/completions **只返回本次的 usage，拿不到账户余额**。
// 要查余额得走各家自己的账单 API（阿里云那边还需要另一套 AccessKey 和
// 请求签名），换平台就完全是另一套 —— 那会破坏本项目
// 「换模型只改配置、不动代码」的设计。所以这里只做一件能确定的事：如实记账。
//
// 两条必须守住的规则（改这个文件前先读）
// ------------------------------------
//   1. **缓存命中的图不记账。** 它根本没发请求，不产生 token。
//      记了就是把"没花钱"算成"花了钱"，账本立刻失去意义。
//   2. **token 一旦花掉就必须记上。** 所以记账发生在模型调用之后、
//      归档之前 —— 归档失败不该把已经花掉的额度从账本里抹掉。
//
// 账本文件是 exe 同目录的 usage.json，按月分桶、月内含每天的小计，
// 这样"本月"和"今日"都能算出来。只保留最近 24 个月，免得用几年后无限变大。
//
// **它只统计本机本程序的调用。** 同一个 API Key 在别的软件里用掉的量
// 这里看不到，所以它是参考值，不是账单 —— 界面上要如实这么写。

#include <QString>

namespace cm {

/// 一段时间内的用量
struct UsageBucket {
    qint64 promptTokens = 0;
    qint64 completionTokens = 0;
    int frames = 0;     ///< 实际送进模型的图片张数
    /// HTTP 请求次数。目前一张图一次调用，所以它和 frames 相等；
    /// 单独记是为了将来做"一次送多张"时不会算错。
    int calls = 0;

    qint64 total() const { return promptTokens + completionTokens; }
    bool isEmpty() const { return frames == 0 && calls == 0 && total() == 0; }
    void add(const UsageBucket &other);
};

/// 本月（自然月）+ 今日
struct UsageSummary {
    UsageBucket month;
    UsageBucket today;
    QString monthKey;       ///< 例如 "2026-10"
    QString dayKey;         ///< 例如 "2026-10-06"

    bool monthEmpty() const { return month.isEmpty(); }
};

/// 记一次用量。**每次整理会话调用一次即可**（传本轮的合计），
/// 内部会累加到对应月份和日期上并原子写盘。
/// 传空的 delta 会直接返回，不会产生没必要的写盘。
void recordUsage(const UsageBucket &delta);

/// 读本月 + 今日。文件不存在或坏了都返回全 0，不抛错。
UsageSummary usageSummary();

/// 账本文件路径（exe 同目录 usage.json）
QString usageFilePath();

} // namespace cm
