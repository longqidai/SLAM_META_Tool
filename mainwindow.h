#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QVector>

#include "batchprogressdialog.h"
#include "historydatabase.h"

class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;
class QColor;
class BatchRunner;
class QCloseEvent;
class ConfigUploadDialog;

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void chooseDataRoot();
    void scanDevices();
    void selectPendingDevices();
    void clearSelection();
    void startSelectedBatch();
    void startParseOnlyBatch();
    void startValidateOnlyBatch();
    void showCurrentBatch();
    void openSelectedDeviceDirectory();
    void openSelectedReportDirectory();
    void showHistoryPreview();
    void exportDatabase();
    void showConfigUploader();
    void updateSelectionSummary();
    void handleEnvironmentChanged(bool dockerReady, bool containerReady,
                                  const QString &message);
    void handleDeviceState(int index, const QString &stage,
                           const QString &parseStatus, const QString &validateStatus,
                           const QString &detail);
    void handleDeviceFinished(int index, const DeviceRunResult &result);
    void handleBatchFinished(bool cancelled);

private:
    struct DeviceEntry {
        // 主界面一次扫描得到的展示模型；开始批次时 selectedBatchSnapshot()
        // 会把其中稳定的文件字段转换成 BatchDevice。
        QString sn;
        QString deviceDir;
        QString metaName;
        QString metaPath;
        QString metaTime;
        QString latestStatus;
        QString lastSuccessTime;
        QString statusKind;
        qint64 metaSize = 0;
        qint64 metaModifiedMs = 0;
        bool selectable = false;
        bool processed = false;
    };

    void buildUi();
    void applyStyle();
    void populateTable();
    void setStatusCell(int row, int column, const QString &text,
                       const QColor &foreground, const QColor &background);
    int selectedDeviceCount() const;
    QVector<BatchDevice> selectedBatchSnapshot() const;
    DeviceEntry deviceAtRow(int row) const;
    void startBatch(BatchMode mode);

    QLineEdit *m_dataRootEdit = nullptr;
    QLabel *m_dockerStatus = nullptr;
    QLabel *m_containerStatus = nullptr;
    QLabel *m_deviceCountValue = nullptr;
    QLabel *m_pendingCountValue = nullptr;
    QLabel *m_processedCountValue = nullptr;
    QLabel *m_selectedCountValue = nullptr;
    QLabel *m_selectionHint = nullptr;
    QTableWidget *m_table = nullptr;
    QPushButton *m_startButton = nullptr;
    QPushButton *m_parseOnlyButton = nullptr;
    QPushButton *m_validateOnlyButton = nullptr;
    QPushButton *m_currentBatchButton = nullptr;
    BatchProgressDialog *m_batchDialog = nullptr;
    BatchRunner *m_runner = nullptr;
    HistoryDatabase m_history;
    QVector<BatchDevice> m_activeBatch;
    QVector<DeviceEntry> m_devices;
    bool m_closeAfterStop = false;
    ConfigUploadDialog *m_uploadDialog = nullptr;
};

#endif

