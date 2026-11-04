#include "historydatabase.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QUuid>
#include <QVariant>

// 为本实例创建唯一 Qt SQL 连接名，并通过 QStandardPaths 选择应用数据目录。
// 数据库文件固定名为 slam_meta_history.sqlite。
HistoryDatabase::HistoryDatabase()
    : m_connectionName(QStringLiteral("history_%1").arg(
          QUuid::createUuid().toString(QUuid::WithoutBraces)))
{
    QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (dataDir.isEmpty())
        dataDir = QCoreApplication::applicationDirPath();
    QDir().mkpath(dataDir);
    m_databasePath = QDir(dataDir).filePath(QStringLiteral("slam_meta_history.sqlite"));
}

// 按 Qt 要求先关闭局部数据库对象，再从连接池删除命名连接。
HistoryDatabase::~HistoryDatabase()
{
    if (QSqlDatabase::contains(m_connectionName)) {
        {
            QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
            db.close();
        }
        QSqlDatabase::removeDatabase(m_connectionName);
    }
}

// 打开 SQLite，创建 processing_history 表和 META/SN 查询索引。
// 当前结构直接保存解析与验证的独立状态，不使用 SHA 字段。
bool HistoryDatabase::open(QString *errorMessage)
{
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    db.setDatabaseName(m_databasePath);
    if (!db.open()) {
        if (errorMessage)
            *errorMessage = db.lastError().text();
        return false;
    }

    QSqlQuery query(db);
    const QString schema = QStringLiteral(
        "CREATE TABLE IF NOT EXISTS processing_history ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "batch_id TEXT NOT NULL, sn TEXT NOT NULL,"
        "meta_name TEXT NOT NULL, meta_path TEXT NOT NULL,"
        "meta_size INTEGER NOT NULL, meta_modified_ms INTEGER NOT NULL,"
        "started_at TEXT, finished_at TEXT,"
        "parse_status TEXT, parse_exit_code INTEGER, parse_error TEXT,"
        "validate_status TEXT, validate_exit_code INTEGER, validate_error TEXT,"
        "warning_count INTEGER DEFAULT 0, hardware_path TEXT, metrics_path TEXT,"
        "log_path TEXT)" );
    if (!query.exec(schema)) {
        if (errorMessage)
            *errorMessage = query.lastError().text();
        return false;
    }

    query.exec(QStringLiteral(
        "CREATE INDEX IF NOT EXISTS idx_history_meta ON processing_history"
        "(meta_path, meta_size, meta_modified_ms, id DESC)"));
    query.exec(QStringLiteral(
        "CREATE INDEX IF NOT EXISTS idx_history_sn ON processing_history(sn, id DESC)"));

    const QString uploadSchema = QStringLiteral(
        "CREATE TABLE IF NOT EXISTS config_upload_history ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "batch_id TEXT NOT NULL, sn TEXT NOT NULL,"
        "config_name TEXT NOT NULL, config_path TEXT NOT NULL, req_id TEXT,"
        "started_at TEXT, finished_at TEXT, status TEXT NOT NULL, error TEXT)" );
    if (!query.exec(uploadSchema)) {
        if (errorMessage)
            *errorMessage = query.lastError().text();
        return false;
    }
    query.exec(QStringLiteral(
        "CREATE INDEX IF NOT EXISTS idx_upload_sn ON config_upload_history(sn, id DESC)"));
    return true;
}

// 检查命名连接是否存在且数据库仍保持打开。
bool HistoryDatabase::isOpen() const
{
    return QSqlDatabase::contains(m_connectionName)
        && QSqlDatabase::database(m_connectionName, false).isOpen();
}

// 返回实际数据库路径，历史记录窗口会把它显示给用户。
QString HistoryDatabase::databasePath() const
{
    return m_databasePath;
}

