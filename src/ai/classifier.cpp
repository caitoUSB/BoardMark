#include "ai/classifier.h"

#include "app/settings.h"

#include <QBuffer>
#include <QEventLoop>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QThread>
#include <QTimer>
#include <QUrl>

namespace cm {

const QStringList kSubjects = {
    QStringLiteral("语文"), QStringLiteral("数学"), QStringLiteral("英语"),
    QStringLiteral("物理"), QStringLiteral("化学"), QStringLiteral("生物"),
    QStringLiteral("政治"), QStringLiteral("历史"), QStringLiteral("地理"),
    QStringLiteral("信息技术"), QStringLiteral("体育"), QStringLiteral("音乐"),
    QStringLiteral("美术"), QStringLiteral("其他"),
};

// 四条提示词规则，每一条都是踩过坑才加的：
//   1. 一次只问一件事 —— 学科和知识点分开问，不要混在一句话里
//   2. 固定 JSON 输出 —— 否则没法解析
//   3. **必须允许"不确定"** —— 不给出口，模型就会硬编一个看起来合理的答案
//   4. 要求写出判断依据 —— 老师能核对，我们也能查错
static const char *kSystemPrompt = R"(你是一位中学教学助理，正在整理一间教室白板上留下的板书。

【背景，请务必先读】
老师常常只在白板上写解题过程、不写题目；只写几个关键词；甚至写一些
脱离上下文完全看不出含义的内容。
要判断"这是哪个知识点"，往往需要知道老师当时说了什么、课本讲到哪一页、
上一道题是什么 —— 这些你都没有。

所以请分清主次：
  * **判断学科是你的主要任务。**
  * 给出知识点标题只是锦上添花；**判断不了就老实说判断不了，
    "未识别"是完全正常的答案，不是失败。**
  * 宁可让老师事后补一个标题，也不要给他一个错误的知识点。

【一】这是哪个学科？
只能从以下选项里选一个：
语文 / 数学 / 英语 / 物理 / 化学 / 生物 / 政治 / 历史 / 地理 / 信息技术 / 体育 / 音乐 / 美术 / 其他

判断依据：出现的关键词、公式符号、文字语种、版面特征。
即使字迹潦草，标题和关键词通常仍然清楚，请重点看它们。
**注意这几种容易混的情况**：
  * 语文 / 历史 / 政治 都是满屏中文 —— 看有没有古文、年代人名、或政治术语
  * 数学 / 物理 都有公式 —— 物理通常带单位（m/s、N、J）和物理量符号
  * 化学有化学式和反应箭头；生物多是结构图和名称
  * 只有一串数字、没有公式也没有文字时，倾向判"数学"，但置信度给 low

如果确实没有线索，选"其他"，置信度标 low。**绝对不要猜。**

【二】板书上有没有明确写出的标题或概念名？
  * 如果老师**明确写了**一个标题或概念名（例如板书顶部写着"牛顿第二定律"），
    直接用它，**4~10 个字**。
  * 可以从**完整的公式或方程**推断（例如看到 x²-3x+2=0 可以概括成"一元二次方程"），
    同样控制在 10 个字以内。
  * 但下面这些情况**必须填"未识别"、置信度填 low**：
      - 只有零散的数字、符号、草稿计算，没有标题也没有完整公式
      - 只有解题步骤、没有题目（这是习题课最常见的情况）
      - 只是随意涂画、或看不出结构的线条
      - **看到一串数字就推测成"排列组合""数列""找规律"这类具体知识点**
        —— 一串数字只是笔迹，不是知识点。这是最容易犯的错误。

【三】只输出 JSON，不要输出任何其他文字、不要用 markdown 代码块包裹：
{
  "subject": "物理",
  "subject_confidence": "high|medium|low",
  "subject_keywords": ["关键词1", "关键词2"],
  "topic": "牛顿第二定律",
  "topic_confidence": "high|medium|low"
})";

// ------------------------------------------------------------------ 工具

QString imageToDataUrl(const QString &path, int maxWidth, int jpegQuality)
{
    QImage img(path);
    if (img.isNull()) {
        return QString();
    }
    if (maxWidth > 0 && img.width() > maxWidth) {
        img = img.scaledToWidth(maxWidth, Qt::SmoothTransformation);
    }

    QByteArray bytes;
    QBuffer buf(&bytes);
    buf.open(QIODevice::WriteOnly);
    // JPEG 不支持透明，先转成 RGB 格式，否则可能存出一张全黑的图
    if (!img.convertToFormat(QImage::Format_RGB32).save(&buf, "JPEG", jpegQuality)) {
        return QString();
    }
    return QStringLiteral("data:image/jpeg;base64,")
           + QString::fromLatin1(bytes.toBase64());
}

