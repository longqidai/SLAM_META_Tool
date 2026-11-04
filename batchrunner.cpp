#include "batchrunner.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QTextStream>

namespace {
// 三类外部命令的最长允许时间：环境检查 30 秒、单台解析 2 小时、
// 单台验证 1 小时。超时后会终止 Windows 侧 QProcess，并尝试清理容器进程。
const int kEnvironmentTimeoutMs = 30000;
const int kParseTimeoutMs = 2 * 60 * 60 * 1000;
const int kValidateTimeoutMs = 60 * 60 * 1000;
// 即使多台设备先后到达CPU释放点，同时存活的解析进程也绝不超过三路。
const int kMaxConcurrentParsers = 3;

// 递归遍历 metrics.json，统计名称中包含 warning 的有效字段数量。
int warningCountInValue(const QJsonValue &value)
{
    int count = 0;
    if (value.isObject()) {
        const QJsonObject object = value.toObject();
        for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
            if (it.key().contains(QStringLiteral("warning"), Qt::CaseInsensitive)) {
                if (it.value().isArray())
                    count += it.value().toArray().size();
                else if (it.value().isString() && !it.value().toString().trimmed().isEmpty())
                    ++count;
                else if (it.value().isBool() && it.value().toBool())
                    ++count;
            }
            count += warningCountInValue(it.value());
        }
    } else if (value.isArray()) {
        for (const QJsonValue &child : value.toArray())
            count += warningCountInValue(child);
    }
    return count;
}

// 仅用于日志展示，把 QProcess 的程序名和参数还原成便于阅读的命令行。
QString displayCommand(const QString &program, const QStringList &arguments)
{
    QStringList escaped;
    for (const QString &argument : arguments)
        escaped.append(argument.contains(QLatin1Char(' '))
                           ? QStringLiteral("\"%1\"").arg(argument) : argument);
    return program + QLatin1Char(' ') + escaped.join(QLatin1Char(' '));
}

// 将 Docker Desktop 返回的 Linux 风格宿主机挂载路径转换为 Windows 路径，
// 供 InspectContainer 阶段与用户选择的数据根目录进行精确比较。
QString normalizedMountSource(QString source)
{
    source = QDir::fromNativeSeparators(source.trimmed());
    const QRegularExpression dockerDesktop(
        QStringLiteral("^/(?:run/desktop/mnt/host/|host_mnt/|mnt/)([A-Za-z])/(.*)$"));
    const QRegularExpressionMatch match = dockerDesktop.match(source);
    if (match.hasMatch())
        source = match.captured(1).toUpper() + QStringLiteral(":/") + match.captured(2);
    return QDir::cleanPath(source);
}

// 对传入 bash -ic 的单个参数进行单引号转义，防止空格或特殊字符拆坏命令。
QString shellQuote(QString value)
{
    value.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QLatin1Char('\'') + value + QLatin1Char('\'');
}
}

// 创建共用控制进程。环境检查和所有验证串行共用 m_controlProcess；
// 每台解析则在 launchParser() 中单独创建 QProcess，因此解析可形成流水线。
BatchRunner::BatchRunner(QObject *parent)
    : QObject(parent)
{
    m_controlProcess.setProcessChannelMode(QProcess::MergedChannels);
    connect(&m_controlProcess, &QProcess::readyReadStandardOutput,
            this, &BatchRunner::readControlOutput);
    connect(&m_controlProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &BatchRunner::controlFinished);
    connect(&m_controlProcess, &QProcess::errorOccurred,
            this, &BatchRunner::controlError);
    m_controlTimer.setSingleShot(true);
    connect(&m_controlTimer, &QTimer::timeout, this, &BatchRunner::controlTimedOut);
}

// 析构前终止仍在运行的控制/解析进程，关闭日志并释放 ParseContext。
BatchRunner::~BatchRunner()
{
    if (m_controlProcess.state() != QProcess::NotRunning) {
        m_controlProcess.kill();
        m_controlProcess.waitForFinished(2000);
    }
    for (ParseContext *context : m_parseContexts) {
        if (context->process && context->process->state() != QProcess::NotRunning) {
            context->process->kill();
            context->process->waitForFinished(2000);
        }
    }
    closeLogs();
    qDeleteAll(m_parseContexts);
}

// 供主窗口判断能否再启动一个批次。
bool BatchRunner::isRunning() const { return m_running; }
// 返回当前批次号，格式由 MainWindow 生成。
QString BatchRunner::batchId() const { return m_batchId; }

// 批次总入口。重置上批状态、初始化每台结果和日志，然后调用 checkDocker()。
// 环境检查通过后 controlFinished(CheckContainerTools) 才会启动第一台解析。
void BatchRunner::start(const QVector<BatchDevice> &devices, const QString &batchId,
                        const QString &dataRoot, BatchMode mode)
{
    // 1. 拒绝空批次，也不允许覆盖正在运行的批次。
    if (m_running || devices.isEmpty())
        return;

    // 2. 清理上批遗留的日志文件和 ParseContext。
    closeLogs();
    qDeleteAll(m_parseContexts);
    m_parseContexts.clear();
    // 3. 保存本批固定输入和调度标记。看到0-th批优化行后，还必须识别到
    // Ceres迭代号1的数据行才会放行下一台；触发文本仍可由环境变量覆盖。
    m_devices = devices;
    m_batchId = batchId;
    m_dataRoot = QDir::cleanPath(dataRoot);
    m_mode = mode;
    m_releaseMarker = qEnvironmentVariable("SLAM_CPU_RELEASE_MARKER",
        QStringLiteral("-- performing '0-th' batch optimization.."));
    // 4. 所有解析/验证游标和运行标志恢复到批次初始状态。
    m_nextParseIndex = 0;
    m_parserStartPending = false;
    m_finishedParseCount = 0;
    m_validationIndex = -1;
    m_validationPhaseStarted = false;
    m_running = true;
    m_stopping = false;
    // 5. 为每台设备创建结果、解析上下文和独立磁盘日志。
    initializeResults();

    // 仅验证不运行解析程序：把解析阶段记为“未执行”，验证直接使用目录中
    // 已有的 sinoslam_hardware.json。文件有效性会在 beginValidation() 检查。
    if (m_mode == BatchMode::ValidateOnly) {
        m_finishedParseCount = m_devices.size();
        for (int i = 0; i < m_parseContexts.size(); ++i) {
            m_parseContexts[i]->completed = true;
            m_results[i].parseStatus = QStringLiteral("not_run");
        }
    }

    // 6. 通知主界面进入运行状态，并写入本批调度规则。
    emit runningChanged(true);
    const QString modeText = m_mode == BatchMode::ParseOnly
        ? QStringLiteral("仅解析")
        : (m_mode == BatchMode::ValidateOnly
            ? QStringLiteral("仅验证") : QStringLiteral("解析并验证"));
    appendLog(-1, QStringLiteral("批次 %1 启动，共 %2 台设备，模式：%3。")
        .arg(batchId).arg(devices.size()).arg(modeText));
    if (m_mode != BatchMode::ValidateOnly) {
        appendLog(-1, QStringLiteral(
            "解析阶段先等待“%1”，再识别到该轮优化的迭代号1数据行后放行下一台；并行解析最多三台。单台解析完整成功后即可开始验证，后续设备解析可与当前验证并行。")
                      .arg(m_releaseMarker));
    }
    // 7. 从 DockerVersion 开始环境检查；检查完成前不会解析 META。
    checkDocker();
}

