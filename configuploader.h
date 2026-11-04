#ifndef CONFIGUPLOADER_H
#define CONFIGUPLOADER_H

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVector>

#include "historydatabase.h"

class QMqttClient;
class QMqttTopicName;
class QTcpSocket;

struct ConfigUploadItem
{
    // 一条待上传任务：SN来自all_data下JSON文件名，configPath为完整本地路径。
    QString sn;
    QString configName;
    QString configPath;
};

// 规范化配置文件名中的SN；只去首尾空白并转大写，不删除SN中的任何字符。
QString normalizedConfigDeviceSn(const QString &value);
// 根据SN生成设备热点标准名称：去掉开头SN后拼成LS200-<序列号>。
// 例如SNL88L00030对应LS200-L88L00030。
QString deviceWifiSsidForSn(const QString &sn);

class ConfigUploader : public QObject
{
    Q_OBJECT
public:
    explicit ConfigUploader(QObject *parent = nullptr);
    bool isRunning() const;
    void start(const QVector<ConfigUploadItem> &items);
    void stop();

signals:
    void runningChanged(bool running);
    void itemStateChanged(int index, const QString &state, const QString &detail);
    void itemFinished(int index, const ConfigUploadResult &result);
    void logMessage(const QString &line);
    void batchFinished(bool cancelled);

private slots:
    void onTick();
    void onMqttConnected();
    void onMqttDisconnected();

private:
    // 单台设备上传状态机。每次只处理一个SN，onTick()按当前Phase推进或超时。
    enum class Phase {
        Idle,              // 当前没有设备上传
        ConnectingWifi,    // 用Windows WLAN API连接目标设备热点
        SettlingNetwork,   // 热点已连接，等待DHCP和路由稳定
        ConnectingDevice,  // 探测192.168.1.8:25001确认设备网络服务可用
        ConnectingMqtt,    // 连接设备MQTT 1883/26001
        WaitingHeartbeat,  // 等待心跳中的DeviceSN并防止传错设备
        WaitingResponse    // 配置已发布，等待相同req_id的save_config响应
    };

    void startNext();
    void beginWifiConnection();
    void attemptWifiConnection();
    void beginDeviceConnection();
    void attemptDeviceConnection();
    void beginMqttConnection();
    void connectCurrentMqttPort();
    void onMqttMessage(const QByteArray &message, const QMqttTopicName &topic);
    void publishCurrentConfig();
    void finishCurrent(bool success, const QString &error = QString());
    void finishBatch(bool cancelled);
    void appendLog(const QString &message);
    QStringList wifiSsidCandidates(const QString &sn) const;
    bool wifiMatchesSn(const QString &ssid, const QString &sn) const;
    QString currentConnectedSsid() const;
    bool connectOpenWifi(const QString &ssid, QString *errorMessage) const;

    QMqttClient *m_mqtt = nullptr;       // 当前设备的MQTT连接
    QTcpSocket *m_deviceSocket = nullptr;// 仅用于探测设备信息端口是否就绪
    QTimer m_tickTimer;
    QElapsedTimer m_phaseElapsed;
    QVector<ConfigUploadItem> m_items;   // 本次点击“一键上传”时固定的任务快照
    int m_index = -1;                    // 当前正在处理的快照下标
    int m_wifiAttemptSeconds = 0;
    int m_wifiCandidateIndex = 0;
    int m_deviceAttemptSeconds = 0;
    QVector<quint16> m_mqttPortCandidates{1883, 26001};
    int m_mqttPortCandidateIndex = 0;
    int m_mqttPortFailureCount = 0;
    Phase m_phase = Phase::Idle;
    bool m_running = false;
    bool m_stopping = false;
    QString m_batchId;                   // 整个上传批次共用的时间批次号
    QString m_reqId;                     // 当前设备本次save_config请求的唯一ID
    QString m_startedAt;
    QString m_targetWifiSsid;
};

#endif
