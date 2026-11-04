#include "mainwindow.h"
#include "batchrunner.h"
#include "configuploaddialog.h"

#include <QAbstractItemView>
#include <QColor>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileInfoList>
#include <QFrame>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QStatusBar>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QDialog>
#include <QTextEdit>
#include <QUrl>
#include <QVBoxLayout>

namespace {
// 创建顶部统计卡片，并通过 valueOut 把数值 QLabel 返回给 MainWindow 保存。
QWidget *makeMetricCard(const QString &title, QLabel **valueOut, QWidget *parent)
{
    auto *card = new QFrame(parent);
    card->setObjectName(QStringLiteral("metricCard"));
    auto *layout = new QVBoxLayout(card);
    layout->setContentsMargins(16, 12, 16, 12);
    layout->setSpacing(2);
    auto *value = new QLabel(QStringLiteral("0"), card);
    value->setObjectName(QStringLiteral("metricValue"));
    auto *caption = new QLabel(title, card);
    caption->setObjectName(QStringLiteral("mutedText"));
    layout->addWidget(value);
    layout->addWidget(caption);
    *valueOut = value;
    return card;
}

// 把数据库内部英文状态转换成主界面中文；未知状态保持原文。
QString localizedRunStatus(const QString &status)
{
    if (status == QStringLiteral("success"))
        return QStringLiteral("成功");
    if (status == QStringLiteral("warning"))
        return QStringLiteral("警告");
    if (status == QStringLiteral("failed"))
        return QStringLiteral("失败");
    if (status == QStringLiteral("timeout"))
        return QStringLiteral("超时");
    if (status == QStringLiteral("interrupted"))
        return QStringLiteral("中断");
    if (status == QStringLiteral("skipped"))
        return QStringLiteral("跳过");
    return status.isEmpty() ? QStringLiteral("未执行") : status;
}

// 将解析和验证两个独立状态合成为主界面的“解析… / 验证…”文字。
QString combinedMetaStatus(const QString &parseStatus, const QString &validateStatus)
{
    return QStringLiteral("解析%1 / 验证%2")
        .arg(localizedRunStatus(parseStatus), localizedRunStatus(validateStatus));
}
}

// 启动顺序：创建界面 -> 应用样式 -> 打开数据库 -> 创建 BatchRunner
// -> 连接调度信号 -> scanDevices() 首次扫描。Docker 此时尚未启动。
MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    buildUi();//创建整个主界面
    applyStyle();//给窗口、按钮、表格、状态标签和勾选框应用QSS样式，不参与解析验证逻辑。
    QString databaseError;
    if (!m_history.open(&databaseError)) {   //打开历史数据库
        QMessageBox::critical(this, QStringLiteral("数据库错误"),
                              QStringLiteral("无法打开历史数据库：%1").arg(databaseError));
    }

    // 创建批次后台调度器。BatchRunner不直接操作界面，负责：
    // Docker/calib环境检查、最多三路解析调度、单路验证调度、进程超时与退出判断、
    // 实时日志采集以及单台/整批结果汇总；所有界面变化通过下面的信号转发。
    m_runner = new BatchRunner(this);

    // Docker或calib容器的就绪状态发生变化时触发。
    // handleEnvironmentChanged()据此刷新主界面右上角两个环境指示灯和状态栏文字。
    connect(m_runner, &BatchRunner::environmentChanged,
            this, &MainWindow::handleEnvironmentChanged);

    // 某台设备进入等待解析、正在解析、解析尾段、正在验证或已结束等阶段时触发。
    // handleDeviceState()把该设备的解析状态、验证状态和详细原因更新到批次进度窗口。
    connect(m_runner, &BatchRunner::deviceStateChanged,
            this, &MainWindow::handleDeviceState);

    // BatchRunner每产生一行[软件]流程日志或[PowerShell]原始输出时触发。
    // index表示本批设备下标，进度窗口按设备分别保存日志，打开窗口时可实时查看。
    connect(m_runner, &BatchRunner::logMessage, this,
            [this](int index, const QString &line) {
                if (m_batchDialog)
                    m_batchDialog->appendDeviceLog(index, line);
            });

    // 单台设备的解析和验证都得到最终状态后触发一次。
    // handleDeviceFinished()立即把该设备结果追加写入SQLite，不必等待整个批次结束。
    connect(m_runner, &BatchRunner::deviceFinished,
            this, &MainWindow::handleDeviceFinished);

    // Docker不可用、calib不存在、挂载错误或容器内缺少程序等批次级环境错误时触发。
    // 先把错误写到批次窗口，再弹出阻塞提示，说明本批为什么无法开始。
    connect(m_runner, &BatchRunner::fatalError, this,
            [this](const QString &message) {
                if (m_batchDialog)
                    m_batchDialog->setEnvironmentFailure(message);
                QMessageBox::critical(this, QStringLiteral("无法开始任务"), message);
            });

    // 本批所有设备均已收口，或者用户停止任务并完成清理后触发。
    // handleBatchFinished()停止进度计时、显示最终状态并重新扫描主界面历史状态。
    connect(m_runner, &BatchRunner::batchFinished,
            this, &MainWindow::handleBatchFinished);

    // 调度器开始或结束运行时触发。运行期间禁用三个启动按钮，防止重复创建批次；
    // 调度结束且仍有勾选设备时重新启用按钮。
    connect(m_runner, &BatchRunner::runningChanged, this, [this](bool running) {
        const bool enabled = !running && selectedDeviceCount() > 0;
        m_startButton->setEnabled(enabled);
        m_parseOnlyButton->setEnabled(enabled);
        m_validateOnlyButton->setEnabled(enabled);
    });
    scanDevices();//首次扫描
}