// 为每个 BatchDevice 建立 DeviceRunResult 和 ParseContext，并调用 openLog()。
// hardwarePath/metricsPath 都落在当前 SN 目录，依靠 /data 共享挂载供容器访问。
void BatchRunner::initializeResults()
{
    // 1. 结果数组和解析上下文数组与本批设备数量保持一一对应。
    m_results.clear();
    m_results.resize(m_devices.size());
    m_parseContexts.resize(m_devices.size());
    // 2. 逐台复制批次快照并生成本地输出路径。
    for (int i = 0; i < m_devices.size(); ++i) {
        // 2.1 每台解析使用独立 ParseContext/QProcess，允许解析流水线重叠。
        m_parseContexts[i] = new ParseContext;
        const BatchDevice &device = m_devices.at(i);
        DeviceRunResult &result = m_results[i];
        result.batchId = m_batchId;
        result.sn = device.sn;
        result.metaName = device.metaName;
        result.metaPath = device.metaPath;
        result.metaSize = device.metaSize;
        result.metaModifiedMs = device.metaModifiedMs;
        result.startedAt = QDateTime::currentDateTime().toString(Qt::ISODate);
        // 2.2 hardware 和 metrics 都位于 Windows SN 目录；容器通过 /data 访问。
        result.hardwarePath = QDir(device.deviceDir)
                                  .filePath(QStringLiteral("sinoslam_hardware.json"));
        result.metricsPath = QDir(device.deviceDir)
                                 .filePath(QStringLiteral("evaluate_output/metrics.json"));
        // 2.3 立即建立该设备本批日志，后续软件和 PowerShell 输出写入同一文件。
        openLog(i);
    }
}

// 环境状态机第一步：运行 docker version；完成后进入 controlFinished() 分派。
void BatchRunner::checkDocker()
{
    emit environmentChanged(false, false, QStringLiteral("正在检查 Docker..."));
    runControl(ControlStep::DockerVersion, QStringLiteral("docker"),
               {QStringLiteral("version"), QStringLiteral("--format"),
                QStringLiteral("{{.Server.Version}}")}, kEnvironmentTimeoutMs);
}

// 启动一条串行控制命令：保存步骤、清输出/错误标志、记录命令并启动超时器。
// DockerVersion/Inspect/Start/ToolsCheck 和 Validating 都通过该函数执行。
void BatchRunner::runControl(ControlStep step, const QString &program,
                             const QStringList &arguments, int timeoutMs)
{
    // 1. 保存当前步骤，controlFinished() 将依据它决定下一跳。
    m_controlStep = step;

    // 2. 新命令启动前清空上条命令的输出和错误/超时状态。
    m_controlOutput.clear();
    m_controlErrorHandled = false;
    m_controlTimedOut = false;
    // 3. 验证日志写到当前设备；环境检查使用 index=-1 的批次日志。
    appendLog(step == ControlStep::Validating ? m_validationIndex : -1,
              QStringLiteral("执行：%1").arg(displayCommand(program, arguments)), true);
    // 4. 异步启动命令并启动对应超时定时器，不阻塞 Qt 主界面。
    m_controlProcess.start(program, arguments);//Docker产生输出，触发BatchRunner::controlFinished
    m_controlTimer.start(timeoutMs);
}

// 实时读取环境检查或验证程序输出。验证阶段写入当前设备日志；
// 其他环境检查使用 index=-1，作为批次级日志发送。
void BatchRunner::readControlOutput()
{
    // 1. 只读取从上次 readyRead 信号之后新增的字节。
    const QByteArray bytes = m_controlProcess.readAllStandardOutput();
    if (bytes.isEmpty())
        return;
    // 2. 完整保留输出，结束时还要判断错误关键字和验证结果。
    m_controlOutput.append(bytes);

    // 3. 只有验证输出归属于某台设备；环境命令属于批次级输出。
    const int index = m_controlStep == ControlStep::Validating ? m_validationIndex : -1;

    // 4. 按行添加 [PowerShell] 日志并通过 logMessage 实时发给界面。
    const QStringList lines = QString::fromUtf8(bytes).split(
        QRegularExpression(QStringLiteral("[\r\n]+")), Qt::SkipEmptyParts);
    for (const QString &line : lines)
        appendLog(index, line.trimmed(), true);
}

