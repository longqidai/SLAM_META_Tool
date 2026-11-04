#include "configuploaddialog.h"

#include "historydatabase.h"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QCloseEvent>
#include <QColor>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QFileInfoList>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTextEdit>
#include <QVBoxLayout>

ConfigUploadDialog::ConfigUploadDialog(const QString &allDataPath,
                                       HistoryDatabase *history,
                                       QWidget *parent)
    : QDialog(parent), m_allDataPath(allDataPath), m_history(history),
      m_uploader(new ConfigUploader(this))
{
    // 上传窗口只负责文件选择、状态展示和数据库落库；网络状态机由ConfigUploader负责。
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    buildUi();
    connect(m_uploader, &ConfigUploader::logMessage,
            m_log, &QTextEdit::append);
    connect(m_uploader, &ConfigUploader::itemStateChanged,
            this, [this](int index, const QString &state, const QString &detail) {
                // uploader的index对应本次固定上传快照，再映射回扫描表格的原始行号。
                if (index >= 0 && index < m_activeRows.size())
                    updateRowState(m_activeRows.at(index), state, detail);
            });
    connect(m_uploader, &ConfigUploader::itemFinished,
            this, [this](int index, const ConfigUploadResult &result) {
                // 单台成功、失败或中止都立即追加到共用SQLite上传历史表。
                QString error;
                if (!m_history->saveUploadResult(result, &error))
                    m_log->append(QStringLiteral("[软件] 上传历史保存失败：%1").arg(error));
                if (index >= 0 && index < m_activeRows.size()
                    && result.status == QStringLiteral("success")) {
                    // 成功后立即显示本次成功时间并取消勾选，避免窗口内重复上传。
                    const int row = m_activeRows.at(index);
                    m_table->item(row, 3)->setText(
                        QDateTime::fromString(result.finishedAt, Qt::ISODate)
                            .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
                    m_table->item(row, 3)->setData(Qt::UserRole, true);
                    m_table->item(row, 0)->setCheckState(Qt::Unchecked);
                }
            });
    connect(m_uploader, &ConfigUploader::runningChanged,
            this, [this](bool running) {
                // 上传期间锁定表格和开始按钮，同时通知主窗口禁用解析验证入口。
                m_startButton->setEnabled(!running);
                m_stopButton->setEnabled(running);
                m_table->setEnabled(!running);
                emit uploadRunningChanged(running);
            });
    connect(m_uploader, &ConfigUploader::batchFinished,
            this, [this](bool cancelled) {
                m_summary->setText(cancelled ? QStringLiteral("上传批次已中止")
                                             : QStringLiteral("上传批次已完成"));
            });
    scanFiles();
}

bool ConfigUploadDialog::isRunning() const
{
    return m_uploader->isRunning();
}

void ConfigUploadDialog::buildUi()
{
    // 创建配置扫描表格、批量选择按钮、一键上传/停止按钮和实时上传日志。
    setWindowTitle(QStringLiteral("批量上传设备配置"));
    resize(1050, 720);
    auto *root = new QVBoxLayout(this);
    auto *title = new QLabel(QStringLiteral("MQTT 批量上传设备配置"), this);
    title->setObjectName(QStringLiteral("dialogTitle"));
    auto *description = new QLabel(
        QStringLiteral("读取 all_data/<SN>.json，按设备 SN 连接 LS200 热点并上传；历史成功设备默认不勾选。"),
        this);
    description->setWordWrap(true);
    description->setObjectName(QStringLiteral("mutedText"));
    root->addWidget(title);
    root->addWidget(description);

    auto *toolbar = new QHBoxLayout;
    auto *rescanButton = new QPushButton(QStringLiteral("重新扫描"), this);
    auto *selectPendingButton = new QPushButton(QStringLiteral("全选未上传"), this);
    auto *clearButton = new QPushButton(QStringLiteral("取消全选"), this);
    m_summary = new QLabel(this);
    m_summary->setObjectName(QStringLiteral("mutedText"));
    m_startButton = new QPushButton(QStringLiteral("一键上传"), this);
    m_startButton->setObjectName(QStringLiteral("primaryButton"));
    m_stopButton = new QPushButton(QStringLiteral("停止"), this);
    m_stopButton->setObjectName(QStringLiteral("dangerButton"));
    m_stopButton->setEnabled(false);
    toolbar->addWidget(rescanButton);
    toolbar->addWidget(selectPendingButton);
    toolbar->addWidget(clearButton);
    toolbar->addWidget(m_summary, 1);
    toolbar->addWidget(m_stopButton);
    toolbar->addWidget(m_startButton);
    root->addLayout(toolbar);

    m_table = new QTableWidget(this);
    m_table->setColumnCount(5);
    m_table->setHorizontalHeaderLabels({QStringLiteral("选择"), QStringLiteral("SN"),
                                        QStringLiteral("配置文件"), QStringLiteral("上次上传成功"),
                                        QStringLiteral("本次状态")});
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Fixed);
    m_table->setColumnWidth(0, 64);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
    m_table->verticalHeader()->setVisible(false);
    m_table->setAlternatingRowColors(true);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    root->addWidget(m_table, 2);

    auto *logTitle = new QLabel(QStringLiteral("上传日志"), this);
    m_log = new QTextEdit(this);
    m_log->setReadOnly(true);
    root->addWidget(logTitle);
    root->addWidget(m_log, 1);

    connect(rescanButton, &QPushButton::clicked, this, &ConfigUploadDialog::scanFiles);
    connect(selectPendingButton, &QPushButton::clicked, this, [this] {
        for (int row = 0; row < m_table->rowCount(); ++row) {
            const bool uploaded = m_table->item(row, 3)->data(Qt::UserRole).toBool();
            m_table->item(row, 0)->setCheckState(uploaded ? Qt::Unchecked : Qt::Checked);
        }
        updateSelectionSummary();
    });
    connect(clearButton, &QPushButton::clicked, this, [this] {
        for (int row = 0; row < m_table->rowCount(); ++row)
            m_table->item(row, 0)->setCheckState(Qt::Unchecked);
        updateSelectionSummary();
    });
    connect(m_startButton, &QPushButton::clicked, this, &ConfigUploadDialog::startUpload);
    connect(m_stopButton, &QPushButton::clicked, this, &ConfigUploadDialog::stopUpload);
    connect(m_table, &QTableWidget::itemChanged, this,
            [this](QTableWidgetItem *item) {
                if (item && item->column() == 0)
                    updateSelectionSummary();
            });
}