bool extractJsonObject(const QString &text, QJsonObject *out)
{
    QString t = text.trimmed();

    // 模型经常裹一层 markdown 代码块，先剥掉
    if (t.startsWith(QLatin1String("```"))) {
        const int first = t.indexOf(QLatin1Char('\n'));
        const int last = t.lastIndexOf(QLatin1String("```"));
        if (first > 0 && last > first) {
            t = t.mid(first + 1, last - first - 1).trimmed();
        }
    }
    // 再退化到"取第一个 { 到最后一个 }"
    const int start = t.indexOf(QLatin1Char('{'));
    const int end = t.lastIndexOf(QLatin1Char('}'));
    if (start < 0 || end <= start) {
        return false;
    }

    QJsonParseError err{};
    const QJsonDocument doc =
        QJsonDocument::fromJson(t.mid(start, end - start + 1).toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        return false;
    }
    *out = doc.object();
    return true;
}

// ------------------------------------------------------------------ 构造

Classifier::Classifier(const AiConfig &cfg) : m_cfg(cfg) {}

Classifier::~Classifier() = default;

QString Classifier::describe() const
{
    if (usingStub()) {
        return QStringLiteral("未配置视觉模型（所有内容会归到「其他」）");
    }
    return QStringLiteral("%1 @ %2").arg(m_cfg.model, m_cfg.baseUrl);
}

// ------------------------------------------------------------------ 调用

namespace {

/// 一次失败的定性
struct HttpErrorInfo {
    QString message;
    /// true = **根本性问题**：Key 无效、余额/额度用完、地址或模型名错、
    /// 模型根本不能看图。这几类换哪张图、重试几次结果都一样，
    /// 调用方应当立刻停下整批（见 Classification::fatal）。
    bool fatal = false;
};

bool containsAny(const QString &low, std::initializer_list<const char *> keys)
{
    for (const char *k : keys) {
        if (low.contains(QLatin1String(k))) {
            return true;
        }
    }
    return false;
}

/// 把服务端的报错翻译成"人话 + 该怎么办"。
/// 这几类错误在网上查半天，不如直接告诉老师。
HttpErrorInfo explainHttpError(int status, const QByteArray &raw)
{
    HttpErrorInfo info;
    const QString body = QString::fromUtf8(raw);
    const QString low = body.toLower();

    // ---- 余额/额度用完：**必须放在最前面** ----
    //
    // 百炼欠费可能报 400，也可能报 403。如果先走下面那条 403 分支，
    // 就会显示「API Key 无效」，把老师带去查 key —— 其实该去充值。
    // 实测这个误导很难自己看出来，所以顺序不能动。
    //
    // 刻意**不收裸的 "quota"**：太泛，"Requests quota exceeded" 这种
    // 限流也会命中，就会把限流误报成"余额用完"。
    if (containsAny(low, {"arrearage", "insufficient", "balance",
                          "good standing", "freetieronly", "allocationquota",
                          "billing"})) {
        info.message =
            QStringLiteral("账户余额或免费额度用完了。\n"
                           "去服务商控制台充值，或换一个有额度的 API Key。\n"
                           "（已经判好的图会留在缓存里，充值后再点一次"
                           "「AI 整理记录」就从断点继续。）\n原始返回：%1")
                .arg(body.left(300));
        info.fatal = true;
        return info;
    }

    if (status == 401 || status == 403) {
        info.message = QStringLiteral(
                           "API Key 无效，或者这个 key 还没开通模型服务。\n"
                           "去百炼控制台确认：设置 → API-Key，"
                           "以及「模型服务」是否显示已开通。\n原始返回：%1")
                           .arg(body.left(300));
        info.fatal = true;      // 换哪张图都一样
        return info;
    }
    if (status == 404) {
        info.message = QStringLiteral(
                           "接口地址或模型名不对（404）。\n"
                           "接口地址应以 /compatible-mode/v1 结尾，"
                           "不要自己在后面加 /chat/completions。\n原始返回：%1")
                           .arg(body.left(300));
        info.fatal = true;
        return info;
    }
    if (status == 429) {
        // 限流是暂时的，可以重试 —— 所以**不**算根本性错误
        info.message = QStringLiteral("请求太频繁（429），稍后会自动重试。\n"
                                      "如果反复出现，可能是免费额度用完了。\n"
                                      "原始返回：%1")
                           .arg(body.left(300));
        return info;
    }
    // 这是最容易踩的一个：模型名填成了纯文本模型
    if (low.contains(QStringLiteral("image"))
        || low.contains(QStringLiteral("vision"))
        || low.contains(QStringLiteral("multimodal"))
        || low.contains(QStringLiteral("content type"))) {
        info.message = QStringLiteral(
                           "**这个模型不能看图。**\n"
                           "你填的模型多半是纯文本模型 —— 判断学科需要视觉模型，"
                           "模型名里通常带 vl（例如 qwen-vl-max）。\n"
                           "原始返回：%1")
                           .arg(body.left(300));
        info.fatal = true;      // 每张图都会被拒
        return info;
    }

    info.message = QStringLiteral("HTTP %1：%2").arg(status).arg(body.left(300));
    return info;
}

} // namespace