// m_controlProcess 结束后的状态机分派中心：
// DockerVersion -> InspectContainer -> 必要时 StartContainer -> CheckContainerTools
// -> startNextParser()；Validating 则进入 completeValidation()/failValidation()。
void BatchRunner::controlFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    m_controlTimer.stop();
    readControlOutput();
    if (!m_running || m_controlErrorHandled || m_stopping)
        return;

    const bool normal = exitStatus == QProcess::NormalExit;
    const QString output = QString::fromUtf8(m_controlOutput).trimmed();
    switch (m_controlStep) {
    case ControlStep::DockerVersion:
        // Docker 服务正常后继续 inspect calib，失败则整批终止。
        if (!normal || exitCode != 0 || output.isEmpty()) {
            failBatchEnvironment(QStringLiteral(
                "Docker 服务不可用，请确认 Docker Desktop 已启动。"));
            return;
        }
        emit environmentChanged(true, false, QStringLiteral("正在检查 calib 容器..."));
        runControl(ControlStep::InspectContainer, QStringLiteral("docker"),
                   {QStringLiteral("inspect"), QStringLiteral("calib")},
                   kEnvironmentTimeoutMs);
        return;

    case ControlStep::InspectContainer: {
        // 解析 docker inspect JSON，确认容器存在、运行状态，以及 /data
        // 实际挂载目录和界面数据根目录完全一致，避免处理错误磁盘位置。
        if (!normal || exitCode != 0) {
            failBatchEnvironment(QStringLiteral("未找到 calib 容器。"));
            return;
        }
        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson(m_controlOutput, &error);
        if (error.error != QJsonParseError::NoError || !document.isArray()
            || document.array().isEmpty()) {
            failBatchEnvironment(QStringLiteral("无法解析 calib 容器信息：%1")
                                 .arg(error.errorString()));
            return;
        }
        const QJsonObject container = document.array().first().toObject();
        const bool running = container.value(QStringLiteral("State"))
                                 .toObject().value(QStringLiteral("Running")).toBool();
        QString mountSource;
        for (const QJsonValue &value : container.value(QStringLiteral("Mounts")).toArray()) {
            const QJsonObject mount = value.toObject();
            if (mount.value(QStringLiteral("Destination")).toString()
                == QStringLiteral("/data")) {
                mountSource = mount.value(QStringLiteral("Source")).toString();
                break;
            }
        }
        if (mountSource.isEmpty()) {
            failBatchEnvironment(QStringLiteral("calib 容器未把数据目录挂载到 /data。"));
            return;
        }
        const QString mountedRoot = normalizedMountSource(mountSource);
        const QString selectedRoot = QDir::cleanPath(QDir::fromNativeSeparators(m_dataRoot));
        if (mountedRoot.compare(selectedRoot, Qt::CaseInsensitive) != 0) {
            failBatchEnvironment(QStringLiteral(
                "容器 /data 挂载为 %1，但界面选择的是 %2。")
                .arg(mountSource, QDir::toNativeSeparators(selectedRoot)));
            return;
        }
        if (!running) {//启动docker start calib
            emit environmentChanged(true, false, QStringLiteral("正在启动 calib 容器..."));
            runControl(ControlStep::StartContainer, QStringLiteral("docker"),
                       {QStringLiteral("start"), QStringLiteral("calib")},
                       kEnvironmentTimeoutMs);
            return;
        }
        emit environmentChanged(true, true, QStringLiteral("Docker 与 calib 容器已就绪"));
        const QString toolsCheck = m_mode == BatchMode::ParseOnly
            ? QStringLiteral("test -x /workspace/sinoslam/libs/sinoslam_calibration && "
                             "test -f /workspace/config/CalibrationConfigLS36arm.json && "
                             "command -v script >/dev/null 2>&1 && test -d /data")
            : (m_mode == BatchMode::ValidateOnly
                ? QStringLiteral("test -x /workspace/sinoslam/libs/evaluate_hardware && "
                                 "command -v script >/dev/null 2>&1 && test -d /data")
                : QStringLiteral("test -x /workspace/sinoslam/libs/sinoslam_calibration && "
                                 "test -x /workspace/sinoslam/libs/evaluate_hardware && "
                                 "test -f /workspace/config/CalibrationConfigLS36arm.json && "
                                 "command -v script >/dev/null 2>&1 && test -d /data"));
        runControl(ControlStep::CheckContainerTools, QStringLiteral("docker"),
                   {QStringLiteral("exec"), QStringLiteral("calib"), QStringLiteral("sh"),
                    QStringLiteral("-lc"), toolsCheck}, kEnvironmentTimeoutMs);
        return;
    }

    case ControlStep::StartContainer: {
        // calib 原本停止时由本步骤启动；成功后继续检查程序和配置文件。
        if (!normal || exitCode != 0) {
            failBatchEnvironment(QStringLiteral("calib 容器启动失败：%1").arg(output));
            return;
        }
        emit environmentChanged(true, true, QStringLiteral("calib 容器已启动"));
        const QString toolsCheck = m_mode == BatchMode::ParseOnly
            ? QStringLiteral("test -x /workspace/sinoslam/libs/sinoslam_calibration && "
                             "test -f /workspace/config/CalibrationConfigLS36arm.json && "
                             "command -v script >/dev/null 2>&1 && test -d /data")
            : (m_mode == BatchMode::ValidateOnly
                ? QStringLiteral("test -x /workspace/sinoslam/libs/evaluate_hardware && "
                                 "command -v script >/dev/null 2>&1 && test -d /data")
                : QStringLiteral("test -x /workspace/sinoslam/libs/sinoslam_calibration && "
                                 "test -x /workspace/sinoslam/libs/evaluate_hardware && "
                                 "test -f /workspace/config/CalibrationConfigLS36arm.json && "
                                 "command -v script >/dev/null 2>&1 && test -d /data"));
        runControl(ControlStep::CheckContainerTools, QStringLiteral("docker"),
                   {QStringLiteral("exec"), QStringLiteral("calib"), QStringLiteral("sh"),
                    QStringLiteral("-lc"), toolsCheck}, kEnvironmentTimeoutMs);
        return;
    }

    case ControlStep::CheckContainerTools:
        // 所需可执行文件、配置、script 伪终端工具和 /data 全部存在后，
        // 才调用 startNextParser() 启动批次第一台设备。
        if (!normal || exitCode != 0) {
            failBatchEnvironment(QStringLiteral(
                "容器内缺少解析程序、验证程序、配置文件、script 工具或 /data 目录。"));
            return;
        }
        if (m_mode == BatchMode::ValidateOnly)
            maybeStartValidationPhase();
        else
            startNextParser();
        return;

    case ControlStep::Validating:
        // 退出码 0 为正常；退出码 2 允许进入 completeValidation() 并按
        // warning 处理。其他非零码、崩溃或致命错误关键字都判验证失败。
        if (m_controlTimedOut) {
            failValidation(m_validationIndex, QStringLiteral("验证命令执行超时"), exitCode);
        } else if (!normal || (exitCode != 0 && exitCode != 2)
                   || outputHasFatalError(m_controlOutput)) {
            failValidation(m_validationIndex,
                           QStringLiteral("验证程序异常退出，退出码 %1").arg(exitCode),
                           exitCode);
        } else {
            completeValidation(m_validationIndex, exitCode);
        }
        ++m_validationIndex;
        beginNextValidation();
        return;

    case ControlStep::Idle:
        return;
    }
}

// 控制进程连启动都失败时调用。验证失败只结束当前设备；Docker环境命令
// 无法启动则调用 failBatchEnvironment() 终止整个批次。
void BatchRunner::controlError(QProcess::ProcessError error)
{
    if (!m_running || error != QProcess::FailedToStart || m_controlErrorHandled)
        return;
    m_controlErrorHandled = true;
    m_controlTimer.stop();
    if (m_controlStep == ControlStep::Validating) {
        failValidation(m_validationIndex,
                       QStringLiteral("无法启动 Docker 验证命令：%1")
                           .arg(m_controlProcess.errorString()));
        ++m_validationIndex;
        beginNextValidation();
    } else {
        failBatchEnvironment(QStringLiteral("无法启动 docker 命令：%1")
                             .arg(m_controlProcess.errorString()));
    }
}

// 共用控制进程超时回调。当前主要用于验证：杀掉 docker 客户端进程，
// 并通过 stopContainerProgram() 尝试终止容器内 evaluate_hardware。
void BatchRunner::controlTimedOut()
{
    if (!m_running || m_controlProcess.state() == QProcess::NotRunning)
        return;
    m_controlTimedOut = true;
    appendLog(m_validationIndex, QStringLiteral("验证命令超时，正在终止。"));
    m_controlProcess.kill();
    stopContainerProgram(QStringLiteral("evaluate_hardware"));
}

// 解析流水线推进函数。m_nextParseIndex 指向下一台尚未启动的设备；
// 它可由环境检查完成、CPU释放标记或并行槽位空出三个时机触发。
void BatchRunner::startNextParser()
{
    // 1. 已停止/正在停止时不再创建新的解析进程。
    if (!m_running || m_stopping)
        return;

    // 2. 统计Starting/Running状态，硬性限制最多只有三个解析进程并行。
    int activeParserCount = 0;
    for (const ParseContext *context : m_parseContexts) {
        if (context->process
            && context->process->state() != QProcess::NotRunning) {
            ++activeParserCount;
        }
    }
    if (activeParserCount >= kMaxConcurrentParsers) {
        m_parserStartPending = true;
        appendLog(-1, QStringLiteral(
            "已有%1台设备正在并行解析，下一台等待解析槽位释放。")
                      .arg(kMaxConcurrentParsers));
        return;
    }

    // 3. 所有设备都已经“启动过解析”时不再越界，转而唤醒验证和结束判断。
    if (m_nextParseIndex >= m_devices.size()) {
        m_parserStartPending = false;
        maybeStartValidationPhase();
        return;
    }

    // 4. 消耗一次待启动请求并增加游标，确保释放信号不会重复启动同一台。
    m_parserStartPending = false;
    const int index = m_nextParseIndex++;
    // META 已在主界面自动扫描时确定；Docker 和 /data 就绪后直接解析。
    // 文件在此后若被删除或损坏，由解析程序的退出码和输出完整性检查处理。
    // 5. 进入当前设备的实际解析启动函数。
    launchParser(index);
}