void ConfigUploadDialog::scanFiles()
{
    // 重新扫描all_data下所有可读JSON；文件名去掉.json后直接作为真实SN。
    if (m_uploader->isRunning())
        return;
    m_table->setRowCount(0);
    m_scannedItems.clear();
    const QDir directory(m_allDataPath);
    const QFileInfoList files = directory.entryInfoList(
        {QStringLiteral("*.json")}, QDir::Files | QDir::Readable, QDir::Name);
    for (const QFileInfo &file : files) {
        ConfigUploadItem item;
        item.sn = normalizedConfigDeviceSn(file.completeBaseName());
        item.configName = file.fileName();
        item.configPath = file.absoluteFilePath();
        if (item.sn.isEmpty())
            continue;
        const int row = m_table->rowCount();
        m_table->insertRow(row);
        m_scannedItems.append(item);
        QString successAt;
        // 共用历史数据库：只要该SN历史上有一次success，以后扫描都默认不勾选。
        const bool uploaded = m_history->hasUploadSuccess(item.sn, &successAt);
        auto *check = new QTableWidgetItem;
        check->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
        check->setCheckState(uploaded ? Qt::Unchecked : Qt::Checked);
        m_table->setItem(row, 0, check);
        m_table->setItem(row, 1, new QTableWidgetItem(item.sn));
        auto *name = new QTableWidgetItem(item.configName);
        name->setTextAlignment(Qt::AlignCenter);
        m_table->setItem(row, 2, name);
        auto *history = new QTableWidgetItem(uploaded
            ? QDateTime::fromString(successAt, Qt::ISODate)
                  .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
            : QStringLiteral("--"));
        history->setData(Qt::UserRole, uploaded);
        m_table->setItem(row, 3, history);
        m_table->setItem(row, 4, new QTableWidgetItem(
            uploaded ? QStringLiteral("历史已成功，默认不选") : QStringLiteral("等待上传")));
    }
    if (!directory.exists())
        m_log->append(QStringLiteral("[软件] 目录不存在：%1").arg(m_allDataPath));
    else if (files.isEmpty())
        m_log->append(QStringLiteral("[软件] all_data 中未找到 JSON 文件"));
    updateSelectionSummary();
}

