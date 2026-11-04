#ifndef BATCHPROGRESSDIALOG_H
#define BATCHPROGRESSDIALOG_H

#include <QDialog>
#include <QElapsedTimer>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVector>

#include "tasktypes.h"

class QLabel;
class QPushButton;
class QTableWidget;
class QColor;

class BatchProgressDialog : public QDialog
{
    Q_OBJECT
public:
    explicit BatchProgressDialog(QWidget *parent = nullptr);
    void setBatch(const QVector<BatchDevice> &devices, const QString &batchId,
                  BatchMode mode);
    void updateDeviceState(int index, const QString &stage,
                           const QString &parseStatus, const QString &validateStatus,
                           const QString &detail);
    void appendDeviceLog(int index, const QString &line);
    void setBatchFinished(bool cancelled);
    void setEnvironmentFailure(const QString &message);

signals:
    void stopRequested();

private slots:
    void openSelectedDeviceDirectory();
    void showCurrentLogHint();

private:
    void updateSummary();
    void updateElapsedTime();
    void setStatusItem(int row, int column, const QString &text,
                       const QColor &foreground, const QColor &background);
    void statusColors(const QString &status, QColor *foreground, QColor *background) const;

    struct RowState {
        QString stage;
        QString parseStatus;
        QString validateStatus;
        QString detail;
    };

    QVector<BatchDevice> m_devices;
    QVector<RowState> m_states;
    QVector<QStringList> m_logs;
    QLabel *m_batchLabel = nullptr;
    QLabel *m_summaryLabel = nullptr;
    QLabel *m_currentTaskLabel = nullptr;
    QLabel *m_runtimeLabel = nullptr;
    QTableWidget *m_table = nullptr;
    QPushButton *m_stopButton = nullptr;
    QElapsedTimer m_elapsed;
    QTimer m_elapsedTimer;
    BatchMode m_mode = BatchMode::ParseAndValidate;
    bool m_finished = false;
    bool m_environmentFailed = false;
};

#endif