// 启动指定设备解析：删除旧 hardware -> 创建独立 QProcess/定时器 ->
// 连接输出、结束、错误、超时信号 -> 拼接 Docker 命令 -> 启动 2 小时计时。
void BatchRunner::launchParser(int index)
{
    // 1. 取得当前设备最终结果和实时解析上下文。
    DeviceRunResult &result = m_results[index];
    ParseContext *context = m_parseContexts.at(index);

    // 2. 删除旧 hardware，确保后续检查到的文件一定由本次解析产生。
    if (QFileInfo::exists(result.hardwarePath) && !QFile::remove(result.hardwarePath)) {
        // 不能删除旧结果时禁止解析，防止后面把旧 JSON 误认成本次结果。
        failParse(index, QStringLiteral(
            "无法删除旧 sinoslam_hardware.json，文件可能正被占用"));
        parserReleaseSeen(index, true);
        return;
    }

    // 3. 每台设备建立独立解析进程和独立 2 小时超时器。
    context->process = new QProcess(this);
    context->process->setProcessChannelMode(QProcess::MergedChannels);
    context->timer = new QTimer(this);
    context->timer->setSingleShot(true);
    // 4. 将进程的输出、启动、结束、启动失败和超时分别连接到对应处理函数。
    connect(context->process, &QProcess::readyReadStandardOutput,
            this, [this, index] { readParserOutput(index); });// 读取增量输出并写日志
    connect(context->process, &QProcess::started, this, [this, index] {
        appendLog(index, QStringLiteral(
            "解析进程已启动，正在等待容器返回初始化信息和实时进度。"));
    });
    connect(context->process,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, index](int code, QProcess::ExitStatus status) {
                parserFinished(index, code, status);
            });
    connect(context->process, &QProcess::errorOccurred,
            this, [this, index](QProcess::ProcessError error) {
                parserError(index, error);
            });
    connect(context->timer, &QTimer::timeout,
            this, [this, index] { parserTimedOut(index); });

    // 5. 在界面标记正在解析，并记录时间用于排除旧 hardware 文件。
    emit deviceStateChanged(index, QStringLiteral("正在解析"), QStringLiteral("进行中"),
                            QStringLiteral("等待"), QString());
    context->startedMs = QDateTime::currentMSecsSinceEpoch();
    // 6. 生成容器内算法命令，输入固定为 /data/<SN>/<最新META>。
    const BatchDevice &device = m_devices.at(index);
    const QString calibrationCommand = QStringLiteral(
        "exec /workspace/sinoslam/libs/sinoslam_calibration "
        "--config_file /workspace/config/CalibrationConfigLS36arm.json "
        "--data_path %1 --series_number %2")
        .arg(shellQuote(containerMetaPath(device)), shellQuote(device.sn));
    // QProcess does not own a console, so `docker exec -t` fails on Windows.
    // Create the pseudo terminal inside the container instead.  This preserves
    // the interactive ros shell environment and makes progress output visible
    // immediately without requiring a terminal on the Windows side.
    // 7. 用容器内 script 伪终端包装 bash -ic，使 ROS 环境和实时输出都可用。
    const QString terminalCommand = QStringLiteral("/bin/bash -ic %1")
                                        .arg(shellQuote(calibrationCommand));
    const QStringList arguments = {
        // Windows QProcess 不是真实终端，不能直接 docker exec -t。
        // 容器内 script 创建伪终端，bash -ic 加载 ros 用户交互环境，
        // 同时让算法的逐帧进度能够实时回传到 readyReadStandardOutput。
        QStringLiteral("exec"),
        QStringLiteral("-u"), QStringLiteral("ros"),
        QStringLiteral("-w"), QStringLiteral("/workspace"), QStringLiteral("calib"),
        QStringLiteral("script"), QStringLiteral("-qefc"), terminalCommand,
        QStringLiteral("/dev/null")
    };
    // 8. 写入实际命令，异步启动 docker，并开始解析超时计时。
    appendLog(index, QStringLiteral("执行：%1")
                       .arg(displayCommand(QStringLiteral("docker"), arguments)), true);
    context->process->start(QStringLiteral("docker"), arguments);
    context->timer->start(kParseTimeoutMs);
}

// 读取单台解析的增量输出并写日志；完整输出还会保存在context->output，
// 用于结束时检查致命错误。lineBuffer负责跨QProcess输出块拼接完整行，
// 避免触发行和它的下一行被拆包时提前或漏掉CPU释放判断。
void BatchRunner::readParserOutput(int index)
{
    // 1. 从当前设备独立 QProcess 读取本次新增输出。
    ParseContext *context = m_parseContexts.at(index);
    const QByteArray bytes = context->process->readAllStandardOutput();
    if (bytes.isEmpty())
        return;
    // 2. 保存完整原始输出，解析结束后还要检查崩溃关键字；同时追加到行缓存。
    context->output.append(bytes);
    context->lineBuffer.append(bytes);

    // 3. 只处理已经收到换行符的完整行；不完整的尾行留给下一次输出继续拼接。
    while (true) {
        const int crIndex = context->lineBuffer.indexOf('\r');
        const int lfIndex = context->lineBuffer.indexOf('\n');
        int delimiterIndex = -1;
        if (crIndex >= 0 && (lfIndex < 0 || crIndex < lfIndex))
            delimiterIndex = crIndex;
        else if (lfIndex >= 0)
            delimiterIndex = lfIndex;

        if (delimiterIndex < 0)
            break;

        // 本次数据若刚好以CR结尾，先等下一块确认它是不是CRLF，
        // 防止随后到达的LF被误认为一条空的“下一行”。
        if (context->lineBuffer.at(delimiterIndex) == '\r'
            && delimiterIndex + 1 == context->lineBuffer.size()) {
            break;
        }

        const QByteArray lineBytes = context->lineBuffer.left(delimiterIndex);
        int delimiterLength = 1;
        if (context->lineBuffer.at(delimiterIndex) == '\r'
            && delimiterIndex + 1 < context->lineBuffer.size()
            && context->lineBuffer.at(delimiterIndex + 1) == '\n') {
            delimiterLength = 2;
        }
        context->lineBuffer.remove(0, delimiterIndex + delimiterLength);
        handleParserOutputLine(index, QString::fromUtf8(lineBytes));
    }
}