// 将当前打开的SQLite数据库导出为一致性快照。VACUUM INTO可在数据库保持
// 打开时生成完整副本；目标同名文件先删除，因此每次导出都覆盖上次结果。
bool HistoryDatabase::exportTo(const QString &destinationPath,
                               QString *errorMessage) const
{
    if (!isOpen()) {
        if (errorMessage)
            *errorMessage = QStringLiteral("历史数据库未打开");
        return false;
    }

    const QString cleanDestination = QDir::cleanPath(destinationPath);
    if (QDir::cleanPath(m_databasePath).compare(cleanDestination,
                                                Qt::CaseInsensitive) == 0) {
        if (errorMessage)
            *errorMessage = QStringLiteral("导出路径不能与正在使用的数据库相同");
        return false;
    }

    const QFileInfo destinationInfo(cleanDestination);
    if (!QDir().mkpath(destinationInfo.absolutePath())) {
        if (errorMessage)
            *errorMessage = QStringLiteral("无法创建导出目录：%1")
                                .arg(destinationInfo.absolutePath());
        return false;
    }
    if (QFileInfo::exists(cleanDestination) && !QFile::remove(cleanDestination)) {
        if (errorMessage)
            *errorMessage = QStringLiteral("无法覆盖已有数据库，文件可能正被占用");
        return false;
    }

    QString sqlPath = QDir::fromNativeSeparators(cleanDestination);
    sqlPath.replace(QLatin1Char('\''), QStringLiteral("''"));
    QSqlQuery query(QSqlDatabase::database(m_connectionName));
    if (!query.exec(QStringLiteral("VACUUM INTO '%1'").arg(sqlPath))) {
        if (errorMessage)
            *errorMessage = query.lastError().text();
        return false;
    }
    return true;
}

// 扫描时的查询入口。第一条 SQL 用路径+大小+修改时间匹配当前具体 META；
// 第二条 SQL 查询该 SN 最近一次解析和验证均成功的时间。
MetaHistoryStatus HistoryDatabase::statusFor(const QString &sn, const QString &metaPath,
                                             qint64 metaSize, qint64 modifiedMs) const
{
    // 1. 数据库不可用时返回全空状态，主界面会把当前 META 显示为未处理。
    MetaHistoryStatus result;
    if (!isOpen())
        return result;

    // 2. 精确查询当前 META：路径、大小、修改时间必须全部相同。
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery exact(db);
    exact.prepare(QStringLiteral(
        "SELECT parse_status, validate_status, finished_at FROM processing_history "
        "WHERE meta_path=? AND meta_size=? AND meta_modified_ms=? "
        "ORDER BY id DESC LIMIT 1"));
    exact.addBindValue(metaPath);
    exact.addBindValue(metaSize);
    exact.addBindValue(modifiedMs);
    // 3. 取精确匹配记录中 id 最大的一条，作为当前 META 的最新处理结果。
    if (exact.exec() && exact.next()) {
        result.exactParseStatus = exact.value(0).toString();
        result.exactValidateStatus = exact.value(1).toString();
        result.exactFinishedAt = exact.value(2).toString();
    }

    // 4. 再按 SN 查询最近一次“解析成功+验证成功”，用于上次成功时间列。
    QSqlQuery success(db);
    success.prepare(QStringLiteral(
        "SELECT finished_at FROM processing_history "
        "WHERE sn=? AND parse_status='success' AND validate_status='success' "
        "ORDER BY id DESC LIMIT 1"));
    success.addBindValue(sn);
    if (success.exec() && success.next()) {
        result.snHasSuccess = true;
        result.lastSuccessAt = success.value(0).toString();
    }
    return result;
}

