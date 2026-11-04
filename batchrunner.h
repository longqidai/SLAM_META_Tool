#ifndef BATCHRUNNER_H
#define BATCHRUNNER_H

#include <QObject>
#include <QProcess>
#include <QTimer>
#include <QVector>

#include "tasktypes.h"

class QFile;

class BatchRunner : public QObject
{
    Q_OBJECT
public:
    explicit BatchRunner(QObject *parent = nullptr);
    ~BatchRunner() override;

    bool isRunning() const;
    QString batchId() const;
    void start(const QVector<BatchDevice> &devices, const QString &batchId,
               const QString &dataRoot, BatchMode mode);

public slots:
    void stop();

signals:
    // 环境指示灯、单台状态、实时日志、单台最终结果和整批最终结果
    // 分别由 MainWindow/BatchProgressDialog 接收；BatchRunner 不直接操作界面。
    void environmentChanged(bool dockerReady, bool containerReady, const QString &message);
    void deviceStateChanged(int index, const QString &stage,
                            const QString &parseStatus, const QString &validateStatus,
                            const QString &detail);
    void logMessage(int index, const QString &line);
    void deviceFinished(int index, const DeviceRunResult &result);
    void fatalError(const QString &message);
    void batchFinished(bool cancelled);
    void runningChanged(bool running);

private slots:
    void readControlOutput();
    void controlFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void controlError(QProcess::ProcessError error);
    void controlTimedOut();

private:
    enum class ControlStep {
        // m_controlProcess 当前正在执行的串行步骤。环境检查按前四个状态推进，
        // Validating 表示正在验证 m_validationIndex 对应设备。
        Idle,
        DockerVersion,
        InspectContainer,
        StartContainer,
        CheckContainerTools,
        Validating
    };

    struct ParseContext {
        // 每台解析的独立运行上下文。与共用的 m_controlProcess 不同，
        // 多个ParseContext的process在收到CPU释放标记后可以重叠运行。
        QProcess *process = nullptr;
        QTimer *timer = nullptr;
        QFile *logFile = nullptr;
        QByteArray output;
        QByteArray lineBuffer;
        qint64 startedMs = 0;
        // 先进入0-th批优化；只有随后识别到Ceres迭代号1的数据行才释放CPU标记。
        bool releaseTriggerLineSeen = false;
        bool releaseSeen = false;
        bool completed = false;
        bool timedOut = false;
        bool resultEmitted = false;
    };

    void initializeResults();
    void checkDocker();
    void runControl(ControlStep step, const QString &program,
                    const QStringList &arguments, int timeoutMs);

    void startNextParser();
    void launchParser(int index);
    void readParserOutput(int index);
    void handleParserOutputLine(int index, const QString &line);
    void parserFinished(int index, int exitCode, QProcess::ExitStatus exitStatus);
    void parserError(int index, QProcess::ProcessError error);
    void parserTimedOut(int index);
    void parserReleaseSeen(int index, bool fallback);
    void failParse(int index, const QString &reason, int exitCode = -1);
    void completeParse(int index, int exitCode);
    void maybeStartValidationPhase();
    void copyParsedHardwareFile(int index);

    void beginNextValidation();
    void beginValidation(int index);
    void failValidation(int index, const QString &reason, int exitCode = -1);
    void completeValidation(int index, int exitCode);
    void finishDevice(int index);
    void finishBatch(bool cancelled);
    void failBatchEnvironment(const QString &reason);

    void appendLog(int index, const QString &line, bool powerShellOutput = false);
    void openLog(int index);
    void closeLogs();
    QString containerMetaPath(const BatchDevice &device) const;
    bool validJsonFile(const QString &path, int *warningCount = nullptr,
                       QString *errorMessage = nullptr) const;
    bool outputHasFatalError(const QByteArray &output) const;
    void stopContainerProgram(const QString &pattern);

    QVector<BatchDevice> m_devices;
    QVector<DeviceRunResult> m_results;
    QVector<ParseContext *> m_parseContexts;
    QString m_batchId;
    QString m_dataRoot;
    QString m_releaseMarker;
    BatchMode m_mode = BatchMode::ParseAndValidate;

    bool m_running = false;
    bool m_stopping = false;
    int m_nextParseIndex = 0;
    // 达到三路并行上限时保留一次启动请求；任一解析退出后再补启下一台。
    bool m_parserStartPending = false;
    // 已完整退出并判定成功/失败的解析数量，用于确认整批能否最终结束。
    int m_finishedParseCount = 0;
    // 验证严格按该索引顺序单路推进；遇到尚未解析完的设备会等待。
    int m_validationIndex = -1;
    bool m_validationPhaseStarted = false;

    ControlStep m_controlStep = ControlStep::Idle;
    QProcess m_controlProcess;
    QTimer m_controlTimer;
    QByteArray m_controlOutput;
    bool m_controlErrorHandled = false;
    bool m_controlTimedOut = false;
};

#endif