// 逐条处理完整PowerShell输出行：先确认进入0-th batch optimization，
// 再用行结构识别Ceres表格中迭代号1的数据行。各设备数值可以不同。
void BatchRunner::handleParserOutputLine(int index, const QString &line)
{
    ParseContext *context = m_parseContexts.at(index);
    const QString trimmed = line.trimmed();
    if (!trimmed.isEmpty())
        appendLog(index, trimmed, true);

    if (context->releaseSeen)
        return;

    if (!m_releaseMarker.isEmpty()
        && line.contains(m_releaseMarker, Qt::CaseInsensitive)) {
        context->releaseTriggerLineSeen = true;
        appendLog(index, QStringLiteral(
            "已进入0-th批优化；等待Ceres迭代号1的数据行后再释放CPU标记。"));
        return;
    }

    if (context->releaseTriggerLineSeen) {
        // 只固定“迭代号为1且其后至少有两列数值”这一结构，不固定cost等实际值。
        // 示例：1  4.974979e+05  1.62e+05 ...
        static const QRegularExpression iterationOnePattern(QStringLiteral(
            "^\\s*1\\s+[+-]?(?:\\d+(?:\\.\\d*)?|\\.\\d+)(?:[eE][+-]?\\d+)?"
            "\\s+[+-]?(?:\\d+(?:\\.\\d*)?|\\.\\d+)(?:[eE][+-]?\\d+)?(?:\\s+|$)"));
        if (iterationOnePattern.match(line).hasMatch()) {
            appendLog(index, QStringLiteral(
                "已识别到0-th批优化的迭代号1数据行，现释放CPU高占用结束标记。"));
            parserReleaseSeen(index, false);
        }
    }
}

// 解析资源释放处理：每台只执行一次，然后调用 startNextParser()。
// fallback=false表示已识别到0-th批优化的迭代号1；fallback=true表示直到进程退出
// 仍未识别到该行。该函数只申请启动下一台，不会把当前设备判为解析成功。
void BatchRunner::parserReleaseSeen(int index, bool fallback)
{
    // 1. releaseSeen 保证同一设备最多只放行一次，避免重复启动下一台。
    ParseContext *context = m_parseContexts.at(index);
    if (context->releaseSeen)
        return;
    // 2. 区分算法主动标记和进程退出后的保底放行，并登记一次调度请求。
    context->releaseSeen = true;
    m_parserStartPending = true;
    appendLog(index, fallback
        ? QStringLiteral("未识别到0-th批优化迭代号1；解析进程已结束，安全启动下一台解析。")
        : QStringLiteral("收到 CPU 高占用阶段结束标志；申请启动下一台解析，当前解析仍未完成。"));
    // 3. 主动标记到达时，当前设备显示为解析尾段，而不是解析成功。
    if (!context->completed) {
        emit deviceStateChanged(index, QStringLiteral("解析尾段"), QStringLiteral("进行中"),
                                QStringLiteral("等待"),
                                QStringLiteral("主要 CPU 阶段已结束，等待解析进程完整退出"));
    }
    // 4. 推进解析游标，启动下一台；若已无下一台则唤醒验证/结束判断。
    startNextParser();
}

// 单台解析进程完整退出后的判定入口。先读尽最后输出并保证下一台解析被
// 放行，再按“超时/崩溃/非零退出码/致命输出”选择 failParse 或 completeParse。
void BatchRunner::parserFinished(int index, int exitCode, QProcess::ExitStatus exitStatus)
{
    // 1. 批次停止或该设备已收口时忽略迟到的 finished 信号。
    if (!m_running || m_stopping)
        return;
    ParseContext *context = m_parseContexts.at(index);
    if (context->completed)
        return;
    // 2. 停止解析超时器，并读取进程退出前最后一批输出。
    if (context->timer)
        context->timer->stop();
    readParserOutput(index);
    // 3. 处理进程退出前没有换行符的最后一行；它也可能正好是迭代号1数据行。
    if (!context->lineBuffer.isEmpty()) {
        const QByteArray finalLine = context->lineBuffer;
        context->lineBuffer.clear();
        handleParserOutputLine(index, QString::fromUtf8(finalLine));
    }
    // 4. 未识别到0-th批优化迭代号1时，在进程完整退出这一安全时刻兜底放行。
    if (!context->releaseSeen)
        parserReleaseSeen(index, true);

    // 5. 超时优先；其次检查崩溃、退出码和致命输出；全部正常才检查结果文件。
    if (context->timedOut) {
        failParse(index, QStringLiteral("解析命令执行超时"), exitCode);
    } else if (exitStatus != QProcess::NormalExit || exitCode != 0
               || outputHasFatalError(context->output)) {
        failParse(index, QStringLiteral("解析程序异常退出，退出码 %1").arg(exitCode),
                  exitCode);
    } else {
        completeParse(index, exitCode);
    }

    // 6. 如果某台此前已到释放点、但因三路并行上限未能启动下一台，
    // 当前进程退出后槽位已经空出，在这里补启且仍经过三路上限检查。
    if (m_parserStartPending)
        startNextParser();
}

// 处理 QProcess 本身无法启动（例如找不到 docker.exe）。普通算法非零退出
// 由 parserFinished() 判断，不在这里重复处理。
void BatchRunner::parserError(int index, QProcess::ProcessError error)
{
    ParseContext *context = m_parseContexts.at(index);
    if (!m_running || m_stopping || context->completed
        || error != QProcess::FailedToStart)
        return;
    if (context->timer)
        context->timer->stop();
    failParse(index, QStringLiteral("无法启动 Docker 解析命令：%1")
                       .arg(context->process->errorString()));
    parserReleaseSeen(index, true);
}

// 解析超过 2 小时：标记 timedOut、终止 Windows 侧进程，并按 META 路径
// 尝试清理容器内对应解析；随后 parserFinished() 统一记录失败结果。
void BatchRunner::parserTimedOut(int index)
{
    ParseContext *context = m_parseContexts.at(index);
    if (!m_running || context->completed || !context->process
        || context->process->state() == QProcess::NotRunning)
        return;
    context->timedOut = true;
    appendLog(index, QStringLiteral("解析命令超时，正在终止该设备解析。"));
    context->process->kill();
    stopContainerProgram(containerMetaPath(m_devices.at(index)));
}

// 固化解析失败：completed=true、累计完成数、记录失败/超时和退出码。
// 组合模式唤醒验证调度并跳过失败项；仅解析模式直接收口该设备。
void BatchRunner::failParse(int index, const QString &reason, int exitCode)
{
    // 1. 防止 finished/error 等多个信号重复结算同一设备。
    ParseContext *context = m_parseContexts.at(index);
    if (context->completed)
        return;
    // 2. 固化内部英文状态；超时和普通失败分别保存，验证直接跳过。
    context->completed = true;
    ++m_finishedParseCount;
    DeviceRunResult &result = m_results[index];
    result.parseStatus = context->timedOut ? QStringLiteral("timeout")
                                            : QStringLiteral("failed");
    result.parseExitCode = exitCode;
    result.parseError = reason;
    result.validateStatus = m_mode == BatchMode::ParseOnly
        ? QStringLiteral("not_run") : QStringLiteral("skipped");
    // 3. 更新界面；仅解析模式在此结束设备，组合模式唤醒验证队列。
    appendLog(index, QStringLiteral("解析失败：%1").arg(reason));
    emit deviceStateChanged(index, QStringLiteral("解析失败"), QStringLiteral("失败"),
                            m_mode == BatchMode::ParseOnly
                                ? QStringLiteral("未执行") : QStringLiteral("已跳过"),
                            reason);
    if (m_mode == BatchMode::ParseOnly)
        finishDevice(index);
    maybeStartValidationPhase();
}

