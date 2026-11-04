#pragma once

// 用视觉模型判断：这是哪个学科、讲的是什么知识点。
//
// 为什么这两件事能做，而「逐字转写」不做
// ----------------------------------------
//   * 转写要求每个字都对 —— 字迹潦草就崩
//   * **分类只要认出 1~2 个关键词** —— 再潦草的字，标题总是相对清楚的
//   * 而且模型还能用版式辅助判断：满屏公式偏理科，大段文字偏文科
//
// 所以这里是「分类」而不是「识别」，风险低一个量级。
//
// 接口刻意做成 OpenAI 兼容的 /chat/completions —— 通义千问、智谱、豆包、
// OpenAI，甚至本地部署的 Ollama / LM Studio 都能直接接，
// **换模型只改配置，不动业务代码**。
//
// 没有配置 key 时退化成 StubClassifier：把所有东西都判成「其他 / 未识别」。
// 这是**正确的保守行为** —— 与其瞎猜，不如让老师事后一眼扫过。
// 有了它整条流程（分类 → 归档）在没有 key 的情况下也能跑通。

#include <QString>
#include <QStringList>

#include <functional>

class QJsonObject;

namespace cm {

/// 可选的学科。让模型在这几个里面选，而不是自由发挥 ——
/// 自由发挥会让「物理」和「物理学」这种结果散开，没法建文件夹。
extern const QStringList kSubjects;

/// 判断结果
struct Classification {
    QString subject = QStringLiteral("其他");
    QString subjectConfidence = QStringLiteral("low");   ///< high / medium / low
    QStringList subjectKeywords;                          ///< 供事后核对判断依据
    QString topic = QStringLiteral("未识别");
    QString topicConfidence = QStringLiteral("low");
    QString raw;        ///< 模型原始回复，排查用
    QString error;      ///< 失败原因；空表示成功
    /// **根本性失败** —— Key 无效、余额/免费额度用完、接口地址或模型名错、
    /// 模型根本不能看图。这几类换哪张图、重试几次结果都一样。
    ///
    /// 调用方看到它就应当**立刻停下整批**，而不是把剩下的图一张张试过去
    /// （实测每张白等 6 秒退避），更不能把会话标记成「已整理」——
    /// 否则老师充值之后那节课永远是「未识别」，再也不会被自动重试。
    bool fatal = false;
    /// 用户中途取消了，这张图没判完
    bool cancelled = false;
    /// **请求是否真的发出去、并且拿到了 HTTP 响应。**
    ///
    /// 只有它为 true，这次调用才可能产生了费用，才该计进用量账本。
    /// 下面这几种情况都是 false，一分钱没花：
    ///   * 没配 API Key（走 Stub，直接返回）
    ///   * 图片读不出来（本地就失败了）
    ///   * 请求超时 / 网络层就没通
    ///   * 被服务端拒绝（余额不足、限流）—— 根本没进模型
    /// 不区分的话，一台没配 Key 的机器会攒出成千上万张"图片"的假账。
    bool requestSent = false;
    /// 本次调用消耗的 token（服务端返回的 usage）。用来估算免费额度还够多久。
    int promptTokens = 0;
    int completionTokens = 0;

    bool ok() const { return error.isEmpty(); }
    bool confident() const { return topicConfidence == QLatin1String("high"); }
};

// 关于「这节是不是习题课」——**刻意不做，不要再加回来**
// ------------------------------------------------------------------
// 曾经试过让模型判断习题课 / 新授课，并在文件名里加"习题课"后缀。
// 实测（78 张真实板书）**不可用**：
//   * 24 个「日期+学科」组里，19 组被判成习题课 —— 全都标记 = 没有标记
//   * 收紧判据、要求"必须有多道独立题目"之后，仍然是 8 个多页组全判习题课
//   * 知识点集中度从 0.09 到 0.62 跨度极大，AI 判定跟它完全无关
//
// 根因和信息量有关，不是提示词能修的：
//   **习题课和新授课的板面看起来几乎一样，都是公式加推导。**
//   真正的区别在老师说了什么、这是本单元第几节课 —— 这些不在像素里。
//   和「知识点往往判不出来」是同一个原因。
//
// 结论：这个维度天然不可观测，硬标只会制造噪音。宁可不标。

/// 接口地址与凭据
struct AiConfig {
    QString baseUrl = QStringLiteral("https://dashscope.aliyuncs.com/compatible-mode/v1");
    QString apiKey;
    QString model = QStringLiteral("qwen-vl-max");
    int timeoutMs = 120000;
    int maxRetries = 2;
    /// 送进去前把图缩到多宽。判断学科和知识点不需要原始分辨率，
    /// 缩放能显著省 token 和时间。原图不受影响。
    int maxImageWidth = 1280;
    int jpegQuality = 85;

    /// 是否发送 temperature 参数。
    /// 有些"深度思考"模型不接受 temperature，会直接返回 400。
    /// 实测遇到就会自动关掉重试一次（见 classify 里的处理）。
    bool sendTemperature = true;

    bool usable() const { return !apiKey.trimmed().isEmpty(); }
};

class Classifier {
public:
    explicit Classifier(const AiConfig &cfg);
    ~Classifier();

    /// 给一张板书截图判学科和知识点。
    /// 失败不抛异常 —— 返回的 Classification.error 里写着原因。
    ///
    /// 调用方要区分两种情况（都在返回值里）：
    ///   * `fatal`     —— 根本性问题，**立刻停下整批**，别把剩下的图试过去
    ///   * `cancelled` —— 用户取消了
    /// 只有既没 fatal 也没 cancelled 的失败，才是"这张图自己的问题"，
    /// 照常归档（落到「其他」）就行，不会因为一张图失败就中断整批。
    ///
    /// isCancelled 可以为空。传进来的话，会在重试之间检查，
    /// 取消时立即返回（不用等退避睡完）。
    Classification classify(
        const QString &imagePath,
        const std::function<bool()> &isCancelled = {}) const;

    bool usingStub() const { return !m_cfg.usable(); }
    QString describe() const;

    /// 连通性自检：把一张图发过去，报告模型到底能不能用、能不能看图。
    ///
    /// 这个命令很有必要 —— 模型名选错（用成纯文本模型）、key 没开通视觉权限、
    /// 接口地址填错，这几种情况的报错都很含糊，直接看原始回复最快。
    /// 返回一段人能看懂的报告。
    QString selfTest(const QString &imagePath) const;

private:
    /// 发一次请求，返回 HTTP 状态码与响应正文
    bool post(const QByteArray &body, int *status, QByteArray *raw,
              QString *error) const;

    AiConfig m_cfg;
};

/// 从设置里读 AI 配置
AiConfig loadAiConfig();

/// 把模型回复里裹着的 JSON 抠出来。
/// 模型有时会用 markdown 代码块包一层，或者多讲两句废话 —— 这很常见，必须容错。
/// 抠不出来时返回 false，并把原文放进 raw。
bool extractJsonObject(const QString &text, QJsonObject *out);

/// 把图片读进来、降采样、转成 data URL
QString imageToDataUrl(const QString &path, int maxWidth, int jpegQuality);

} // namespace cm
