#ifndef TASKTYPES_H
#define TASKTYPES_H

#include <QString>

// 本批任务的执行范围。组合模式严格分成“全部解析”和“全部验证”两个阶段。
enum class BatchMode {
    ParseAndValidate,
    ParseOnly,
    ValidateOnly
};

// 用户点击“开始解析验证”时，从主界面当前勾选行生成的设备快照。
// 批次运行期间固定使用这些字段，不再依赖可能变化的主表内容。
struct BatchDevice
{
    QString sn;
    QString metaName;
    QString metaPath;
    QString deviceDir;
    qint64 metaSize = 0;
    qint64 metaModifiedMs = 0;
};

// 单台设备从解析开始到验证结束的完整结果。finishDevice() 通过
// deviceFinished 信号把它交给 MainWindow，再写入 SQLite 历史数据库。
struct DeviceRunResult
{
    QString batchId;
    QString sn;
    QString metaName;
    QString metaPath;
    qint64 metaSize = 0;
    qint64 metaModifiedMs = 0;
    QString startedAt;
    QString finishedAt;
    QString parseStatus;
    int parseExitCode = -1;
    QString parseError;
    QString validateStatus;
    int validateExitCode = -1;
    QString validateError;
    int warningCount = 0;
    QString hardwarePath;
    QString metricsPath;
    QString logPath;
};

#endif