// 创建全部控件和信号槽连接。开始按钮最终调用 startSelectedBatch()，
// 勾选框变化最终调用 updateSelectionSummary()。
void MainWindow::buildUi()
{
    setWindowTitle(QStringLiteral("SLAM META 批量解析验证工具"));
    resize(1460, 900);
    setMinimumSize(1180, 720);

    auto *central = new QWidget(this);
    setCentralWidget(central);
    auto *root = new QVBoxLayout(central);
    root->setContentsMargins(24, 20, 24, 22);
    root->setSpacing(14);

    auto *header = new QHBoxLayout;
    auto *titles = new QVBoxLayout;
    auto *title = new QLabel(QStringLiteral("SLAM META 批量解析验证"), central);
    title->setObjectName(QStringLiteral("pageTitle"));
    auto *subtitle = new QLabel(
        QStringLiteral("扫描设备最新 META，选择本批设备并查看解析验证状态"), central);
    subtitle->setObjectName(QStringLiteral("pageSubtitle"));
    titles->addWidget(title);
    titles->addWidget(subtitle);
    header->addLayout(titles);
    header->addStretch();
    m_dockerStatus = new QLabel(QStringLiteral("● Docker 未检查"), central);
    m_dockerStatus->setObjectName(QStringLiteral("statusNeutral"));
    m_containerStatus = new QLabel(QStringLiteral("● calib 未检查"), central);
    m_containerStatus->setObjectName(QStringLiteral("statusNeutral"));
    header->addWidget(m_dockerStatus);
    header->addWidget(m_containerStatus);
    root->addLayout(header);

    auto *pathCard = new QFrame(central);
    pathCard->setObjectName(QStringLiteral("panel"));
    auto *pathLayout = new QHBoxLayout(pathCard);
    pathLayout->setContentsMargins(16, 14, 16, 14);
    auto *pathLabel = new QLabel(QStringLiteral("数据目录"), pathCard);
    m_dataRootEdit = new QLineEdit(QStringLiteral("E:/slam_data"), pathCard);
    auto *browseButton = new QPushButton(QStringLiteral("选择目录"), pathCard);
    auto *scanButton = new QPushButton(QStringLiteral("重新扫描"), pathCard);
    scanButton->setObjectName(QStringLiteral("primaryButton"));
    pathLayout->addWidget(pathLabel);
    pathLayout->addWidget(m_dataRootEdit, 1);
    pathLayout->addWidget(browseButton);
    pathLayout->addWidget(scanButton);
    root->addWidget(pathCard);

    auto *metrics = new QHBoxLayout;
    metrics->setSpacing(12);
    metrics->addWidget(makeMetricCard(QStringLiteral("扫描设备"), &m_deviceCountValue, central));
    metrics->addWidget(makeMetricCard(QStringLiteral("待处理"), &m_pendingCountValue, central));
    metrics->addWidget(makeMetricCard(QStringLiteral("已有结果"), &m_processedCountValue, central));
    metrics->addWidget(makeMetricCard(QStringLiteral("本批已选择"), &m_selectedCountValue, central));
    root->addLayout(metrics);

    auto *toolbar = new QHBoxLayout;
    auto *selectPendingButton = new QPushButton(QStringLiteral("全选未处理设备"), central);
    auto *clearButton = new QPushButton(QStringLiteral("取消全选"), central);
    auto *historyButton = new QPushButton(QStringLiteral("查看历史记录"), central);
    auto *exportDatabaseButton = new QPushButton(QStringLiteral("导出数据库"), central);
    auto *uploadConfigButton = new QPushButton(QStringLiteral("上传设备配置"), central);
    m_currentBatchButton = new QPushButton(QStringLiteral("查看当前批次"), central);
    m_currentBatchButton->setEnabled(false);
    m_parseOnlyButton = new QPushButton(QStringLiteral("仅解析"), central);
    m_validateOnlyButton = new QPushButton(QStringLiteral("仅验证"), central);
    m_startButton = new QPushButton(QStringLiteral("开始解析验证"), central);
    m_startButton->setObjectName(QStringLiteral("primaryButton"));
    toolbar->addWidget(selectPendingButton);
    toolbar->addWidget(clearButton);
    toolbar->addWidget(historyButton);
    toolbar->addWidget(exportDatabaseButton);
    toolbar->addWidget(uploadConfigButton);
    toolbar->addWidget(m_currentBatchButton);
    toolbar->addStretch();
    toolbar->addWidget(m_parseOnlyButton);
    toolbar->addWidget(m_validateOnlyButton);
    toolbar->addWidget(m_startButton);
    root->addLayout(toolbar);

    m_table = new QTableWidget(central);
    m_table->setColumnCount(6);
    m_table->setHorizontalHeaderLabels({QStringLiteral("选择"),
                                        QStringLiteral("SN"),
                                        QStringLiteral("最新 META"),
                                        QStringLiteral("META 时间"),
                                        QStringLiteral("最新 META 状态"),
                                        QStringLiteral("上次成功时间")});
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Fixed);
    m_table->setColumnWidth(0, 58);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    for (int column = 3; column < 6; ++column)
        m_table->horizontalHeader()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
    // 状态列使用了自定义标签，ResizeToContents 无法准确取得标签尺寸，
    // 因此给这些列留出稳定宽度，避免中文被挤压或裁切。
    m_table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Fixed);
    m_table->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Fixed);
    m_table->setColumnWidth(4, 220);
    m_table->setColumnWidth(5, 150);
    m_table->horizontalHeaderItem(2)->setTextAlignment(Qt::AlignCenter);
    m_table->verticalHeader()->setVisible(false);
    m_table->setAlternatingRowColors(true);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setShowGrid(false);
    root->addWidget(m_table, 1);

    auto *footer = new QHBoxLayout;
    m_selectionHint = new QLabel(QStringLiteral("请选择需要处理的设备"), central);
    m_selectionHint->setObjectName(QStringLiteral("mutedText"));
    auto *openDeviceButton = new QPushButton(QStringLiteral("打开设备目录"), central);
    auto *openReportButton = new QPushButton(QStringLiteral("查看验证报告"), central);
    footer->addWidget(m_selectionHint);
    footer->addStretch();
    footer->addWidget(openDeviceButton);
    footer->addWidget(openReportButton);
    root->addLayout(footer);

    connect(browseButton, &QPushButton::clicked, this, &MainWindow::chooseDataRoot);//选择目录
    connect(scanButton, &QPushButton::clicked, this, &MainWindow::scanDevices);//重新扫描
    connect(selectPendingButton, &QPushButton::clicked, this, &MainWindow::selectPendingDevices);//全选未处理设备
    connect(clearButton, &QPushButton::clicked, this, &MainWindow::clearSelection);//取消全选
    connect(historyButton, &QPushButton::clicked, this, &MainWindow::showHistoryPreview);//查看历史记录
    connect(exportDatabaseButton, &QPushButton::clicked, this, &MainWindow::exportDatabase);//导出数据库
    connect(uploadConfigButton, &QPushButton::clicked, this, &MainWindow::showConfigUploader);
    connect(m_currentBatchButton, &QPushButton::clicked, this, &MainWindow::showCurrentBatch);//查看当前批次
    connect(m_startButton, &QPushButton::clicked, this, &MainWindow::startSelectedBatch);//开始解析验证
    connect(m_parseOnlyButton, &QPushButton::clicked, this, &MainWindow::startParseOnlyBatch);//仅解析
    connect(m_validateOnlyButton, &QPushButton::clicked, this, &MainWindow::startValidateOnlyBatch);//仅验证
    connect(openDeviceButton, &QPushButton::clicked, this, &MainWindow::openSelectedDeviceDirectory);//打开设备目录
    connect(openReportButton, &QPushButton::clicked, this, &MainWindow::openSelectedReportDirectory);//查看验证报告
    //设备勾选框变化时
    connect(m_table, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *item) {
        if (item && item->column() == 0)
            updateSelectionSummary();
    });
}