bool Classifier::post(const QByteArray &body, int *status, QByteArray *raw,
                      QString *error) const
{
    QNetworkAccessManager nam;
    QNetworkRequest req{QUrl(m_cfg.baseUrl + QStringLiteral("/chat/completions"))};
    req.setHeader(QNetworkRequest::ContentTypeHeader,
                  QStringLiteral("application/json"));
    req.setRawHeader("Authorization",
                     QByteArrayLiteral("Bearer ") + m_cfg.apiKey.toUtf8());

    // **强制 HTTP/1.1。**
    // Qt 默认在服务器支持时协商 HTTP/2，而百炼的网关跟 Qt 的 HTTP/2 实现
    // 合不来：请求会以 "Received GOAWAY / Server is unable to maintain the
    // header compression context" 结束，且拿不到任何 HTTP 状态码。
    // 症状是"HTTP 0"加一串看不懂的报错 —— 实测踩到过。
    req.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);

    QNetworkReply *reply = nam.post(req, body);

    // 同步等待。这个类只在工作线程里用，阻塞是刻意的 ——
    // 用事件循环而不是 waitForReadyRead，是为了让超时和信号都能正常工作。
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timer.start(m_cfg.timeoutMs);
    loop.exec();

    if (!reply->isFinished()) {
        reply->abort();
        reply->deleteLater();
        *error = QStringLiteral("请求超时（%1 秒）。网络慢或模型响应太慢，"
                                "可以换一个更快的模型。")
                     .arg(m_cfg.timeoutMs / 1000);
        return false;
    }

    *status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    *raw = reply->readAll();
    // 网络层失败时状态码会是 0，原因只在 errorString 里 ——
    // 不把它带出来，用户就只能看到一句"HTTP 0"，完全无从下手。
    if (*status == 0 && reply->error() != QNetworkReply::NoError) {
        *error = QStringLiteral("%1（网络层失败，没拿到 HTTP 状态码。"
                                "如果反复出现，多半是公司网络或代理拦了）")
                     .arg(reply->errorString());
    }
    reply->deleteLater();
    return true;
}

