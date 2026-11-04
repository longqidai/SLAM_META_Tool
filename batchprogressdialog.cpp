#include "batchprogressdialog.h"

#include <QAbstractItemView>
#include <QColor>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollBar>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTextCursor>
#include <QTextEdit>
#include <QUrl>
#include <QVBoxLayout>

// 创建非模态批次窗口以及按钮连接。stopRequested 最终连接到
// BatchRunner::stop()；运行时间计时器每秒调用 updateElapsedTime()。
BatchProgressDialog::BatchProgressDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    setWindowTitle(QStringLiteral("本批任务进度"));
    resize(1040, 610);
    setModal(false);
    setAttribute(Qt::WA_DeleteOnClose, false);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(24, 22, 24, 22);
    root->setSpacing(16);

    auto *titleRow = new QHBoxLayout;
    auto *title = new QLabel(QStringLiteral("本批任务进度"), this);
    title->setObjectName(QStringLiteral("dialogTitle"));
    m_batchLabel = new QLabel(this);
    m_batchLabel->setObjectName(QStringLiteral("batchBadge"));
    titleRow->addWidget(title);
    titleRow->addStretch();
    titleRow->addWidget(m_batchLabel);
    root->addLayout(titleRow);

    m_summaryLabel = new QLabel(this);
    m_summaryLabel->setObjectName(QStringLiteral("summaryBanner"));
    root->addWidget(m_summaryLabel);

    m_table = new QTableWidget(this);
    m_table->setColumnCount(5);
    m_table->setHorizontalHeaderLabels({QStringLiteral("SN"),
                                        QStringLiteral("最新 META"),
                                        QStringLiteral("当前阶段"),
                                        QStringLiteral("解析结果"),
                                        QStringLiteral("验证结果")});
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Fixed);
    m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Fixed);
    m_table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Fixed);
    m_table->setColumnWidth(2, 130);
    m_table->setColumnWidth(3, 120);
    m_table->setColumnWidth(4, 120);
    m_table->verticalHeader()->setVisible(false);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setAlternatingRowColors(true);
    root->addWidget(m_table, 1);

    auto *taskPanel = new QWidget(this);
    taskPanel->setObjectName(QStringLiteral("taskPanel"));
    auto *taskLayout = new QVBoxLayout(taskPanel);
    taskLayout->setContentsMargins(16, 12, 16, 12);
    m_currentTaskLabel = new QLabel(QStringLiteral("当前任务：等待开始"), taskPanel);
    m_currentTaskLabel->setObjectName(QStringLiteral("currentTask"));
    m_runtimeLabel = new QLabel(QStringLiteral("运行时间：00:00:00    预计剩余：--"), taskPanel);
    m_runtimeLabel->setObjectName(QStringLiteral("mutedText"));
    taskLayout->addWidget(m_currentTaskLabel);
    taskLayout->addWidget(m_runtimeLabel);
    root->addWidget(taskPanel);

    auto *buttons = new QHBoxLayout;
    auto *logButton = new QPushButton(QStringLiteral("查看当前日志"), this);
    auto *openButton = new QPushButton(QStringLiteral("打开当前设备目录"), this);
    m_stopButton = new QPushButton(QStringLiteral("停止本批任务"), this);
    m_stopButton->setObjectName(QStringLiteral("dangerButton"));
    auto *hideButton = new QPushButton(QStringLiteral("隐藏窗口"), this);
    buttons->addWidget(logButton);
    buttons->addWidget(openButton);
    buttons->addStretch();
    buttons->addWidget(m_stopButton);
    buttons->addWidget(hideButton);
    root->addLayout(buttons);

    connect(logButton, &QPushButton::clicked,
            this, &BatchProgressDialog::showCurrentLogHint);
    connect(openButton, &QPushButton::clicked,
            this, &BatchProgressDialog::openSelectedDeviceDirectory);
    connect(m_stopButton, &QPushButton::clicked, this, [this] {
        if (m_finished)
            return;
        const auto answer = QMessageBox::question(
            this, QStringLiteral("停止任务"),
            QStringLiteral("将终止当前解析或验证，并取消本批次后续设备。\n\n确定停止吗？"));
        if (answer == QMessageBox::Yes)
            emit stopRequested();
    });
    connect(hideButton, &QPushButton::clicked, this, &QDialog::hide);
    m_elapsedTimer.setInterval(1000);
    connect(&m_elapsedTimer, &QTimer::timeout,
            this, &BatchProgressDialog::updateElapsedTime);
}

