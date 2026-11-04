#ifndef HISTORYDATABASE_H
#define HISTORYDATABASE_H

#include <QString>
#include <QStringList>
#include <QVector>

#include "tasktypes.h"

struct MetaHistoryStatus
{
    // exact* 对应当前路径+大小+修改时间完全相同的 META；lastSuccessAt
    // 对应该 SN 历史上最近一次解析和验证均成功的完成时间。
    QString exactParseStatus;
    QString exactValidateStatus;
    QString exactFinishedAt;
    QString lastSuccessAt;
    bool snHasSuccess = false;
};

struct ConfigUploadResult
{
    QString batchId;
    QString sn;
    QString configName;
    QString configPath;
    QString reqId;
    QString startedAt;
    QString finishedAt;
    QString status;
    QString error;
};

class HistoryDatabase
{
public:
    HistoryDatabase();
    ~HistoryDatabase();

    bool open(QString *errorMessage = nullptr);
    bool isOpen() const;
    QString databasePath() const;
    bool exportTo(const QString &destinationPath, QString *errorMessage = nullptr) const;
    MetaHistoryStatus statusFor(const QString &sn, const QString &metaPath,
                                qint64 metaSize, qint64 modifiedMs) const;
    bool saveResult(const DeviceRunResult &result, QString *errorMessage = nullptr);
    bool hasUploadSuccess(const QString &sn, QString *successTime = nullptr) const;
    bool saveUploadResult(const ConfigUploadResult &result,
                          QString *errorMessage = nullptr);
    QVector<QStringList> recentHistory(int limit = 200) const;
    QVector<QStringList> historyBySn(const QString &snKeyword, int limit = 200) const;
    QVector<QStringList> recentUploadHistory(int limit = 200) const;
    QVector<QStringList> uploadHistoryBySn(const QString &snKeyword,
                                           int limit = 200) const;

private:
    QString m_connectionName;
    QString m_databasePath;
};

#endif