Classification Classifier::classify(
    const QString &imagePath,
    const std::function<bool()> &isCancelled) const
{
    Classification c;

    if (usingStub()) {
        c.error = QStringLiteral("未配置视觉模型");
        return c;
    }

    const QString dataUrl =
        imageToDataUrl(imagePath, m_cfg.maxImageWidth, m_cfg.jpegQuality);
    if (dataUrl.isEmpty()) {
        c.error = QStringLiteral("图片读取失败：%1").arg(imagePath);
        return c;
    }

    // 一次只送一张图。
    // 早先的 Python 版试过一次送一批，但那样一张图判断失败会拖累整批，
    // 而且模型容易把不同页的内容混在一起谈。一张一判，失败只影响一张。
    QJsonArray content;
    {
        QJsonObject text;
        text[QStringLiteral("type")] = QStringLiteral("text");
        text[QStringLiteral("text")] = QStringLiteral(
            "下面是一张课堂板书截图。请按系统提示的要求输出 JSON。");
        content.append(text);

        QJsonObject img;
        img[QStringLiteral("type")] = QStringLiteral("image_url");
        QJsonObject url;
        url[QStringLiteral("url")] = dataUrl;
        img[QStringLiteral("image_url")] = url;
        content.append(img);
    }

    QJsonArray messages;
    {
        QJsonObject sys;
        sys[QStringLiteral("role")] = QStringLiteral("system");
        sys[QStringLiteral("content")] = QString::fromUtf8(kSystemPrompt);
        messages.append(sys);

        QJsonObject usr;
        usr[QStringLiteral("role")] = QStringLiteral("user");
        usr[QStringLiteral("content")] = content;
        messages.append(usr);
    }

    const auto buildBody = [&](bool withTemperature) {
        QJsonObject payload;
        payload[QStringLiteral("model")] = m_cfg.model;
        // 温度设 0：同一个输入应该得到同一个输出，否则出了问题没法复现。
        // 但有些"深度思考"模型不接受这个参数，会直接 400 —— 那种情况会自动去掉重试。
        if (withTemperature) {
            payload[QStringLiteral("temperature")] = 0.0;
        }
        payload[QStringLiteral("messages")] = messages;
        return QJsonDocument(payload).toJson(QJsonDocument::Compact);
    };

    bool withTemperature = m_cfg.sendTemperature;
    QString lastError;
    bool lastFatal = false;

    for (int attempt = 0; attempt <= m_cfg.maxRetries; ++attempt) {
        // 用户取消：立刻返回，不把剩下的重试走完
        if (isCancelled && isCancelled()) {
            c.cancelled = true;
            c.error = QStringLiteral("已取消");
            return c;
        }

        if (attempt > 0) {
            // 退避重试：限流和偶发网络问题都能靠它扛过去。
            // 分片睡，这样取消请求能及时生效，而不是硬等 2~4 秒。
            for (int slept = 0; slept < 2000 * attempt; slept += 100) {
                if (isCancelled && isCancelled()) {
                    c.cancelled = true;
                    c.error = QStringLiteral("已取消");
                    return c;
                }
                QThread::msleep(100);
            }
        }

        int status = 0;
        QByteArray raw;
        QString netError;
        if (!post(buildBody(withTemperature), &status, &raw, &netError)) {
            lastError = netError;
            continue;
        }

        if (status < 200 || status >= 300) {
            if (status == 0) {
                // 网络层就没通，报网络原因，别报 HTTP
                lastError = netError.isEmpty()
                                ? QStringLiteral("请求没发出去，也拿不到原因。")
                                : netError;
                continue;
            }
            const QString body = QString::fromUtf8(raw);
            // 自愈：模型不认 temperature 就去掉它重试一次。
            // 深度思考类模型常见，与其让用户去猜，不如程序自己让开。
            if (withTemperature && status == 400
                && body.contains(QLatin1String("temperature"),
                                 Qt::CaseInsensitive)) {
                withTemperature = false;
                --attempt;      // 这次不算重试次数
                continue;
            }
            const HttpErrorInfo info = explainHttpError(status, raw);
            lastError = info.message;
            lastFatal = info.fatal;
            if (info.fatal) {
                // 根本性问题：再重试几次也是白等 6 秒，直接上报让调用方停批
                break;
            }
            continue;
        }

        QJsonParseError perr{};
        const QJsonDocument doc = QJsonDocument::fromJson(raw, &perr);
        if (perr.error != QJsonParseError::NoError) {
            lastError = QStringLiteral("响应不是合法 JSON：%1")
                            .arg(QString::fromUtf8(raw.left(200)));
            continue;
        }

        // 走到这里说明服务端**真的处理了这次请求**（2xx + 合法 JSON），
        // 才可能产生费用。用量账本只认这一种。见 Classifier::requestSent。
        c.requestSent = true;

        const QJsonArray choices =
            doc.object().value(QStringLiteral("choices")).toArray();
        if (choices.isEmpty()) {
            lastError = QStringLiteral("响应里没有 choices：%1")
                            .arg(QString::fromUtf8(raw.left(300)));
            continue;
        }

        const QJsonObject msg = choices.first().toObject()
                                    .value(QStringLiteral("message")).toObject();
        QString text = msg.value(QStringLiteral("content")).toString();
        // 深度思考模型可能把正文放在 reasoning_content 里，或者开了思考模式
        // 导致 content 为空 —— 那就退回去用思考内容，总比整个判失败强。
        if (text.trimmed().isEmpty()) {
            text = msg.value(QStringLiteral("reasoning_content")).toString();
        }
        c.raw = text;

        if (text.trimmed().isEmpty()) {
            lastError = QStringLiteral(
                "模型返回了空内容。多半是这个模型不支持看图，"
                "或者思考模式把 token 用光了。");
            continue;
        }

        QJsonObject obj;
        if (!extractJsonObject(text, &obj)) {
            lastError = QStringLiteral("回复里没有 JSON：%1").arg(text.left(200));
            continue;
        }

        QString subject = obj.value(QStringLiteral("subject")).toString().trimmed();
        // 模型可能返回一个不在候选里的学科（例如"物理学"）——
        // 直接落到"其他"，否则会建出一堆稀奇古怪的文件夹
        if (!kSubjects.contains(subject)) {
            subject = QStringLiteral("其他");
        }
        c.subject = subject;
        c.subjectConfidence =
            obj.value(QStringLiteral("subject_confidence")).toString(
                QStringLiteral("low"));
        for (const QJsonValue &v :
             obj.value(QStringLiteral("subject_keywords")).toArray()) {
            c.subjectKeywords << v.toString();
        }
        c.topic = obj.value(QStringLiteral("topic")).toString().trimmed();
        if (c.topic.isEmpty()) {
            c.topic = QStringLiteral("未识别");
        }
        c.topicConfidence =
            obj.value(QStringLiteral("topic_confidence")).toString(
                QStringLiteral("low"));

        // token 用量：用来告诉老师免费额度还能撑多少节课
        const QJsonObject usage =
            doc.object().value(QStringLiteral("usage")).toObject();
        c.promptTokens = usage.value(QStringLiteral("prompt_tokens")).toInt();
        c.completionTokens =
            usage.value(QStringLiteral("completion_tokens")).toInt();

        c.error.clear();
        return c;
    }

    c.error = lastError;
    c.fatal = lastFatal;
    return c;
}