void ConfigUploadDialog::startUpload()
{
    // 把当前勾选项复制成固定快照，并保存“快照下标→表格行号”映射。
    QVector<ConfigUploadItem> selected;
    m_activeRows.clear();
    for (int row = 0; row < m_table->rowCount(); ++row) {
        if (m_table->item(row, 0)->checkState() != Qt::Checked)
            continue;
        selected.append(m_scannedItems.at(row));
        m_activeRows.append(row);
        updateRowState(row, QStringLiteral("排队等待"), QString());
    }
    if (selected.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("没有设备"),
                                 QStringLiteral("请先勾选需要上传配置的设备。"));
        return;
    }
    m_log->clear();
    m_uploader->start(selected);
}

void ConfigUploadDialog::stopUpload()
{
    // 停止操作需要二次确认，确认后由ConfigUploader记录当前项中止并清理连接。
    if (QMessageBox::question(this, QStringLiteral("停止上传"),
                              QStringLiteral("确定停止当前上传批次吗？"))
        == QMessageBox::Yes)
        m_uploader->stop();
}

void ConfigUploadDialog::updateSelectionSummary()
{
    // 重新统计勾选数量；没有选中设备时不允许启动上传。
    int selected = 0;
    for (int row = 0; row < m_table->rowCount(); ++row) {
        if (m_table->item(row, 0)->checkState() == Qt::Checked)
            ++selected;
    }
    m_summary->setText(QStringLiteral("共 %1 个配置，已选择 %2 个")
                           .arg(m_table->rowCount()).arg(selected));
    m_startButton->setEnabled(!m_uploader->isRunning() && selected > 0);
}

void ConfigUploadDialog::updateRowState(int index, const QString &state,
                                        const QString &detail)
{
    // 根据运行状态刷新文字和颜色：成功绿色、失败红色、处理中蓝色。
    if (index < 0 || index >= m_table->rowCount())
        return;
    QTableWidgetItem *item = m_table->item(index, 4);
    item->setText(detail.isEmpty() ? state : QStringLiteral("%1：%2").arg(state, detail));
    if (state == QStringLiteral("上传成功"))
        item->setForeground(QColor(QStringLiteral("#15803d")));
    else if (state == QStringLiteral("上传失败"))
        item->setForeground(QColor(QStringLiteral("#dc2626")));
    else
        item->setForeground(QColor(QStringLiteral("#1d4ed8")));
}

void ConfigUploadDialog::closeEvent(QCloseEvent *event)
{
    // 上传期间关闭窗口等价于停止任务，必须确认；空闲时可直接关闭。
    if (!m_uploader->isRunning()) {
        event->accept();
        return;
    }
    if (QMessageBox::question(this, QStringLiteral("上传仍在进行"),
                              QStringLiteral("关闭窗口将停止当前上传批次，是否继续？"))
        == QMessageBox::Yes) {
        m_uploader->stop();
        event->accept();
    } else {
        event->ignore();
    }
}