// 外部进程正常退出后的第二层成功检查：hardware JSON 必须存在、非空、
// 可解析，并且修改时间不早于本次解析启动时间。通过后才进入验证队列。
void BatchRunner::completeParse(int index, int exitCode)
{
    // 1. 防止重复成功判定，并取得本次期望的 hardware 输出路径。
    ParseContext *context = m_parseContexts.at(index);
    if (context->completed)
        return;
    DeviceRunResult &result = m_results[index];
    // 2. 检查 hardware 文件存在、非空且为合法 JSON；失败会转入 failParse()。
    QString jsonError;
    if (!validJsonFile(result.hardwarePath, nullptr, &jsonError)) {
        failParse(index, QStringLiteral("未生成有效 sinoslam_hardware.json：%1")
                            .arg(jsonError), exitCode);
        return;
    }
    // 3. 检查修改时间，避免把解析前残留的旧文件当成本次结果。
    if (QFileInfo(result.hardwarePath).lastModified().toMSecsSinceEpoch()
        < context->startedMs) {
        failParse(index, QStringLiteral("sinoslam_hardware.json 不是本次解析生成的"),
                  exitCode);
        return;
    }
    // 4. 至此才正式标记解析成功，并累计完整结束数量。
    context->completed = true;
    ++m_finishedParseCount;
    result.parseStatus = QStringLiteral("success");
    result.parseExitCode = exitCode;
    // 5. 单台解析成功后立即把hardware覆盖复制到all_data/<SN>.json。
    copyParsedHardwareFile(index);
    // 6. 更新界面。组合模式登记成功后立即唤醒单路验证调度器。
    appendLog(index, QStringLiteral("解析进程已完整结束，硬件文件检查通过。"));
    if (m_mode == BatchMode::ParseOnly) {
        result.validateStatus = QStringLiteral("not_run");
        emit deviceStateChanged(index, QStringLiteral("解析完成"), QStringLiteral("成功"),
                                QStringLiteral("未执行"), QStringLiteral("仅解析任务已完成"));
        finishDevice(index);
    } else {
        emit deviceStateChanged(index, QStringLiteral("解析完成"), QStringLiteral("成功"),
                                QStringLiteral("等待"),
                                QStringLiteral("解析成功，等待验证进程空闲"));
    }
    maybeStartValidationPhase();
}

// 验证调度入口：组合模式在任意单台解析完成后即可启用验证，因此后续设备
// 的解析允许与当前设备验证并行。仅解析不验证；仅验证从第0台直接开始。
void BatchRunner::maybeStartValidationPhase()
{
    // 1. 停止状态不再启动新验证。
    if (!m_running || m_stopping)
        return;

    // 2. 仅解析不启动验证；所有解析完成后结束整个批次。
    if (m_mode == BatchMode::ParseOnly) {
        if (m_finishedParseCount == m_devices.size())
            finishBatch(false);
        return;
    }

    // 3. 组合模式第一次有设备完成解析时即打开验证调度；验证仍按选择顺序
    // 单路执行，遇到当前索引尚未解析完成时暂时等待其完成。
    if (!m_validationPhaseStarted) {
        m_validationPhaseStarted = true;
        m_validationIndex = 0;
        appendLog(-1, m_mode == BatchMode::ValidateOnly
            ? QStringLiteral("仅验证阶段开始：按选择顺序检查已有硬件文件并逐台验证。")
            : QStringLiteral("已有设备解析完成，开始按选择顺序逐台验证；后续设备解析可与当前验证并行。"));
    }
    // 4. 验证共用一个控制进程，因此验证之间始终逐台串行。
    beginNextValidation();
}

// 单台设备解析成功并通过JSON、修改时间检查后，立即把它的
// sinoslam_hardware.json复制到<数据根目录>/all_data/<SN>.json。
// 同名SN文件覆盖；复制失败只记录日志，不改变已经确认的解析结果。
void BatchRunner::copyParsedHardwareFile(int index)
{
    if (index < 0 || index >= m_results.size())
        return;

    const QString outputDir = QDir(m_dataRoot).filePath(QStringLiteral("all_data"));
    if (!QDir().mkpath(outputDir)) {
        appendLog(index, QStringLiteral("无法创建hardware汇总目录：%1")
                           .arg(QDir::toNativeSeparators(outputDir)));
        return;
    }

    const DeviceRunResult &result = m_results.at(index);
    const QString destination = QDir(outputDir).filePath(
        QStringLiteral("%1.json").arg(result.sn));
    if ((QFileInfo::exists(destination) && !QFile::remove(destination))
        || !QFile::copy(result.hardwarePath, destination)) {
        appendLog(index, QStringLiteral("hardware汇总复制失败：%1")
                             .arg(QDir::toNativeSeparators(destination)));
        return;
    }

    appendLog(index, QStringLiteral("解析成功，hardware已立即复制到：%1")
                         .arg(QDir::toNativeSeparators(destination)));
}

// 单路、按选择顺序的验证调度器。验证共用 m_controlProcess，因此同一时刻
// 最多一台验证；若当前索引仍在解析则返回等待，解析完成时会再次唤醒。
// 解析失败项调用 finishDevice() 后跳过；解析成功项调用 beginValidation()。
void BatchRunner::beginNextValidation()
{
    // 1. 批次无效、验证尚未启用或已有验证命令运行时直接等待。
    if (!m_running || m_stopping || !m_validationPhaseStarted
        || m_controlProcess.state() != QProcess::NotRunning)
        return;
    // 2. 按用户选择顺序逐台检查，不允许后面的设备越过前面的设备验证。
    while (m_validationIndex < m_devices.size()) {
        // 2.1 当前索引还在解析：先返回；completeParse/failParse 会再次调用本函数。
        if (!m_parseContexts.at(m_validationIndex)->completed)
            return;

        // 2.2 组合模式只验证解析成功项；仅验证模式直接检查已有 hardware。
        if (m_mode == BatchMode::ValidateOnly
            || m_results.at(m_validationIndex).parseStatus == QStringLiteral("success")) {
            beginValidation(m_validationIndex);
            return;
        }
        // 2.3 当前索引解析失败：验证状态已经是 skipped，直接结束该设备并后移。
        finishDevice(m_validationIndex);
        ++m_validationIndex;
    }
    // 3. 验证索引走到末尾且所有解析都已真正结束时，才结束整个批次。
    if (m_finishedParseCount == m_devices.size())
        finishBatch(false);
}