// 每台设备 finishDevice() 后追加一条完整历史。采用 INSERT 保留历次结果，
// 不 UPDATE/覆盖旧记录；主界面查询时按 id DESC 取最新一条。
bool HistoryDatabase::saveResult(const DeviceRunResult &r, QString *errorMessage)
{
    // 1. 数据库未打开时立即失败，并把原因交回主窗口状态栏。
    if (!isOpen()) {
        if (errorMessage)
            *errorMessage = QStringLiteral("历史数据库未打开");
        return false;
    }
    // 2. INSERT 一条完整运行结果；历史记录采用追加方式，不覆盖旧记录。
    QSqlQuery query(QSqlDatabase::database(m_connectionName));
    query.prepare(QStringLiteral(
        "INSERT INTO processing_history (batch_id,sn,meta_name,meta_path,meta_size,"
        "meta_modified_ms,started_at,finished_at,parse_status,parse_exit_code,"
        "parse_error,validate_status,validate_exit_code,validate_error,warning_count,"
        "hardware_path,metrics_path,log_path) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"));
    // 3. 按 SQL 字段顺序绑定结果，避免字符串拼接和转义问题。
    query.addBindValue(r.batchId);
    query.addBindValue(r.sn);
    query.addBindValue(r.metaName);
    query.addBindValue(r.metaPath);
    query.addBindValue(r.metaSize);
    query.addBindValue(r.metaModifiedMs);
    query.addBindValue(r.startedAt);
    query.addBindValue(r.finishedAt);
    query.addBindValue(r.parseStatus);
    query.addBindValue(r.parseExitCode);
    query.addBindValue(r.parseError);
    query.addBindValue(r.validateStatus);
    query.addBindValue(r.validateExitCode);
    query.addBindValue(r.validateError);
    query.addBindValue(r.warningCount);
    query.addBindValue(r.hardwarePath);
    query.addBindValue(r.metricsPath);
    query.addBindValue(r.logPath);
    // 4. 执行失败返回 SQLite 错误；成功后该设备历史立即可查询。
    if (!query.exec()) {
        if (errorMessage)
            *errorMessage = query.lastError().text();
        return false;
    }
    return true;
}

bool HistoryDatabase::hasUploadSuccess(const QString &sn, QString *successTime) const
{
    // 上传窗口扫描all_data时调用：按SN查找任意一次历史success。
    // 一旦成功过就返回最近成功时间，使该SN以后默认不勾选。
    if (successTime)
        successTime->clear();
    if (!isOpen())
        return false;

    QSqlQuery query(QSqlDatabase::database(m_connectionName));
    query.prepare(QStringLiteral(
        "SELECT finished_at FROM config_upload_history "
        "WHERE sn=? AND status='success' ORDER BY id DESC LIMIT 1"));
    query.addBindValue(sn);
    if (!query.exec() || !query.next())
        return false;
    if (successTime)
        *successTime = query.value(0).toString();
    return true;
}

bool HistoryDatabase::saveUploadResult(const ConfigUploadResult &r,
                                       QString *errorMessage)
{
    // 每台设备上传完成后INSERT一条新记录，保留success、failed和interrupted历次历史。
    if (!isOpen()) {
        if (errorMessage)
            *errorMessage = QStringLiteral("历史数据库未打开");
        return false;
    }

    QSqlQuery query(QSqlDatabase::database(m_connectionName));
    query.prepare(QStringLiteral(
        "INSERT INTO config_upload_history "
        "(batch_id,sn,config_name,config_path,req_id,started_at,finished_at,status,error) "
        "VALUES (?,?,?,?,?,?,?,?,?)"));
    query.addBindValue(r.batchId);
    query.addBindValue(r.sn);
    query.addBindValue(r.configName);
    query.addBindValue(r.configPath);
    query.addBindValue(r.reqId);
    query.addBindValue(r.startedAt);
    query.addBindValue(r.finishedAt);
    query.addBindValue(r.status);
    query.addBindValue(r.error);
    if (!query.exec()) {
        if (errorMessage)
            *errorMessage = query.lastError().text();
        return false;
    }
    return true;
}

// 返回最近 limit 条记录供历史窗口显示，默认 200 条，只读不修改数据库。
QVector<QStringList> HistoryDatabase::recentHistory(int limit) const
{
    QVector<QStringList> rows;
    if (!isOpen())
        return rows;
    QSqlQuery query(QSqlDatabase::database(m_connectionName));
    query.prepare(QStringLiteral(
        "SELECT finished_at,sn,meta_name,parse_status,validate_status,warning_count,"
        "COALESCE(NULLIF(validate_error,''),parse_error) FROM processing_history "
        "ORDER BY id DESC LIMIT ?"));
    query.addBindValue(limit);
    if (!query.exec())
        return rows;
    while (query.next()) {
        QStringList row;
        for (int i = 0; i < 7; ++i)
            row.append(query.value(i).toString());
        rows.append(row);
    }
    return rows;
}