// 设置界面 QSS，不读取数据，也不改变解析/验证状态。
void MainWindow::applyStyle()
{
    setStyleSheet(QStringLiteral(R"QSS(
        QMainWindow, QWidget { background:#f4f7fb; color:#172033; font-family:"Microsoft YaHei UI"; font-size:13px; }
        QLabel#pageTitle { font-size:27px; font-weight:700; color:#0f172a; }
        QLabel#pageSubtitle, QLabel#mutedText { color:#64748b; }
        QLabel#statusNeutral { background:#eef2f7; border:1px solid #d8e0ea; border-radius:16px; padding:8px 13px; color:#64748b; }
        QFrame#panel, QFrame#metricCard { background:#ffffff; border:1px solid #dfe6ef; border-radius:10px; }
        QLabel#metricValue { font-size:23px; font-weight:700; color:#1d4ed8; }
        QLineEdit { background:#ffffff; border:1px solid #cbd5e1; border-radius:7px; padding:9px 11px; selection-background-color:#2563eb; }
        QLineEdit:focus { border:2px solid #2563eb; padding:8px 10px; }
        QPushButton { background:#ffffff; border:1px solid #cbd5e1; border-radius:7px; padding:9px 15px; color:#334155; }
        QPushButton:hover { border-color:#2563eb; color:#1d4ed8; background:#eff6ff; }
        QPushButton:disabled { color:#94a3b8; background:#f1f5f9; border-color:#e2e8f0; }
        QPushButton#primaryButton { background:#2563eb; color:white; border-color:#2563eb; font-weight:600; }
        QPushButton#primaryButton:hover { background:#1d4ed8; }
        QPushButton#dangerButton { background:#fff1f2; color:#be123c; border-color:#fecdd3; }
        QTableWidget { background:#ffffff; border:1px solid #dfe6ef; border-radius:10px; alternate-background-color:#f8fafc; selection-background-color:#dbeafe; selection-color:#172033; }
        QHeaderView::section { background:#eef3f8; color:#475569; border:0; border-bottom:1px solid #d8e0ea; padding:11px 9px; font-weight:600; }
        QTableWidget::item { border-bottom:1px solid #edf1f5; padding:8px; }
        QCheckBox::indicator, QTableWidget::indicator {
            width:24px;
            height:24px;
        }
        QDialog { background:#f4f7fb; }
        QLabel#dialogTitle { font-size:23px; font-weight:700; color:#0f172a; }
        QLabel#batchBadge { background:#e0e7ff; color:#3730a3; border-radius:14px; padding:7px 12px; font-weight:600; }
        QLabel#summaryBanner { background:#eff6ff; color:#1d4ed8; border:1px solid #bfdbfe; border-radius:8px; padding:13px 16px; font-weight:600; }
        QWidget#taskPanel { background:#ffffff; border:1px solid #dfe6ef; border-radius:8px; }
        QLabel#currentTask { font-size:14px; font-weight:600; color:#1e3a8a; }
    )QSS"));
}

// 选择共享数据根目录；选择成功后立即调用 scanDevices() 重新扫描。
void MainWindow::chooseDataRoot()
{
    const QString path = QFileDialog::getExistingDirectory(
        this, QStringLiteral("选择 SLAM 数据目录"), m_dataRootEdit->text());
    if (path.isEmpty())
        return;
    m_dataRootEdit->setText(QDir::toNativeSeparators(path));
    scanDevices();
}

// 扫描调用链：枚举根目录下一层 SN 文件夹 -> 每个 SN 只取最新 *.meta
// -> HistoryDatabase::statusFor() 查询该 META 历史 -> populateTable()。
// META 身份使用“完整路径+文件大小+修改时间”
void MainWindow::scanDevices()
{
    // 1. 清空上一次扫描结果，避免旧设备继续留在主表中。
    m_devices.clear();

    // 2. 从界面取得 Windows 数据根目录，并统一成 Qt 使用的路径格式。
    const QString rootPath = QDir::fromNativeSeparators(m_dataRootEdit->text().trimmed());
    QDir root(rootPath);

    // 3. 根目录不存在：先清空表格，再显示错误并结束本次扫描。
    if (!root.exists()) {
        populateTable();
        statusBar()->showMessage(QStringLiteral("数据目录不存在：%1").arg(m_dataRootEdit->text()), 6000);
        return;
    }

    // 4. 只枚举根目录下一层文件夹；每个文件夹都被当成一个设备 SN。
    const QFileInfoList snFolders = root.entryInfoList(
        QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::IgnoreCase);

    // 5. 逐个处理 SN 文件夹，并生成一条 DeviceEntry 界面记录。
    for (const QFileInfo &folder : snFolders) {
        DeviceEntry entry;

        // 5.1 文件夹名作为 SN，绝对路径作为该设备的数据目录。
        entry.sn = folder.fileName();
        entry.deviceDir = folder.absoluteFilePath();

        // 5.2 查找当前 SN 目录下所有可读 *.meta；QDir::Time 按修改时间排序。
        QDir deviceDir(folder.absoluteFilePath());
        const QFileInfoList metaFiles = deviceDir.entryInfoList(
            QStringList() << QStringLiteral("*.meta"),
            QDir::Files | QDir::Readable, QDir::Time);

        // 5.3 没有 META 时保留设备行，但禁用勾选，状态显示“无 META”。
        if (metaFiles.isEmpty()) {
            entry.metaName = QStringLiteral("--");
            entry.metaTime = QStringLiteral("--");
            entry.latestStatus = QStringLiteral("无 META");
            entry.lastSuccessTime = QStringLiteral("--");
        } else {
            // 5.4 排序后的第一项就是该 SN 当前最新的 META，本软件只处理这一份。
            const QFileInfo latest = metaFiles.first();

            // 5.5 保存文件名、绝对路径、时间、大小和毫秒修改时间。
            //     后三项会参与数据库中的“当前具体 META”身份匹配。
            entry.metaName = latest.fileName();
            entry.metaPath = latest.absoluteFilePath();
            entry.metaTime = latest.lastModified().toString(QStringLiteral("yyyy-MM-dd HH:mm"));
            entry.metaSize = latest.size();
            entry.metaModifiedMs = latest.lastModified().toMSecsSinceEpoch();
            entry.selectable = true;

            // 5.6 用 SN + META路径/大小/修改时间查询历史处理状态。
            const MetaHistoryStatus history = m_history.statusFor(
                entry.sn, entry.metaPath, entry.metaSize, entry.metaModifiedMs);

            // 5.7 将该 SN 最近一次完全成功时间转换为主界面显示格式。
            if (history.lastSuccessAt.isEmpty()) {
                entry.lastSuccessTime = QStringLiteral("--");
            } else {
                const QDateTime successTime = QDateTime::fromString(
                    history.lastSuccessAt, Qt::ISODate);
                entry.lastSuccessTime = successTime.isValid()
                    ? successTime.toString(QStringLiteral("yyyy-MM-dd HH:mm"))
                    : history.lastSuccessAt;
            }

            // 5.8 根据“当前这个 META”的解析/验证组合结果生成文字和颜色类别。
            if (history.exactParseStatus == QStringLiteral("success")
                && history.exactValidateStatus == QStringLiteral("success")) {
                entry.processed = true;
                entry.statusKind = QStringLiteral("success");
                entry.latestStatus = combinedMetaStatus(
                    history.exactParseStatus, history.exactValidateStatus);
            } else if (history.exactParseStatus == QStringLiteral("success")
                       && history.exactValidateStatus == QStringLiteral("warning")) {
                entry.processed = true;
                entry.statusKind = QStringLiteral("warning");
                entry.latestStatus = combinedMetaStatus(
                    history.exactParseStatus, history.exactValidateStatus);
            } else if (!history.exactParseStatus.isEmpty()) {
                entry.statusKind = QStringLiteral("failed");
                entry.latestStatus = combinedMetaStatus(
                    history.exactParseStatus, history.exactValidateStatus);
            } else if (history.snHasSuccess) {
                entry.statusKind = QStringLiteral("pending");
                entry.latestStatus = QStringLiteral("新数据待处理");
            } else {
                entry.statusKind = QStringLiteral("pending");
                entry.latestStatus = QStringLiteral("未处理");
            }
        }

        // 5.9 当前 SN 处理完成，加入本次扫描结果。
        m_devices.append(entry);
    }

    // 6. 所有 SN 扫描完成，把 m_devices 写入主表并更新统计卡片。
    populateTable();

    // 7. 在状态栏显示本次发现的 SN 文件夹数量。
    statusBar()->showMessage(QStringLiteral("扫描完成：发现 %1 个设备目录").arg(m_devices.size()), 5000);
}

// 将 m_devices 填入主表，设置状态颜色，并统计待处理和已有结果数量。
// processed 仅表示当前 META 已解析成功且验证成功/警告；失败项仍可重试。
void MainWindow::populateTable()
{
    m_table->blockSignals(true);
    m_table->setRowCount(m_devices.size());
    int pending = 0;
    int processed = 0;

    for (int row = 0; row < m_devices.size(); ++row) {
        const DeviceEntry &entry = m_devices.at(row);
        auto *check = new QTableWidgetItem;
        check->setTextAlignment(Qt::AlignCenter);
        if (entry.selectable) {
            check->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable | Qt::ItemIsSelectable);
            check->setCheckState(Qt::Unchecked);
        } else {
            check->setFlags(Qt::NoItemFlags);
        }
        m_table->setItem(row, 0, check);

        auto *sn = new QTableWidgetItem(entry.sn);
        sn->setData(Qt::UserRole, entry.deviceDir);
        m_table->setItem(row, 1, sn);
        auto *meta = new QTableWidgetItem(entry.metaName);
        meta->setTextAlignment(Qt::AlignCenter);
        meta->setToolTip(entry.metaPath);
        m_table->setItem(row, 2, meta);
        m_table->setItem(row, 3, new QTableWidgetItem(entry.metaTime));

        if (!entry.selectable) {
            setStatusCell(row, 4, entry.latestStatus, QColor("#64748b"), QColor("#e2e8f0"));
        } else if (entry.statusKind == QStringLiteral("success")) {
            setStatusCell(row, 4, entry.latestStatus, QColor("#166534"), QColor("#dcfce7"));
        } else if (entry.statusKind == QStringLiteral("warning")) {
            setStatusCell(row, 4, entry.latestStatus, QColor("#854d0e"), QColor("#fef9c3"));
        } else if (entry.statusKind == QStringLiteral("failed")) {
            setStatusCell(row, 4, entry.latestStatus, QColor("#b91c1c"), QColor("#fee2e2"));
        } else {
            setStatusCell(row, 4, entry.latestStatus, QColor("#9a3412"), QColor("#ffedd5"));
        }
        m_table->setItem(row, 5, new QTableWidgetItem(entry.lastSuccessTime));
        m_table->setRowHeight(row, 50);

        if (entry.processed)
            ++processed;
        else if (entry.selectable)
            ++pending;
    }
    m_table->blockSignals(false);

    m_deviceCountValue->setText(QString::number(m_devices.size()));
    m_pendingCountValue->setText(QString::number(pending));
    m_processedCountValue->setText(QString::number(processed));
    updateSelectionSummary();
}

// 用独立 QLabel 绘制状态，避免选中整行时表格选择色覆盖红/绿状态色。
void MainWindow::setStatusCell(int row, int column, const QString &text,
                               const QColor &foreground, const QColor &background)
{
    // 使用独立标签显示状态，避免选中整行时 QTableWidget 的
    // selection-color 覆盖状态本身的红/绿/灰颜色。
    auto *container = new QWidget(m_table);
    container->setStyleSheet(QStringLiteral("background:transparent;"));
    auto *layout = new QHBoxLayout(container);
    layout->setContentsMargins(6, 4, 6, 4);

    auto *label = new QLabel(text, container);
    label->setAlignment(Qt::AlignCenter);
    label->setMinimumHeight(30);
    label->setStyleSheet(QStringLiteral(
        "QLabel { color:%1; background:%2; border-radius:9px; "
        "padding:0px 10px; font-weight:600; }")
        .arg(foreground.name(), background.name()));
    layout->addWidget(label);
    m_table->setCellWidget(row, column, container);
}

// 勾选所有存在 META 且当前最新 META 尚未成功处理的设备。
void MainWindow::selectPendingDevices()
{
    m_table->blockSignals(true);
    for (int row = 0; row < m_devices.size(); ++row) {
        if (m_devices.at(row).selectable && !m_devices.at(row).processed)
            m_table->item(row, 0)->setCheckState(Qt::Checked);
    }
    m_table->blockSignals(false);
    updateSelectionSummary();
}

// 清空界面勾选，不删除 META、输出文件或数据库记录。
void MainWindow::clearSelection()
{
    m_table->blockSignals(true);
    for (int row = 0; row < m_devices.size(); ++row) {
        if (m_devices.at(row).selectable)
            m_table->item(row, 0)->setCheckState(Qt::Unchecked);
    }
    m_table->blockSignals(false);
    updateSelectionSummary();
}

// 以表格第一列的实时勾选状态统计本批设备数。
int MainWindow::selectedDeviceCount() const
{
    int count = 0;
    for (int row = 0; row < m_devices.size(); ++row) {
        const QTableWidgetItem *item = m_table->item(row, 0);
        if (item && item->checkState() == Qt::Checked)
            ++count;
    }
    return count;
}

// 勾选改变后的统一刷新入口：更新数量、提示文字和开始按钮可用状态。
void MainWindow::updateSelectionSummary()
{
    const int count = selectedDeviceCount();
    m_selectedCountValue->setText(QString::number(count));
    m_selectionHint->setText(count == 0
        ? QStringLiteral("请选择需要处理的设备")
        : QStringLiteral("本批已选择 %1 台设备").arg(count));
    const bool enabled = count > 0 && (!m_runner || !m_runner->isRunning());
    m_startButton->setEnabled(enabled);
    m_parseOnlyButton->setEnabled(enabled);
    m_validateOnlyButton->setEnabled(enabled);
}

// 将勾选设备复制成固定 BatchDevice 快照。运行开始后不再从表格取路径。
QVector<BatchDevice> MainWindow::selectedBatchSnapshot() const
{
    QVector<BatchDevice> result;
    for (int row = 0; row < m_devices.size(); ++row) {
        const QTableWidgetItem *item = m_table->item(row, 0);
        if (!item || item->checkState() != Qt::Checked)
            continue;
        const DeviceEntry &entry = m_devices.at(row);
        BatchDevice device;
        device.sn = entry.sn;
        device.metaName = entry.metaName;
        device.metaPath = entry.metaPath;
        device.deviceDir = entry.deviceDir;
        device.metaSize = entry.metaSize;
        device.metaModifiedMs = entry.metaModifiedMs;
        result.append(device);
    }
    return result;
}

// 用户点击开始后的总入口：检查重复运行 -> 固化设备快照 -> 初始化进度窗口
// -> 生成批次号 -> 调用 BatchRunner::start() 进入 Docker 环境检查。
void MainWindow::startSelectedBatch()
{
    startBatch(BatchMode::ParseAndValidate);
}

void MainWindow::startParseOnlyBatch()
{
    startBatch(BatchMode::ParseOnly);
}

void MainWindow::startValidateOnlyBatch()
{
    startBatch(BatchMode::ValidateOnly);
}

// 三个开始按钮的统一入口。mode 决定执行“解析并验证”“仅解析”或“仅验证”。
void MainWindow::startBatch(BatchMode mode)
{
    // 1. 同一时间只允许一个批次运行。
    if (m_runner && m_runner->isRunning()) {
        QMessageBox::information(this, QStringLiteral("任务正在运行"),
                                 QStringLiteral("同一时间只能运行一个批次。"));
        return;
    }
    if (m_uploadDialog && m_uploadDialog->isRunning()) {
        QMessageBox::information(this, QStringLiteral("配置正在上传"),
                                 QStringLiteral("请等待配置上传批次结束后再启动解析验证。"));
        return;
    }
    // 2. 把当前勾选行复制成固定 BatchDevice 快照。
    const QVector<BatchDevice> devices = selectedBatchSnapshot();

    // 3. 没有有效设备时不创建批次。
    if (devices.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("没有选择设备"),
                                 QStringLiteral("请先勾选需要处理的设备。"));
        return;
    }

    // 4. 进度窗口只创建一次；停止按钮通过 stopRequested 调用 BatchRunner::stop()。
    if (!m_batchDialog) {
        m_batchDialog = new BatchProgressDialog(this);
        connect(m_batchDialog, &BatchProgressDialog::stopRequested,
                m_runner, &BatchRunner::stop);
    }

    // 5. 生成批次号并将本批设备交给进度窗口初始化。
    const QString batchId = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    m_activeBatch = devices;
    m_batchDialog->setBatch(devices, batchId, mode);
    // 6. 显示进度窗口，使用户能够查看设备状态和实时任务日志。
    m_batchDialog->show();
    m_batchDialog->raise();
    m_batchDialog->activateWindow();
    m_currentBatchButton->setEnabled(true);
    statusBar()->showMessage(QStringLiteral("批次已启动，正在检查 Docker 环境"), 6000);
    // 7. 调用 BatchRunner::start()；它先检查 Docker，而不是直接运行解析。
    m_runner->start(devices, batchId,
                    QDir::fromNativeSeparators(m_dataRootEdit->text().trimmed()), mode);
}

// 重新显示现有批次窗口，不创建任务，也不会重复调用 BatchRunner::start()。
void MainWindow::showCurrentBatch()
{
    if (!m_batchDialog)
        return;
    m_batchDialog->show();
    m_batchDialog->raise();
    m_batchDialog->activateWindow();
}

// 安全取得当前行数据；越界时返回空对象，供打开目录等函数直接忽略。
MainWindow::DeviceEntry MainWindow::deviceAtRow(int row) const
{
    return row >= 0 && row < m_devices.size() ? m_devices.at(row) : DeviceEntry{};
}

// 调用系统文件管理器打开当前行的 SN 目录。
void MainWindow::openSelectedDeviceDirectory()
{
    const DeviceEntry entry = deviceAtRow(m_table->currentRow());
    if (entry.deviceDir.isEmpty())
        return;
    QDesktopServices::openUrl(QUrl::fromLocalFile(entry.deviceDir));
}

// 打开当前 SN/evaluate_output；不存在时只提示，不自动生成报告。
void MainWindow::openSelectedReportDirectory()
{
    const DeviceEntry entry = deviceAtRow(m_table->currentRow());
    if (entry.deviceDir.isEmpty())
        return;
    const QString reportDir = QDir(entry.deviceDir).filePath(QStringLiteral("evaluate_output"));
    if (!QDir(reportDir).exists()) {
        QMessageBox::information(this, QStringLiteral("暂无报告"),
                                 QStringLiteral("当前设备还没有 evaluate_output 目录。"));
        return;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(reportDir));
}

// 显示只读历史窗口，并支持按完整或部分SN查询数据库记录。
void MainWindow::showHistoryPreview()
{
    auto *dialog = new QDialog(this);
    dialog->setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(QStringLiteral("历史记录"));
    dialog->resize(1100, 650);
    auto *layout = new QVBoxLayout(dialog);
    auto *path = new QLabel(QStringLiteral("数据库：%1").arg(m_history.databasePath()), dialog);
    path->setObjectName(QStringLiteral("mutedText"));
    layout->addWidget(path);

    auto *searchLayout = new QHBoxLayout;
    auto *historyType = new QComboBox(dialog);
    historyType->addItem(QStringLiteral("解析验证历史"));
    historyType->addItem(QStringLiteral("配置上传历史"));
    auto *snSearch = new QLineEdit(dialog);
    snSearch->setPlaceholderText(QStringLiteral("输入完整或部分SN号"));
    snSearch->setClearButtonEnabled(true);
    auto *searchButton = new QPushButton(QStringLiteral("查询"), dialog);
    auto *resetButton = new QPushButton(QStringLiteral("显示最近记录"), dialog);
    auto *resultLabel = new QLabel(dialog);
    resultLabel->setObjectName(QStringLiteral("mutedText"));
    searchLayout->addWidget(new QLabel(QStringLiteral("记录类型"), dialog));
    searchLayout->addWidget(historyType);
    searchLayout->addWidget(new QLabel(QStringLiteral("SN查询"), dialog));
    searchLayout->addWidget(snSearch, 1);
    searchLayout->addWidget(searchButton);
    searchLayout->addWidget(resetButton);
    searchLayout->addWidget(resultLabel);
    layout->addLayout(searchLayout);

    auto *table = new QTableWidget(dialog);
    table->setColumnCount(7);
    const auto showRows = [table, resultLabel](const QVector<QStringList> &rows,
                                               bool uploadHistory) {
        table->setHorizontalHeaderLabels(uploadHistory
            ? QStringList{QStringLiteral("完成时间"), QStringLiteral("SN"),
                          QStringLiteral("配置文件"), QStringLiteral("上传状态"),
                          QStringLiteral("请求ID"), QStringLiteral("失败原因"),
                          QStringLiteral("批次号")}
            : QStringList{QStringLiteral("完成时间"), QStringLiteral("SN"),
                          QStringLiteral("META"), QStringLiteral("解析"),
                          QStringLiteral("验证"), QStringLiteral("警告数"),
                          QStringLiteral("失败原因")});
        table->clearContents();
        table->setRowCount(rows.size());
        for (int row = 0; row < rows.size(); ++row) {
            for (int column = 0; column < rows.at(row).size(); ++column) {
                QString value = rows.at(row).at(column);
                if (column == 0) {
                    const QDateTime time = QDateTime::fromString(value, Qt::ISODate);
                    if (time.isValid())
                        value = time.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
                } else if (column == 3 || (!uploadHistory && column == 4)) {
                    if (value == QStringLiteral("success")) value = QStringLiteral("成功");
                    else if (value == QStringLiteral("warning")) value = QStringLiteral("警告");
                    else if (value == QStringLiteral("failed")) value = QStringLiteral("失败");
                    else if (value == QStringLiteral("timeout")) value = QStringLiteral("超时");
                    else if (value == QStringLiteral("skipped")) value = QStringLiteral("已跳过");
                    else if (value == QStringLiteral("interrupted")) value = QStringLiteral("已中断");
                    else if (value == QStringLiteral("not_run")) value = QStringLiteral("未执行");
                }
                table->setItem(row, column, new QTableWidgetItem(value));
            }
        }
        resultLabel->setText(QStringLiteral("共 %1 条").arg(rows.size()));
    };

    const auto search = [this, snSearch, historyType, showRows] {
        const bool uploadHistory = historyType->currentIndex() == 1;
        showRows(uploadHistory
                     ? m_history.uploadHistoryBySn(snSearch->text())
                     : m_history.historyBySn(snSearch->text()),
                 uploadHistory);
    };
    connect(searchButton, &QPushButton::clicked, dialog, search);
    connect(snSearch, &QLineEdit::returnPressed, dialog, search);
    connect(resetButton, &QPushButton::clicked, dialog,
            [this, snSearch, historyType, showRows] {
                snSearch->clear();
                const bool uploadHistory = historyType->currentIndex() == 1;
                showRows(uploadHistory ? m_history.recentUploadHistory()
                                       : m_history.recentHistory(),
                         uploadHistory);
            });
    connect(historyType, QOverload<int>::of(&QComboBox::currentIndexChanged),
            dialog, [search](int) { search(); });

    showRows(m_history.recentHistory(), false);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(6, QHeaderView::Stretch);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    layout->addWidget(table);
    dialog->show();
}

// 将当前SQLite历史库导出到数据根目录。默认数据根目录为E:/slam_data，
// 因此默认输出E:/slam_data/slam_meta_history.sqlite；同名文件会被覆盖。
void MainWindow::exportDatabase()
{
    if (m_runner && m_runner->isRunning()) {
        QMessageBox::information(this, QStringLiteral("任务正在运行"),
                                 QStringLiteral("请等待当前批次结束后再导出数据库。"));
        return;
    }

    const QString dataRoot = QDir::cleanPath(
        QDir::fromNativeSeparators(m_dataRootEdit->text().trimmed()));
    if (dataRoot.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("导出失败"),
                             QStringLiteral("数据目录不能为空。"));
        return;
    }

    const QString destination = QDir(dataRoot).filePath(
        QStringLiteral("slam_meta_history.sqlite"));
    QString error;
    if (!m_history.exportTo(destination, &error)) {
        QMessageBox::critical(this, QStringLiteral("导出失败"), error);
        return;
    }

    QMessageBox::information(this, QStringLiteral("导出成功"),
                             QStringLiteral("数据库已导出到：\n%1")
                                 .arg(QDir::toNativeSeparators(destination)));
}

void MainWindow::showConfigUploader()
{
    // 配置上传需要切换电脑Wi-Fi并独占任务入口，因此不允许与解析验证批次同时运行。
    if (m_runner && m_runner->isRunning()) {
        QMessageBox::information(this, QStringLiteral("任务正在运行"),
                                 QStringLiteral("请等待当前解析验证批次结束后再上传配置。"));
        return;
    }
    if (m_uploadDialog) {
        m_uploadDialog->raise();
        m_uploadDialog->activateWindow();
        return;
    }

    // 上传文件固定来自当前数据根目录/all_data/<SN>.json，并共用解析验证历史数据库。
    const QString allDataPath = QDir(m_dataRootEdit->text().trimmed())
                                    .filePath(QStringLiteral("all_data"));
    m_uploadDialog = new ConfigUploadDialog(allDataPath, &m_history, this);
    m_uploadDialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(m_uploadDialog, &QObject::destroyed, this, [this] {
        m_uploadDialog = nullptr;
    });
    connect(m_uploadDialog, &ConfigUploadDialog::uploadRunningChanged,
            this, [this](bool running) {
                // MQTT上传期间禁用三个解析验证入口，上传结束后按当前勾选状态恢复。
                const bool enabled = !running && (!m_runner || !m_runner->isRunning())
                                     && selectedDeviceCount() > 0;
                m_startButton->setEnabled(enabled);
                m_parseOnlyButton->setEnabled(enabled);
                m_validateOnlyButton->setEnabled(enabled);
            });
    m_uploadDialog->show();
}

// 接收 BatchRunner::environmentChanged，刷新 Docker/calib 指示灯和状态栏。
void MainWindow::handleEnvironmentChanged(bool dockerReady, bool containerReady,
                                          const QString &message)
{
    m_dockerStatus->setText(dockerReady ? QStringLiteral("● Docker 已就绪")
                                        : QStringLiteral("● Docker 未就绪"));
    m_containerStatus->setText(containerReady ? QStringLiteral("● calib 已就绪")
                                              : QStringLiteral("● calib 未就绪"));
    const QString readyStyle = QStringLiteral(
        "background:#dcfce7;color:#166534;border:1px solid #86efac;"
        "border-radius:16px;padding:8px 13px;");
    const QString idleStyle = QStringLiteral(
        "background:#eef2f7;color:#64748b;border:1px solid #d8e0ea;"
        "border-radius:16px;padding:8px 13px;");
    m_dockerStatus->setStyleSheet(dockerReady ? readyStyle : idleStyle);
    m_containerStatus->setStyleSheet(containerReady ? readyStyle : idleStyle);
    statusBar()->showMessage(message, 8000);
}

// 接收单台流程状态并转发到 BatchProgressDialog::updateDeviceState()。
void MainWindow::handleDeviceState(int index, const QString &stage,
                                   const QString &parseStatus,
                                   const QString &validateStatus,
                                   const QString &detail)
{
    if (m_batchDialog)
        m_batchDialog->updateDeviceState(index, stage, parseStatus, validateStatus, detail);
}

// 每台设备结束后立即调用 saveResult() 追加历史，不等待整个批次完成。
void MainWindow::handleDeviceFinished(int, const DeviceRunResult &result)
{
    QString error;
    if (!m_history.saveResult(result, &error))
        statusBar()->showMessage(QStringLiteral("历史记录保存失败：%1").arg(error), 10000);
}

// 批次收尾：更新进度窗口，然后重新 scanDevices()，使主表显示最新数据库状态。
void MainWindow::handleBatchFinished(bool cancelled)
{
    if (m_batchDialog)
        m_batchDialog->setBatchFinished(cancelled);
    statusBar()->showMessage(cancelled ? QStringLiteral("本批任务已停止")
                                       : QStringLiteral("本批解析验证已完成"), 10000);
    scanDevices();
    if (m_closeAfterStop) {
        m_closeAfterStop = false;
        QTimer::singleShot(0, this, &QWidget::close);
    }
}

// 运行中关闭窗口会先询问；确认后调用 BatchRunner::stop()，等清理完成再关闭。
void MainWindow::closeEvent(QCloseEvent *event)
{
    if (m_uploadDialog && m_uploadDialog->isRunning()) {
        m_uploadDialog->raise();
        m_uploadDialog->activateWindow();
        QMessageBox::information(this, QStringLiteral("配置正在上传"),
                                 QStringLiteral("请先在配置上传窗口中停止上传任务。"));
        event->ignore();
        return;
    }
    if (m_runner && m_runner->isRunning()) {
        const auto answer = QMessageBox::question(
            this, QStringLiteral("任务仍在运行"),
            QStringLiteral("关闭软件会终止当前任务。确定关闭吗？"));
        if (answer != QMessageBox::Yes) {
            event->ignore();
            return;
        }
        m_closeAfterStop = true;
        m_runner->stop();
        event->ignore();
        return;
    }
    event->accept();
}