// 启动单台验证。先确认删除目标严格是当前 SN 下的 evaluate_output，
// 清空并重建目录以避免旧报告混入，再通过 runControl(Validating) 执行。
void BatchRunner::beginValidation(int index)
{
    // 1. 计算 Windows 侧验证输出目录及规范化路径。
    const BatchDevice &device = m_devices.at(index);
    DeviceRunResult &result = m_results[index];
    // 仅验证不会生成 hardware，因此必须先确认现有文件可读且是合法 JSON。
    // 检查失败只结束当前设备，后续选中设备仍会继续验证。
    QString hardwareError;
    if (!validJsonFile(result.hardwarePath, nullptr, &hardwareError)) {
        failValidation(index, QStringLiteral("缺少有效 sinoslam_hardware.json：%1")
                                  .arg(hardwareError));
        ++m_validationIndex;
        beginNextValidation();
        return;
    }
    const QString outputDir = QDir(device.deviceDir).filePath(QStringLiteral("evaluate_output"));
    const QString cleanDeviceDir = QDir::cleanPath(QDir::fromNativeSeparators(device.deviceDir));
    const QString cleanOutputDir = QDir::cleanPath(QDir::fromNativeSeparators(outputDir));
    // 2. 安全检查：只能删除当前 SN 目录内且名称正好为 evaluate_output 的目录。
    if (!cleanOutputDir.startsWith(cleanDeviceDir + QLatin1Char('/'),
                                   Qt::CaseInsensitive)
        || QFileInfo(cleanOutputDir).fileName() != QStringLiteral("evaluate_output")) {
        failValidation(index, QStringLiteral("验证输出目录安全检查失败"));
        ++m_validationIndex;
        beginNextValidation();
        return;
    }
    // 3. 同名验证产物采用整目录覆盖策略；META 和 hardware 文件不会在这里删除。
    QDir oldOutput(cleanOutputDir);
    if (oldOutput.exists() && !oldOutput.removeRecursively()) {
        failValidation(index, QStringLiteral(
            "无法清空旧 evaluate_output，结果文件可能正被占用"));
        ++m_validationIndex;
        beginNextValidation();
        return;
    }
    // 4. 重新创建空输出目录，保证 metrics 和警告图片全部来自本次验证。
    if (!QDir().mkpath(cleanOutputDir)) {
        failValidation(index, QStringLiteral("无法重新创建 evaluate_output 目录"));
        ++m_validationIndex;
        beginNextValidation();
        return;
    }

    // 5. 更新界面为“正在验证”。解析列仍保持成功。
    emit deviceStateChanged(index, QStringLiteral("正在验证"),
                            m_mode == BatchMode::ValidateOnly
                                ? QStringLiteral("未执行") : QStringLiteral("成功"),
                            QStringLiteral("进行中"), QString());
    // 6. 生成 evaluate_hardware 参数：META、刚生成的 hardware、输出目录。
    const QString validationCommand = QStringLiteral(
        "exec /workspace/sinoslam/libs/evaluate_hardware "
        "--data %1 --hardware %2 --output %3")
        .arg(shellQuote(containerMetaPath(device)),
             shellQuote(QStringLiteral("/data/%1/sinoslam_hardware.json").arg(device.sn)),
             shellQuote(QStringLiteral("/data/%1/evaluate_output").arg(device.sn)));
    const QString terminalCommand = QStringLiteral("/bin/bash -ic %1")
                                        .arg(shellQuote(validationCommand));
    // 7. 通过共用 m_controlProcess 启动验证；因此验证之间不会并行。
    runControl(ControlStep::Validating, QStringLiteral("docker"),
               {QStringLiteral("exec"),
                QStringLiteral("-u"), QStringLiteral("ros"),
                QStringLiteral("-w"), QStringLiteral("/workspace"), QStringLiteral("calib"),
                QStringLiteral("script"), QStringLiteral("-qefc"), terminalCommand,
                QStringLiteral("/dev/null")},
               kValidateTimeoutMs);
}

// 固化验证失败/超时并结束当前设备。controlFinished() 随后增加验证索引，
// 调用 beginNextValidation() 继续下一台，不阻塞整个批次。
void BatchRunner::failValidation(int index, const QString &reason, int exitCode)
{
    DeviceRunResult &result = m_results[index];
    result.validateStatus = m_controlTimedOut ? QStringLiteral("timeout")
                                               : QStringLiteral("failed");
    result.validateExitCode = exitCode;
    result.validateError = reason;
    appendLog(index, QStringLiteral("验证失败：%1").arg(reason));
    emit deviceStateChanged(index, QStringLiteral("已结束"),
                            m_mode == BatchMode::ValidateOnly
                                ? QStringLiteral("未执行") : QStringLiteral("成功"),
                            QStringLiteral("失败"), reason);
    finishDevice(index);
}

// 验证进程正常结束后的结果检查：metrics.json 必须是有效 JSON；退出码 2
// 或任何 warning 字段会记为 warning，否则记为 success，最后 finishDevice()。
void BatchRunner::completeValidation(int index, int exitCode)
{
    // 1. 读取当前设备结果对象，并准备接收 JSON 检查错误和警告数量。
    DeviceRunResult &result = m_results[index];
    QString jsonError;
    int warningCount = 0;
    // 2. metrics.json 必须存在、非空且语法合法，否则转为验证失败。
    if (!validJsonFile(result.metricsPath, &warningCount, &jsonError)) {
        failValidation(index, QStringLiteral("未生成有效 metrics.json：%1").arg(jsonError),
                       exitCode);
        return;
    }
    // 3. 保存外部程序退出码和从 metrics.json 递归统计出的警告数。
    result.validateExitCode = exitCode;
    result.warningCount = warningCount;
    // 4. 退出码2或存在warning字段记为警告；退出码0且无警告才记为成功。
    if (exitCode == 2 || warningCount > 0) {
        result.validateStatus = QStringLiteral("warning");
        result.validateError = QStringLiteral("验证完成，但存在 %1 项警告")
                                   .arg(warningCount);
        emit deviceStateChanged(index, QStringLiteral("已完成"),
                                m_mode == BatchMode::ValidateOnly
                                    ? QStringLiteral("未执行") : QStringLiteral("成功"),
                                QStringLiteral("警告"), result.validateError);
    } else {
        result.validateStatus = QStringLiteral("success");
        emit deviceStateChanged(index, QStringLiteral("已完成"),
                                m_mode == BatchMode::ValidateOnly
                                    ? QStringLiteral("未执行") : QStringLiteral("成功"),
                                QStringLiteral("成功"), QString());
    }
    // 5. 当前设备解析和验证均已得到最终状态，进入统一收口。
    finishDevice(index);
}

// 单台设备唯一收口。resultEmitted 防止重复写库；记录结束时间、关闭日志，
// 再发 deviceFinished，由 MainWindow::handleDeviceFinished() 写 SQLite。
void BatchRunner::finishDevice(int index)
{
    // 1. 防止多个异常/结束路径重复发 deviceFinished、重复写数据库。
    ParseContext *context = m_parseContexts.at(index);
    if (context->resultEmitted)
        return;
    // 2. 固化结束时间并记录解析、验证内部状态。
    context->resultEmitted = true;
    DeviceRunResult &result = m_results[index];
    result.finishedAt = QDateTime::currentDateTime().toString(Qt::ISODate);
    appendLog(index, QStringLiteral("设备处理结束：解析=%1，验证=%2")
                       .arg(result.parseStatus, result.validateStatus));
    // 3. 日志写完后关闭文件，确保磁盘内容立即完整可读。
    if (context->logFile) {
        context->logFile->close();
        delete context->logFile;
        context->logFile = nullptr;
    }
    // 4. 发给 MainWindow::handleDeviceFinished()，由它调用数据库 saveResult()。
    emit deviceFinished(index, result);
}