// 按SN关键字查询历史，支持输入完整SN或其中一部分；LIKE通配字符会被转义，
// 因此用户输入的%、_都按普通字符处理。结果仍按最新记录优先排列。
QVector<QStringList> HistoryDatabase::historyBySn(const QString &snKeyword,
                                                  int limit) const
{
    const QString keyword = snKeyword.trimmed();
    if (keyword.isEmpty())
        return recentHistory(limit);

    QVector<QStringList> rows;
    if (!isOpen())
        return rows;

    QString pattern = keyword;
    pattern.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    pattern.replace(QLatin1Char('%'), QStringLiteral("\\%"));
    pattern.replace(QLatin1Char('_'), QStringLiteral("\\_"));

    QSqlQuery query(QSqlDatabase::database(m_connectionName));
    query.prepare(QStringLiteral(
        "SELECT finished_at,sn,meta_name,parse_status,validate_status,warning_count,"
        "COALESCE(NULLIF(validate_error,''),parse_error) FROM processing_history "
        "WHERE sn LIKE ? ESCAPE '\\' COLLATE NOCASE ORDER BY id DESC LIMIT ?"));
    query.addBindValue(QStringLiteral("%") + pattern + QStringLiteral("%"));
    query.addBindValue(limit);
    if (!query.exec())
        return rows;

    while (query.next()) {
        QStringList row;
        for (int i = 0; i < 7; ++i)
            row.append(query.value(i).toString());
        rows.append(row);
    }
    return rows;
}

QVector<QStringList> HistoryDatabase::recentUploadHistory(int limit) const
{
    // 上传历史窗口默认按最新ID倒序显示最近limit条记录。
    QVector<QStringList> rows;
    if (!isOpen())
        return rows;
    QSqlQuery query(QSqlDatabase::database(m_connectionName));
    query.prepare(QStringLiteral(
        "SELECT finished_at,sn,config_name,status,req_id,error,batch_id "
        "FROM config_upload_history ORDER BY id DESC LIMIT ?"));
    query.addBindValue(limit);
    if (!query.exec())
        return rows;
    while (query.next()) {
        QStringList row;
        for (int i = 0; i < 7; ++i)
            row.append(query.value(i).toString());
        rows.append(row);
    }
    return rows;
}

QVector<QStringList> HistoryDatabase::uploadHistoryBySn(const QString &snKeyword,
                                                        int limit) const
{
    // 支持完整或部分SN查询；转义LIKE通配符，防止用户输入改变匹配语义。
    const QString keyword = snKeyword.trimmed();
    if (keyword.isEmpty())
        return recentUploadHistory(limit);
    QVector<QStringList> rows;
    if (!isOpen())
        return rows;

    QString pattern = keyword;
    pattern.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    pattern.replace(QLatin1Char('%'), QStringLiteral("\\%"));
    pattern.replace(QLatin1Char('_'), QStringLiteral("\\_"));
    QSqlQuery query(QSqlDatabase::database(m_connectionName));
    query.prepare(QStringLiteral(
        "SELECT finished_at,sn,config_name,status,req_id,error,batch_id "
        "FROM config_upload_history WHERE sn LIKE ? ESCAPE '\\' COLLATE NOCASE "
        "ORDER BY id DESC LIMIT ?"));
    query.addBindValue(QStringLiteral("%") + pattern + QStringLiteral("%"));
    query.addBindValue(limit);
    if (!query.exec())
        return rows;
    while (query.next()) {
        QStringList row;
        for (int i = 0; i < 7; ++i)
            row.append(query.value(i).toString());
        rows.append(row);
    }
    return rows;
}