QString Classifier::selfTest(const QString &imagePath) const
{
    QStringList out;
    out << QStringLiteral("接口地址：%1").arg(m_cfg.baseUrl);
    out << QStringLiteral("模型名  ：%1").arg(m_cfg.model);
    out << QStringLiteral("API Key ：%1")
               .arg(m_cfg.apiKey.isEmpty()
                        ? QStringLiteral("（空）")
                        : QStringLiteral("已填写（%1 位）").arg(m_cfg.apiKey.size()));
    out << QString();

    if (usingStub()) {
        out << QStringLiteral("没有填 API Key，无法自检。");
        return out.join(QLatin1Char('\n'));
    }

    // 先检查一眼模型名 —— 这是最常见的错
    if (!m_cfg.model.contains(QLatin1String("vl"), Qt::CaseInsensitive)
        && !m_cfg.model.contains(QLatin1String("vision"), Qt::CaseInsensitive)) {
        out << QStringLiteral("⚠ 模型名里没有 vl / vision。"
                              "如果它是纯文本模型，看不懂截图 —— 这正是最常见的错误。");
        out << QString();
    }

    const Classification c = classify(imagePath);
    if (c.ok()) {
        out << QStringLiteral("✓ 调用成功。模型能看图，返回了合法 JSON。");
        out << QStringLiteral("  学科：%1（置信度 %2）")
                   .arg(c.subject, c.subjectConfidence);
        out << QStringLiteral("  知识点：%1（置信度 %2）")
                   .arg(c.topic, c.topicConfidence);
        if (!c.subjectKeywords.isEmpty()) {
            out << QStringLiteral("  判断依据：%1").arg(c.subjectKeywords.join(
                QStringLiteral("、")));
        }
        out << QString();
        out << QStringLiteral("模型原始回复：");
        out << c.raw.left(500);
    } else {
        out << QStringLiteral("✗ 调用失败：");
        out << c.error;
        if (!c.raw.isEmpty()) {
            out << QString();
            out << QStringLiteral("模型原始回复：");
            out << c.raw.left(500);
        }
    }
    return out.join(QLatin1Char('\n'));
}

AiConfig loadAiConfig()
{
    const Settings s = Settings::load();
    AiConfig cfg;
    cfg.baseUrl = s.aiBaseUrl;
    cfg.apiKey = s.aiApiKey;
    cfg.model = s.aiModel;
    return cfg;
}

} // namespace cm