// 整批唯一收口：停止定时器、清除运行标志并发出 batchFinished。
// MainWindow 收到后更新进度窗口并重新扫描数据目录。
void BatchRunner::finishBatch(bool cancelled)
{
    // 1. 防止重复结束批次。
    if (!m_running)
        return;
    // 2. 停止控制计时并恢复空闲状态。
    m_controlTimer.stop();
    m_controlStep = ControlStep::Idle;
    m_running = false;
    // 3. 通知主窗口重新启用操作，并由 handleBatchFinished() 重新扫描状态。
    emit runningChanged(false);
    emit batchFinished(cancelled);
}

// Docker、容器、挂载或工具检查失败时终止整个批次，并通过 fatalError
// 让主窗口弹出原因；此时不会启动任何 META 解析。
void BatchRunner::failBatchEnvironment(const QString &reason)
{
    appendLog(-1, QStringLiteral("环境检查失败：%1").arg(reason));
    m_controlTimer.stop();
    m_controlStep = ControlStep::Idle;
    m_running = false;
    emit environmentChanged(false, false, reason);
    emit fatalError(reason);
    emit runningChanged(false);
    emit batchFinished(false);
}

// 日志统一入口：增加毫秒时间戳和来源标签；有效设备索引同时写磁盘日志，
// 然后发 logMessage 给进度窗口。index=-1 代表批次/环境级日志。
void BatchRunner::appendLog(int index, const QString &line, bool powerShellOutput)
{
    // 1. 给每行添加毫秒时间戳，并明确区分软件流程与外部命令原始输出。
    const QString stamped = QStringLiteral("[%1] [%2] %3")
        .arg(QDateTime::currentDateTime().toString(
                 QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")),
             powerShellOutput ? QStringLiteral("PowerShell") : QStringLiteral("软件"),
             line);
    // 2. 有效设备索引写入该 SN 的 processing_logs 文件；批次级日志不落到单台文件。
    if (index >= 0 && index < m_parseContexts.size()) {
        QFile *file = m_parseContexts.at(index)->logFile;
        if (file && file->isOpen()) {
            QTextStream stream(file);
            stream.setCodec("UTF-8");
            stream << stamped << '\n';
            stream.flush();
        }
    }
    // 3. 无论是否写磁盘都发给界面；进度窗口按设备索引保存并每秒刷新。
    emit logMessage(index, stamped);
}

// 为单台设备创建 processing_logs/批次号_SN.log，覆盖同批次同名日志。
void BatchRunner::openLog(int index)
{
    const BatchDevice &device = m_devices.at(index);
    const QString logDir = QDir(device.deviceDir).filePath(QStringLiteral("processing_logs"));
    QDir().mkpath(logDir);
    DeviceRunResult &result = m_results[index];
    result.logPath = QDir(logDir).filePath(
        QStringLiteral("%1_%2.log").arg(m_batchId, device.sn));
    ParseContext *context = m_parseContexts.at(index);
    context->logFile = new QFile(result.logPath, this);
    context->logFile->open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate);
    appendLog(index, QStringLiteral(
        "日志来源说明：[软件] 为上位机流程日志；[PowerShell] 为 Docker/Bash 命令及其原始输出。"));
    appendLog(index, QStringLiteral("开始处理设备 %1，META=%2")
                       .arg(device.sn, device.metaName));
}

// 关闭并释放所有仍打开的设备日志，供新批次初始化和析构清理调用。
void BatchRunner::closeLogs()
{
    for (ParseContext *context : m_parseContexts) {
        if (context && context->logFile) {
            context->logFile->close();
            delete context->logFile;
            context->logFile = nullptr;
        }
    }
}

// 把 Windows 设备快照映射为容器共享路径 /data/<SN>/<META>。
QString BatchRunner::containerMetaPath(const BatchDevice &device) const
{
    return QStringLiteral("/data/%1/%2").arg(device.sn, device.metaName);
}

// 通用 JSON 完整性检查：存在、非空、可打开、语法合法；传入 warningCount
// 时额外递归统计 warning 字段，供验证结果分成 success/warning。
bool BatchRunner::validJsonFile(const QString &path, int *warningCount,
                                QString *errorMessage) const
{
    QFile file(path);
    if (!file.exists() || file.size() <= 0) {
        if (errorMessage)
            *errorMessage = QStringLiteral("文件不存在或为空");
        return false;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        if (errorMessage)
            *errorMessage = file.errorString();
        return false;
    }
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || document.isNull()) {
        if (errorMessage)
            *errorMessage = error.errorString();
        return false;
    }
    if (warningCount) {
        *warningCount = warningCountInValue(document.isObject()
            ? QJsonValue(document.object()) : QJsonValue(document.array()));
    }
    return true;
}

// 即使退出码为 0，只要原始输出出现这些崩溃关键字也判定外部程序失败。
bool BatchRunner::outputHasFatalError(const QByteArray &output) const
{
    const QString text = QString::fromUtf8(output);
    return text.contains(QStringLiteral("Aborted"), Qt::CaseInsensitive)
        || text.contains(QStringLiteral("core dumped"), Qt::CaseInsensitive)
        || text.contains(QStringLiteral("Segmentation fault"), Qt::CaseInsensitive);
}

// 在 calib 容器内异步执行 pkill，处理仅 kill Windows docker 客户端后
// 容器内算法仍可能继续运行的情况。
void BatchRunner::stopContainerProgram(const QString &pattern)
{
    QString safe = pattern;
    safe.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    QProcess::startDetached(QStringLiteral("docker"),
        {QStringLiteral("exec"), QStringLiteral("calib"), QStringLiteral("sh"),
         QStringLiteral("-lc"),
         QStringLiteral("pkill -TERM -f '%1' || true").arg(safe)});
}

// 用户停止批次的总清理入口：停止定时器和所有 QProcess、给未完成设备
// 写 interrupted/skipped、逐台 finishDevice()、清理容器算法，最后结束批次。
void BatchRunner::stop()
{
    if (!m_running || m_stopping)
        return;
    m_stopping = true;
    appendLog(-1, QStringLiteral("用户请求停止本批任务。"));
    m_controlTimer.stop();
    if (m_controlProcess.state() != QProcess::NotRunning) {
        m_controlProcess.disconnect(this);
        m_controlProcess.kill();
    }
    for (int i = 0; i < m_parseContexts.size(); ++i) {
        ParseContext *context = m_parseContexts.at(i);
        if (context->timer)
            context->timer->stop();
        if (context->process && context->process->state() != QProcess::NotRunning) {
            context->process->disconnect(this);
            context->process->kill();
        }
        DeviceRunResult &result = m_results[i];
        if (result.parseStatus.isEmpty()) {
            result.parseStatus = QStringLiteral("interrupted");
            result.parseError = QStringLiteral("用户停止任务");
            result.validateStatus = QStringLiteral("skipped");
        } else if (result.validateStatus.isEmpty()) {
            result.validateStatus = QStringLiteral("interrupted");
            result.validateError = QStringLiteral("用户停止任务");
        }
        if (!context->resultEmitted) {
            emit deviceStateChanged(i, QStringLiteral("已取消"), QStringLiteral("已取消"),
                                    QStringLiteral("已取消"), QStringLiteral("批次已停止"));
            finishDevice(i);
        }
    }
    stopContainerProgram(QStringLiteral("sinoslam_calibration"));
    stopContainerProgram(QStringLiteral("evaluate_hardware"));
    finishBatch(true);
}