// 新批次初始化：复制设备列表、清空行状态/内存日志、全部设为等待，
// 重启运行计时。这里只准备界面，不启动 Docker。
void BatchProgressDialog::setBatch(const QVector<BatchDevice> &devices,
                                   const QString &batchId, BatchMode mode)
{
    m_mode = mode;
    m_devices = devices;
    m_states.clear();
    m_states.resize(devices.size());
    m_logs.clear();
    m_logs.resize(devices.size());
    m_finished = false;
    m_environmentFailed = false;
    m_stopButton->setEnabled(true);
    const QString modeText = mode == BatchMode::ParseOnly
        ? QStringLiteral("仅解析")
        : (mode == BatchMode::ValidateOnly
            ? QStringLiteral("仅验证") : QStringLiteral("解析并验证"));
    m_batchLabel->setText(QStringLiteral("批次号  %1  ·  %2").arg(batchId, modeText));
    m_table->setRowCount(devices.size());

    for (int row = 0; row < devices.size(); ++row) {
        const BatchDevice &device = devices.at(row);
        auto *sn = new QTableWidgetItem(device.sn);
        sn->setData(Qt::UserRole, device.deviceDir);
        auto *meta = new QTableWidgetItem(device.metaName);
        meta->setToolTip(device.metaPath);
        m_table->setItem(row, 0, sn);
        m_table->setItem(row, 1, meta);
        const QString initialStage = mode == BatchMode::ValidateOnly
            ? QStringLiteral("等待验证") : QStringLiteral("等待解析");
        const QString initialParse = mode == BatchMode::ValidateOnly
            ? QStringLiteral("未执行") : QStringLiteral("等待");
        const QString initialValidate = mode == BatchMode::ParseOnly
            ? QStringLiteral("未执行") : QStringLiteral("等待");
        setStatusItem(row, 2, initialStage,
                      QColor(QStringLiteral("#9a3412")), QColor(QStringLiteral("#ffedd5")));
        setStatusItem(row, 3, initialParse,
                      QColor(QStringLiteral("#475569")), QColor(QStringLiteral("#f1f5f9")));
        setStatusItem(row, 4, initialValidate,
                      QColor(QStringLiteral("#475569")), QColor(QStringLiteral("#f1f5f9")));
        m_states[row] = {initialStage, initialParse, initialValidate, QString()};
        m_table->setRowHeight(row, 50);
    }

    if (!devices.isEmpty()) {
        m_table->selectRow(0);
        m_currentTaskLabel->setText(
            QStringLiteral("当前任务：%1 %2")
                .arg(devices.first().sn,
                     mode == BatchMode::ValidateOnly
                         ? QStringLiteral("等待开始验证") : QStringLiteral("等待开始解析")));
    } else {
        m_currentTaskLabel->setText(QStringLiteral("当前任务：无"));
    }
    m_elapsed.restart();
    m_elapsedTimer.start();
    updateElapsedTime();
    updateSummary();
}

// 根据各行中文状态统计总完成、解析完成、验证完成和失败数量。
void BatchProgressDialog::updateSummary()
{
    int completed = 0;
    int parseCompleted = 0;
    int validateCompleted = 0;
    int failed = 0;
    for (const RowState &state : m_states) {
        const bool parseDone = state.parseStatus == QStringLiteral("成功")
            || state.parseStatus == QStringLiteral("失败")
            || state.parseStatus == QStringLiteral("未执行")
            || state.parseStatus == QStringLiteral("已取消");
        const bool validateDone = state.validateStatus == QStringLiteral("成功")
            || state.validateStatus == QStringLiteral("警告")
            || state.validateStatus == QStringLiteral("失败")
            || state.validateStatus == QStringLiteral("已跳过")
            || state.validateStatus == QStringLiteral("未执行")
            || state.validateStatus == QStringLiteral("已取消");
        if (parseDone && state.parseStatus != QStringLiteral("未执行"))
            ++parseCompleted;
        if (validateDone && state.validateStatus != QStringLiteral("已跳过")
            && state.validateStatus != QStringLiteral("未执行"))
            ++validateCompleted;
        if ((m_mode == BatchMode::ParseOnly && parseDone)
            || (m_mode == BatchMode::ValidateOnly && validateDone)
            || (m_mode == BatchMode::ParseAndValidate && parseDone && validateDone))
            ++completed;
        if (state.parseStatus == QStringLiteral("失败")
            || state.validateStatus == QStringLiteral("失败"))
            ++failed;
    }
    m_summaryLabel->setText(QStringLiteral(
        "总完成  %1/%2     解析完成  %3/%2     验证完成  %4     失败  %5")
        .arg(completed).arg(m_devices.size()).arg(parseCompleted)
        .arg(validateCompleted).arg(failed));
}

