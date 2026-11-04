#ifndef CONFIGUPLOADDIALOG_H
#define CONFIGUPLOADDIALOG_H

#include <QDialog>
#include <QVector>

#include "configuploader.h"

class HistoryDatabase;
class QLabel;
class QPushButton;
class QTableWidget;
class QTextEdit;
class QCloseEvent;

class ConfigUploadDialog : public QDialog
{
    Q_OBJECT
public:
    explicit ConfigUploadDialog(const QString &allDataPath,
                                HistoryDatabase *history,
                                QWidget *parent = nullptr);
    bool isRunning() const;

signals:
    void uploadRunningChanged(bool running);

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    void buildUi();
    void scanFiles();
    void startUpload();
    void stopUpload();
    void updateSelectionSummary();
    void updateRowState(int index, const QString &state, const QString &detail);

    QString m_allDataPath;
    HistoryDatabase *m_history = nullptr;
    ConfigUploader *m_uploader = nullptr;
    QTableWidget *m_table = nullptr;
    QTextEdit *m_log = nullptr;
    QLabel *m_summary = nullptr;
    QPushButton *m_startButton = nullptr;
    QPushButton *m_stopButton = nullptr;
    QVector<ConfigUploadItem> m_scannedItems;
    QVector<int> m_activeRows;
};

#endif