// BatchRunner::deviceStateChanged 的最终界面入口：更新指定行三个状态列，
// 选择当前设备，显示详情并重新统计汇总。
void BatchProgressDialog::updateDeviceState(int index, const QString &stage,
                                            const QString &parseStatus,
                                            const QString &validateStatus,
                                            const QString &detail)
{
    if (index < 0 || index >= m_states.size())
        return;
    RowState &state = m_states[index];
    state = {stage, parseStatus, validateStatus, detail};
    QColor foreground;
    QColor background;
    statusColors(stage, &foreground, &background);
    setStatusItem(index, 2, stage, foreground, background);
    statusColors(parseStatus, &foreground, &background);
    setStatusItem(index, 3, parseStatus, foreground, background);
    statusColors(validateStatus, &foreground, &background);
    setStatusItem(index, 4, validateStatus, foreground, background);
    m_table->selectRow(index);
    m_currentTaskLabel->setText(detail.isEmpty()
        ? QStringLiteral("当前任务：%1  %2").arg(m_devices.at(index).sn, stage)
        : QStringLiteral("当前任务：%1  %2；%3").arg(m_devices.at(index).sn, stage, detail));
    updateSummary();
}

// 接收 BatchRunner::logMessage，把日志加入界面内存；磁盘写入由
// BatchRunner::appendLog() 独立完成。
void BatchProgressDialog::appendDeviceLog(int index, const QString &line)
{
    if (index >= 0 && index < m_logs.size())
        m_logs[index].append(line);
}

// 收到 batchFinished 后停止计时、禁用停止按钮并显示完成/取消结果。
void BatchProgressDialog::setBatchFinished(bool cancelled)
{
    m_finished = true;
    m_elapsedTimer.stop();
    m_stopButton->setEnabled(false);
    if (m_environmentFailed) {
        // 环境失败原因已经由 setEnvironmentFailure 显示，不覆盖成“已完成”。
    } else if (cancelled) {
        for (int i = 0; i < m_states.size(); ++i) {
            if (m_states.at(i).stage.startsWith(QStringLiteral("等待")))
                updateDeviceState(i, QStringLiteral("已取消"), QStringLiteral("已取消"),
                                  QStringLiteral("已取消"), QStringLiteral("批次已停止"));
        }
        m_currentTaskLabel->setText(QStringLiteral("本批任务已停止"));
    } else {
        m_currentTaskLabel->setText(QStringLiteral("本批任务已完成"));
    }
    updateSummary();
}

// Docker环境失败时把本批所有行设为未执行/失败/跳过，并保留具体原因。
void BatchProgressDialog::setEnvironmentFailure(const QString &message)
{
    m_environmentFailed = true;
    for (int i = 0; i < m_states.size(); ++i) {
        updateDeviceState(i, QStringLiteral("未执行"), QStringLiteral("失败"),
                          QStringLiteral("已跳过"), message);
    }
    m_currentTaskLabel->setText(QStringLiteral("环境检查失败：%1").arg(message));
}

// 每秒根据 QElapsedTimer 生成 HH:mm:ss，只显示批次已运行时间。
void BatchProgressDialog::updateElapsedTime()
{
    const qint64 seconds = m_elapsed.isValid() ? m_elapsed.elapsed() / 1000 : 0;
    const QString text = QStringLiteral("%1:%2:%3")
        .arg(seconds / 3600, 2, 10, QLatin1Char('0'))
        .arg((seconds / 60) % 60, 2, 10, QLatin1Char('0'))
        .arg(seconds % 60, 2, 10, QLatin1Char('0'));
    const QString scheduleText = m_mode == BatchMode::ParseOnly
        ? QStringLiteral("仅执行解析")
        : (m_mode == BatchMode::ValidateOnly
            ? QStringLiteral("仅执行验证")
            : QStringLiteral("全部解析完成后，再按顺序验证"));
    m_runtimeLabel->setText(QStringLiteral("运行时间：%1    %2")
                                .arg(text, scheduleText));
}

// 把状态文本映射成绿/红/黄/蓝/橙/灰配色。
void BatchProgressDialog::statusColors(const QString &status, QColor *foreground,
                                       QColor *background) const
{
    if (status == QStringLiteral("成功") || status == QStringLiteral("已完成")) {
        *foreground = QColor(QStringLiteral("#166534"));
        *background = QColor(QStringLiteral("#dcfce7"));
    } else if (status == QStringLiteral("失败") || status == QStringLiteral("已结束")
               || status == QStringLiteral("未执行")) {
        *foreground = QColor(QStringLiteral("#b91c1c"));
        *background = QColor(QStringLiteral("#fee2e2"));
    } else if (status == QStringLiteral("警告")) {
        *foreground = QColor(QStringLiteral("#854d0e"));
        *background = QColor(QStringLiteral("#fef9c3"));
    } else if (status.contains(QStringLiteral("正在")) || status == QStringLiteral("进行中")) {
        *foreground = QColor(QStringLiteral("#1d4ed8"));
        *background = QColor(QStringLiteral("#dbeafe"));
    } else if (status.contains(QStringLiteral("等待"))) {
        *foreground = QColor(QStringLiteral("#9a3412"));
        *background = QColor(QStringLiteral("#ffedd5"));
    } else {
        *foreground = QColor(QStringLiteral("#475569"));
        *background = QColor(QStringLiteral("#f1f5f9"));
    }
}

// 使用独立 QLabel 绘制胶囊状态，避免表格选中颜色覆盖状态色。
void BatchProgressDialog::setStatusItem(int row, int column, const QString &text,
                                        const QColor &foreground,
                                        const QColor &background)
{
    // 状态颜色不能随着当前选中行一起被表格选择色覆盖。
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

// 使用系统文件管理器打开进度表当前设备目录。
void BatchProgressDialog::openSelectedDeviceDirectory()
{
    const int row = m_table->currentRow();
    if (row < 0 || row >= m_devices.size())
        return;
    QDesktopServices::openUrl(QUrl::fromLocalFile(m_devices.at(row).deviceDir));
}

// 打开当前设备日志窗口。初次加载已有内存日志，再由局部 QTimer 每秒
// 追加新行；位于底部时自动跟随，用户上翻后保留原滚动位置。
void BatchProgressDialog::showCurrentLogHint()
{
    const int row = m_table->currentRow();
    if (row < 0 || row >= m_devices.size())
        return;
    auto *dialog = new QDialog(this);
    dialog->setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(QStringLiteral("%1 任务日志").arg(m_devices.at(row).sn));
    dialog->resize(900, 600);
    auto *layout = new QVBoxLayout(dialog);
    auto *text = new QTextEdit(dialog);
    text->setReadOnly(true);
    text->setPlainText(m_logs.at(row).join(QLatin1Char('\n')));
    text->setProperty("displayedLineCount", m_logs.at(row).size());
    layout->addWidget(text);

    // The log viewer used to contain only a snapshot taken when it was opened.
    // Refresh it once per second so long calibration output becomes visible
    // without closing and reopening the window.
    const QString deviceSn = m_devices.at(row).sn;
    auto *refreshTimer = new QTimer(dialog);
    refreshTimer->setInterval(1000);
    connect(refreshTimer, &QTimer::timeout, dialog,
            [this, row, deviceSn, text, refreshTimer] {
        if (row < 0 || row >= m_devices.size()
            || m_devices.at(row).sn != deviceSn) {
            refreshTimer->stop();
            return;
        }

        const QStringList &lines = m_logs.at(row);
        int displayed = text->property("displayedLineCount").toInt();
        if (displayed > lines.size()) {
            text->setPlainText(lines.join(QLatin1Char('\n')));
            text->setProperty("displayedLineCount", lines.size());
            text->verticalScrollBar()->setValue(text->verticalScrollBar()->maximum());
            return;
        }
        if (displayed == lines.size())
            return;

        QScrollBar *scrollBar = text->verticalScrollBar();
        const int oldScrollValue = scrollBar->value();
        const bool followTail = oldScrollValue >= scrollBar->maximum() - 2;

        QTextCursor cursor(text->document());
        cursor.movePosition(QTextCursor::End);
        if (displayed > 0)
            cursor.insertText(QStringLiteral("\n"));
        cursor.insertText(lines.mid(displayed).join(QLatin1Char('\n')));
        text->setProperty("displayedLineCount", lines.size());

        if (followTail)
            scrollBar->setValue(scrollBar->maximum());
        else
            scrollBar->setValue(oldScrollValue);
    });
    refreshTimer->start();
    dialog->show();
}

