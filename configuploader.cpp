#include "configuploader.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpSocket>
#include <QtMqtt/qmqttclient.h>
#include <QtMqtt/qmqttsubscription.h>
#include <QtMqtt/qmqtttopicfilter.h>
#include <QtMqtt/qmqtttopicname.h>

#ifdef Q_OS_WIN
#include <windows.h>
#include <wlanapi.h>
#endif

namespace {
constexpr int kWifiTimeoutMs = 45000;
constexpr int kNetworkSettleMs = 2500;
constexpr int kDeviceConnectTimeoutMs = 30000;
constexpr int kMqttTimeoutMs = 30000;
constexpr int kHeartbeatTimeoutMs = 15000;
constexpr int kResponseTimeoutMs = 20000;

#ifdef Q_OS_WIN
bool installOpenProfile(HANDLE handle, const GUID &guid, const QString &ssid,
                        DWORD *reasonCode)
{
    const QString escaped = ssid.toHtmlEscaped();
    const QString xml = QStringLiteral(
        "<?xml version=\"1.0\"?>"
        "<WLANProfile xmlns=\"http://www.microsoft.com/networking/WLAN/profile/v1\">"
        "<name>%1</name><SSIDConfig><SSID><name>%1</name></SSID>"
        "<nonBroadcast>false</nonBroadcast></SSIDConfig>"
        "<connectionType>ESS</connectionType><connectionMode>manual</connectionMode>"
        "<MSM><security><authEncryption><authentication>open</authentication>"
        "<encryption>none</encryption><useOneX>false</useOneX>"
        "</authEncryption></security></MSM></WLANProfile>").arg(escaped);
    DWORD localReason = 0;
    const DWORD code = WlanSetProfile(
        handle, &guid, 0, reinterpret_cast<LPCWSTR>(xml.utf16()), nullptr,
        TRUE, nullptr, &localReason);
    if (reasonCode)
        *reasonCode = localReason;
    return code == ERROR_SUCCESS;
}
#endif
}

QString normalizedConfigDeviceSn(const QString &value)
{
    // JSON文件名就是设备真实SN，仅统一大小写，不能再自动删除历史多余字符。
    return value.trimmed().toUpper();
}

QString deviceWifiSsidForSn(const QString &sn)
{
    // 设备热点不带SN前缀：SNL88L00030 -> LS200-L88L00030。
    const QString normalized = normalizedConfigDeviceSn(sn);
    const QString suffix = normalized.startsWith(QStringLiteral("SN"))
                               ? normalized.mid(2)
                               : normalized;
    return QStringLiteral("LS200-%1").arg(suffix);
}

ConfigUploader::ConfigUploader(QObject *parent)
    : QObject(parent), m_mqtt(new QMqttClient(this)),
      m_deviceSocket(new QTcpSocket(this))
{
    // 与simple_qt和设备端保持一致，使用MQTT 3.1.1，并兼容1883和26001端口。
    m_mqtt->setProtocolVersion(QMqttClient::MQTT_3_1_1);
    // 状态机的“时钟”：上传开始后每1秒调用一次onTick()。
    // onTick()负责轮询Wi-Fi、定时重试TCP，以及检查各阶段是否超时；
    // TCP/MQTT连接成功和收到MQTT消息则由下面各自的异步信号立即推进，不必等下一次tick。
    m_tickTimer.setInterval(1000);
    connect(&m_tickTimer, &QTimer::timeout, this, &ConfigUploader::onTick);

    // ConnectingDevice阶段由attemptDeviceConnection()异步连接25001端口。
    // 连接成功时QTcpSocket发出connected，本lambda把状态机推进到ConnectingMqtt。
    connect(m_deviceSocket, &QTcpSocket::connected, this, [this]() {
        // 25001端口连通证明热点不仅关联成功，而且DHCP、路由和设备服务已就绪。
        if (!m_running || m_phase != Phase::ConnectingDevice)
            return;
        appendLog(QStringLiteral("[%1] 设备信息端口 192.168.1.8:25001 已连接，网络已就绪")
                      .arg(m_items.at(m_index).sn));
        emit itemStateChanged(m_index, QStringLiteral("网络已就绪"),
                              QStringLiteral("192.168.1.8:25001"));
        m_deviceSocket->disconnectFromHost();
        beginMqttConnection();
    });
    // ConnectingMqtt连接成功后进入onMqttConnected()：订阅主题并转入WaitingHeartbeat。
    connect(m_mqtt, &QMqttClient::connected,
            this, &ConfigUploader::onMqttConnected);
    connect(m_mqtt, &QMqttClient::disconnected,
            this, &ConfigUploader::onMqttDisconnected);
    connect(m_mqtt, &QMqttClient::errorChanged, this,
            [this](QMqttClient::ClientError error) {
                // MQTT连接阶段发生错误时记录失败次数；当前端口连续失败三次后
                // 切换到另一个候选端口，再由singleShot稍后重连。
                if (error == QMqttClient::NoError || !m_running
                    || m_phase != Phase::ConnectingMqtt) {
                    return;
                }

                const quint16 failedPort =
                    m_mqttPortCandidates.at(m_mqttPortCandidateIndex);
                appendLog(QStringLiteral("[%1] MQTT 192.168.1.8:%2 连接失败，错误码 %3")
                              .arg(m_items.at(m_index).sn)
                              .arg(failedPort)
                              .arg(static_cast<int>(error)));

                ++m_mqttPortFailureCount;
                if (m_mqttPortFailureCount >= 3
                    && m_mqttPortCandidateIndex + 1 < m_mqttPortCandidates.size()) {
                    ++m_mqttPortCandidateIndex;
                    m_mqttPortFailureCount = 0;
                    appendLog(QStringLiteral("[%1] MQTT 端口 %2 连续失败，切换到端口 %3")
                                  .arg(m_items.at(m_index).sn)
                                  .arg(failedPort)
                                  .arg(m_mqttPortCandidates.at(m_mqttPortCandidateIndex)));
                }

                m_mqtt->disconnectFromHost();
                QTimer::singleShot(800, this, [this]() {
                    if (m_running && m_phase == Phase::ConnectingMqtt)
                        connectCurrentMqttPort();
                });
            });
    // WaitingHeartbeat和WaitingResponse都靠messageReceived推进：
    // 前者接收设备心跳并核对SN，后者接收save_config的处理结果。
    connect(m_mqtt, &QMqttClient::messageReceived, this,
            [this](const QByteArray &message, const QMqttTopicName &topic) {
                // 心跳和save_config响应共用一个入口，再按topic和当前Phase分流。
                onMqttMessage(message, topic);
            });
}

bool ConfigUploader::isRunning() const
{
    return m_running;
}

void ConfigUploader::start(const QVector<ConfigUploadItem> &items)
{
    // 固定本批上传快照并生成批次号；上传设备始终逐台串行处理。
    if (m_running || items.isEmpty())
        return;
    m_items = items;
    m_index = -1;
    m_stopping = false;
    m_running = true;
    m_batchId = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    // 从这里开始，onTick()每秒检查一次当前m_phase。
    m_tickTimer.start();
    emit runningChanged(true);
    appendLog(QStringLiteral("开始配置上传批次，共 %1 台设备").arg(items.size()));
    // 选择第一台设备，最终调用beginWifiConnection()进入ConnectingWifi。
    startNext();
}

void ConfigUploader::stop()
{
    // 用户停止时当前项记为interrupted，finishCurrent()仍会发结果供数据库保存。
    if (!m_running)
        return;
    m_stopping = true;
    if (m_index >= 0 && m_index < m_items.size())
        finishCurrent(false, QStringLiteral("用户中止上传"));
    else
        finishBatch(true);
}

void ConfigUploader::startNext()
{
    // 当前设备结束后把下标移到下一台；全部完成或已请求停止则结束上传批次。
    if (!m_running)
        return;
    if (m_stopping) {
        finishBatch(true);
        return;
    }
    // 上传严格逐台进行：当前设备结束后才把下标移到下一台。
    ++m_index;
    if (m_index >= m_items.size()) {
        finishBatch(false);
        return;
    }
    m_reqId.clear();
    m_startedAt = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
    // 每台设备的状态机入口：Idle -> ConnectingWifi。
    beginWifiConnection();
}

void ConfigUploader::beginWifiConnection()
{
    // 每台设备开始前清理上一台TCP/MQTT连接，再进入热点连接阶段。
    m_deviceSocket->abort();
    if (m_mqtt->state() != QMqttClient::Disconnected)
        m_mqtt->disconnectFromHost();
    // onTick()进入ConnectingWifi分支后，每秒读取电脑当前连接的SSID。
    m_phase = Phase::ConnectingWifi;
    m_phaseElapsed.restart();
    m_wifiAttemptSeconds = 100;
    m_wifiCandidateIndex = 0;
    const QStringList candidates = wifiSsidCandidates(m_items.at(m_index).sn);
    emit itemStateChanged(m_index, QStringLiteral("正在连接热点"),
                          candidates.join(QStringLiteral(" / ")));
    appendLog(QStringLiteral("[%1] 正在连接设备热点，候选名称：%2")
                  .arg(m_items.at(m_index).sn,
                       candidates.join(QStringLiteral("、"))));
    // 立即提交第一次WLAN连接请求；之后连接仍未成功时由onTick()每5秒重试。
    attemptWifiConnection();
}

void ConfigUploader::attemptWifiConnection()
{
    // 在LS200-<序列号>和SLAM-<完整SN>候选名之间轮流尝试连接。
    QString error;
    const QStringList candidates = wifiSsidCandidates(m_items.at(m_index).sn);
    if (candidates.isEmpty())
        return;
    const QString ssid = candidates.at(m_wifiCandidateIndex % candidates.size());
    m_wifiCandidateIndex = (m_wifiCandidateIndex + 1) % candidates.size();
    appendLog(QStringLiteral("[%1] 尝试连接热点 %2")
                  .arg(m_items.at(m_index).sn, ssid));
    if (!connectOpenWifi(ssid, &error) && !error.isEmpty())
        appendLog(QStringLiteral("[%1] 发起热点连接失败：%2")
                      .arg(m_items.at(m_index).sn, error));
    m_wifiAttemptSeconds = 0;
}

void ConfigUploader::onTick()
{
    // 本函数每秒执行一次。它根据m_phase只处理“当前阶段”的轮询、重试或超时，
    // 不会从头重复整套流程。阶段成功时会修改m_phase或调用下一阶段的begin函数。
    if (!m_running || m_index < 0 || m_index >= m_items.size())
        return;

    // ConnectingWifi阶段要允许电脑从原网络切换到设备热点，所以暂不执行“掉线”检查。
    // 一旦离开ConnectingWifi，后续每个阶段都要求电脑始终连着当前设备热点；
    // 若SSID改变或无线网断开，立即结束本台，防止把配置发给错误设备。
    if (m_phase != Phase::Idle && m_phase != Phase::ConnectingWifi) {
        const QString connected = currentConnectedSsid();
        const QString sn = m_items.at(m_index).sn;
        const QString expected = wifiSsidCandidates(sn).join(QStringLiteral(" / "));
        if (!wifiMatchesSn(connected, sn)) {
            finishCurrent(false,
                          QStringLiteral("设备 Wi-Fi 连接中断：当前 %1，期望 %2")
                              .arg(connected.isEmpty() ? QStringLiteral("未连接") : connected,
                                   expected));
            return;
        }
    }

    // m_phase是单台设备上传的当前位置。一个tick只会进入下面一个case。
    switch (m_phase) {
    case Phase::ConnectingWifi: {
        // 第1阶段：确认Windows是否已经真正连接目标热点。
        // beginWifiConnection()/attemptWifiConnection()只负责提交WlanConnect请求；
        // 真正连接成功必须以currentConnectedSsid()读到匹配SSID为准。
        const QString connected = currentConnectedSsid();
        const QString sn = m_items.at(m_index).sn;
        const QString expected = wifiSsidCandidates(sn).join(QStringLiteral(" / "));
        if (wifiMatchesSn(connected, sn)) {
            appendLog(QStringLiteral("[%1] 已连接热点 %2，等待网络地址就绪")
                          .arg(m_items.at(m_index).sn, connected));
            emit itemStateChanged(m_index, QStringLiteral("网络就绪中"), connected);
            // 热点已连上，但DHCP地址和路由可能尚未就绪，进入2.5秒稳定等待阶段。
            // restart()从零开始计算SettlingNetwork阶段的耗时。
            m_phase = Phase::SettlingNetwork;
            m_phaseElapsed.restart();
            return;
        }
        // 从进入ConnectingWifi起超过45秒仍未匹配到目标SSID，本台上传失败。
        if (m_phaseElapsed.elapsed() >= kWifiTimeoutMs) {
            finishCurrent(false, QStringLiteral("连接热点超时：%1").arg(expected));
            return;
        }
        // onTick每秒加1；满5次约等于5秒，再轮换候选SSID提交一次连接请求。
        if (++m_wifiAttemptSeconds >= 5)
            attemptWifiConnection();
        break;
    }
    case Phase::SettlingNetwork:
        // 第2阶段：只等待，不主动连接。
        // 给Windows约2.5秒完成DHCP和路由配置；到时开始探测设备固定地址的25001端口。
        if (m_phaseElapsed.elapsed() >= kNetworkSettleMs)
            beginDeviceConnection();
        break;
    case Phase::ConnectingDevice:
        // 第3阶段：确认192.168.1.8:25001是否可达。
        // TCP成功不是在这里判断，而是由构造函数中QTcpSocket::connected的lambda处理，
        // 该lambda会立即调用beginMqttConnection()进入ConnectingMqtt。
        if (m_phaseElapsed.elapsed() >= kDeviceConnectTimeoutMs) {
            finishCurrent(false,
                          QStringLiteral("连接设备信息端口 192.168.1.8:25001 超时"));
        } else if (++m_deviceAttemptSeconds >= 3) {
            // 连接尚未成功且没有超时，每约3秒重新发起一次异步TCP连接。
            attemptDeviceConnection();
        }
        break;
    case Phase::ConnectingMqtt:
        // 第4阶段：等待QMqttClient建立连接。
        // connectCurrentMqttPort()发起连接；errorChanged负责重试并从1883切到26001；
        // connected信号负责调用onMqttConnected()，这里仅负责30秒总超时。
        if (m_phaseElapsed.elapsed() >= kMqttTimeoutMs) {
            finishCurrent(false,
                          QStringLiteral("连接 MQTT 192.168.1.8 失败，已尝试端口 1883、26001"));
        }
        break;
    case Phase::WaitingHeartbeat:
        // 第5阶段：MQTT已连接且已订阅主题，等待设备心跳中的DeviceSN。
        // 心跳到达时messageReceived -> onMqttMessage()核对SN；核对通过才上传JSON。
        // 此分支本身只负责15秒内一直收不到有效心跳时判失败。
        if (m_phaseElapsed.elapsed() >= kHeartbeatTimeoutMs)
            finishCurrent(false, QStringLiteral("未收到设备心跳，无法核对 SN"));
        break;
    case Phase::WaitingResponse:
        // 第6阶段：JSON已经发布，等待设备真正执行save_config后的响应。
        // messageReceived -> onMqttMessage()只接受相同req_id的响应；error==0才算成功。
        // MQTT publish返回消息ID不代表设备保存成功，因此这里必须继续等，最长20秒。
        if (m_phaseElapsed.elapsed() >= kResponseTimeoutMs)
            finishCurrent(false, QStringLiteral("等待 save_config 响应超时"));
        break;
    case Phase::Idle:
        // 当前没有正在上传的设备，不进行任何轮询；开始下一台时会重新进入ConnectingWifi。
        break;
    }
}

void ConfigUploader::beginDeviceConnection()
{
    // 热点稳定后进入设备端口探测阶段，最长等待30秒。
    // 此函数由onTick()的SettlingNetwork分支在等待时间到达后调用。
    m_phase = Phase::ConnectingDevice;
    m_phaseElapsed.restart();
    m_deviceAttemptSeconds = 100;
    emit itemStateChanged(m_index, QStringLiteral("检查设备网络"),
                          QStringLiteral("192.168.1.8:25001"));
    appendLog(QStringLiteral("[%1] 等待 DHCP/路由就绪，连接设备信息端口 192.168.1.8:25001")
                  .arg(m_items.at(m_index).sn));
    // 不等待下一个tick，进入阶段后立即进行第一次25001端口探测。
    attemptDeviceConnection();
}

void ConfigUploader::attemptDeviceConnection()
{
    // 使用异步QTcpSocket探测固定设备地址；连接中时不重复发起。
    if (!m_running || m_phase != Phase::ConnectingDevice)
        return;
    if (m_deviceSocket->state() == QAbstractSocket::ConnectingState
        || m_deviceSocket->state() == QAbstractSocket::ConnectedState) {
        return;
    }
    m_deviceAttemptSeconds = 0;
    m_deviceSocket->abort();
    // connectToHost()是异步调用：这里只是开始连接；成功结果由QTcpSocket::connected通知。
    m_deviceSocket->connectToHost(QStringLiteral("192.168.1.8"), 25001);
}

void ConfigUploader::beginMqttConnection()
{
    // 此函数由QTcpSocket::connected的lambda调用，说明25001探测已通过。
    // 为当前SN创建唯一MQTT客户端ID，从1883端口开始连接。
    m_phase = Phase::ConnectingMqtt;
    m_phaseElapsed.restart();
    m_mqttPortCandidateIndex = 0;
    m_mqttPortFailureCount = 0;
    m_mqtt->setHostname(QStringLiteral("192.168.1.8"));
    m_mqtt->setClientId(QStringLiteral("slam-meta-upload-%1-%2")
                            .arg(m_items.at(m_index).sn)
                            .arg(QDateTime::currentMSecsSinceEpoch()));
    // 立即异步连接第一个候选MQTT端口；失败重试/换端口由errorChanged处理。
    connectCurrentMqttPort();
}

void ConfigUploader::connectCurrentMqttPort()
{
    // 使用当前候选端口发起异步连接，失败切换逻辑由errorChanged处理。
    if (!m_running || m_phase != Phase::ConnectingMqtt)
        return;
    const quint16 port = m_mqttPortCandidates.at(m_mqttPortCandidateIndex);
    emit itemStateChanged(m_index, QStringLiteral("连接 MQTT"),
                          QStringLiteral("192.168.1.8:%1").arg(port));
    appendLog(QStringLiteral("[%1] 连接 MQTT 192.168.1.8:%2")
                  .arg(m_items.at(m_index).sn).arg(port));
    m_mqtt->setPort(port);
    // connectToHost()异步返回；成功时发出QMqttClient::connected -> onMqttConnected()。
    m_mqtt->connectToHost();
}

void ConfigUploader::onMqttConnected()
{
    // 由QMqttClient::connected信号调用。
    // MQTT连通后先订阅设备确认响应和心跳；尚未核对SN前绝不发布配置。
    if (!m_running || m_phase != Phase::ConnectingMqtt)
        return;
    QMqttSubscription *response = m_mqtt->subscribe(
        QMqttTopicFilter(QStringLiteral("slam_app/response/save_config")), 1);
    QMqttSubscription *heartbeat = m_mqtt->subscribe(
        QMqttTopicFilter(QStringLiteral("slam_app/report/heartbeat")), 0);
    if (!response || !heartbeat) {
        finishCurrent(false, QStringLiteral("订阅 MQTT 响应或心跳主题失败"));
        return;
    }
    // 两个订阅均成功后进入WaitingHeartbeat，等messageReceived送来设备心跳。
    m_phase = Phase::WaitingHeartbeat;
    m_phaseElapsed.restart();
    emit itemStateChanged(m_index, QStringLiteral("核对设备 SN"),
                          QStringLiteral("等待设备心跳"));
    appendLog(QStringLiteral("[%1] MQTT 已连接，等待心跳核对设备 SN")
                  .arg(m_items.at(m_index).sn));
}

void ConfigUploader::onMqttDisconnected()
{
    // 已进入心跳核对或等待设备确认阶段时断线，本台上传立即失败。
    if (!m_running)
        return;
    if (m_phase == Phase::WaitingHeartbeat || m_phase == Phase::WaitingResponse)
        finishCurrent(false, QStringLiteral("上传过程中 MQTT 连接断开"));
}

void ConfigUploader::onMqttMessage(const QByteArray &message,
                                   const QMqttTopicName &topic)
{
    // 由QMqttClient::messageReceived信号调用。
    // 设备消息必须是JSON对象；其他消息直接忽略，不改变当前状态机。
    if (!m_running || m_index < 0 || m_index >= m_items.size())
        return;

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(message, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return;
    const QJsonObject object = document.object();

    if (topic.name() == QStringLiteral("slam_app/report/heartbeat")
        && m_phase == Phase::WaitingHeartbeat) {
        // 只有“心跳主题 + WaitingHeartbeat阶段”同时满足，消息才用于核对设备身份。
        // 心跳中的DeviceSN必须等于完整SN，或等于去掉开头SN后的设备上报格式。
        // 核对成功才能调用publishCurrentConfig()，避免连接错热点后误写配置。
        const QString actualSn = object.value(QStringLiteral("DeviceSN")).toString().trimmed();
        if (actualSn.isEmpty())
            return;
        const QString expectedSn = m_items.at(m_index).sn;
        const QString expectedHeartbeatSn =
            expectedSn.startsWith(QStringLiteral("SN"), Qt::CaseInsensitive)
                ? expectedSn.mid(2)
                : expectedSn;
        if (actualSn.compare(expectedSn, Qt::CaseInsensitive) != 0
            && actualSn.compare(expectedHeartbeatSn, Qt::CaseInsensitive) != 0) {
            finishCurrent(false, QStringLiteral("热点设备 SN 不匹配：期望 %1，实际 %2")
                              .arg(expectedSn, actualSn));
            return;
        }
        appendLog(QStringLiteral("[%1] 心跳 SN 核对通过（设备上报 %2）")
                      .arg(expectedSn, actualSn));
        // 身份核对通过后才读取并发布该SN对应的JSON配置。
        publishCurrentConfig();
        return;
    }

    if (topic.name() == QStringLiteral("slam_app/response/save_config")
        && m_phase == Phase::WaitingResponse) {
        // 只有“保存响应主题 + WaitingResponse阶段”同时满足，消息才作为上传结果处理。
        // 只接受req_id与当前请求完全一致的响应，旧响应或其他客户端响应会被忽略。
        const QString reqId = object.value(QStringLiteral("req_id")).toVariant().toString();
        if (reqId != m_reqId)
            return;
        const int error = object.value(QStringLiteral("error")).toInt(-1);
        const QString description = object.value(QStringLiteral("desc")).toString();
        appendLog(QStringLiteral("[%1] 收到 save_config 响应：error=%2, desc=%3")
                      .arg(m_items.at(m_index).sn).arg(error).arg(description));
        // 设备明确返回error==0才算上传并保存成功；MQTT发送成功本身不能代替此判断。
        if (error == 0)
            finishCurrent(true);
        else
            finishCurrent(false, QStringLiteral("设备返回错误 %1：%2")
                                      .arg(error).arg(description));
    }
}

void ConfigUploader::publishCurrentConfig()
{
    // 重新从磁盘读取当前JSON，确认内容仍是合法JSON对象后再组装MQTT请求。
    const ConfigUploadItem item = m_items.at(m_index);
    QFile file(item.configPath);
    if (!file.open(QIODevice::ReadOnly)) {
        finishCurrent(false, QStringLiteral("无法读取配置文件：%1").arg(file.errorString()));
        return;
    }
    QJsonParseError parseError;
    const QJsonDocument config = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !config.isObject()) {
        finishCurrent(false, QStringLiteral("配置文件不是有效 JSON 对象：%1")
                                  .arg(parseError.errorString()));
        return;
    }

    // req_id由毫秒时间戳、批次内序号和SN组成，用于关联唯一请求与设备响应。
    m_reqId = QStringLiteral("%1-%2-%3")
                  .arg(QDateTime::currentMSecsSinceEpoch())
                  .arg(m_index + 1)
                  .arg(item.sn);
    const QJsonObject request{
        {QStringLiteral("req_id"), m_reqId},
        {QStringLiteral("content"), config.object()}
    };
    const QByteArray payload = QJsonDocument(request).toJson(QJsonDocument::Compact);
    const qint32 messageId = m_mqtt->publish(
        QMqttTopicName(QStringLiteral("app_slam/request/save_config")), payload, 1, false);
    if (messageId < 0) {
        finishCurrent(false, QStringLiteral("MQTT 发布 save_config 失败"));
        return;
    }
    // publish返回消息ID只代表客户端已受理发送，不能判定设备保存成功；
    // 必须等待相同req_id且error=0的save_config响应。
    // 请求已交给MQTT客户端发送，状态转入WaitingResponse；
    // 接下来由messageReceived匹配相同req_id，或由onTick()在20秒后判超时。
    m_phase = Phase::WaitingResponse;
    m_phaseElapsed.restart();
    emit itemStateChanged(m_index, QStringLiteral("等待设备确认"), m_reqId);
    appendLog(QStringLiteral("[%1] 已发布配置 %2，req_id=%3，消息ID=%4")
                  .arg(item.sn, item.configName, m_reqId).arg(messageId));
}

void ConfigUploader::finishCurrent(bool success, const QString &error)
{
    // 把本台最终结果封装为ConfigUploadResult并发给窗口；窗口负责写SQLite。
    if (m_index < 0 || m_index >= m_items.size())
        return;
    const ConfigUploadItem item = m_items.at(m_index);
    ConfigUploadResult result;
    result.batchId = m_batchId;
    result.sn = item.sn;
    result.configName = item.configName;
    result.configPath = item.configPath;
    result.reqId = m_reqId;
    result.startedAt = m_startedAt;
    result.finishedAt = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
    result.status = success ? QStringLiteral("success")
                            : (m_stopping ? QStringLiteral("interrupted")
                                          : QStringLiteral("failed"));
    result.error = error;
    emit itemStateChanged(m_index, success ? QStringLiteral("上传成功")
                                           : QStringLiteral("上传失败"), error);
    // 窗口收到itemFinished后把本台上传结果写入共用SQLite数据库。
    emit itemFinished(m_index, result);
    appendLog(success ? QStringLiteral("[%1] 配置上传成功").arg(item.sn)
                      : QStringLiteral("[%1] 配置上传失败：%2").arg(item.sn, error));
    m_phase = Phase::Idle;
    m_deviceSocket->abort();
    if (m_mqtt->state() != QMqttClient::Disconnected)
        m_mqtt->disconnectFromHost();
    // 给上一台网络连接留出短暂清理时间，然后开始下一台设备。
    // 500毫秒后startNext()把m_index移到下一台，并再次从ConnectingWifi开始。
    QTimer::singleShot(500, this, &ConfigUploader::startNext);
}

void ConfigUploader::finishBatch(bool cancelled)
{
    // 上传批次统一收尾：停止定时器和连接，恢复界面按钮并报告完成/中止。
    m_tickTimer.stop();
    m_phase = Phase::Idle;
    m_deviceSocket->abort();
    if (m_mqtt->state() != QMqttClient::Disconnected)
        m_mqtt->disconnectFromHost();
    m_running = false;
    emit runningChanged(false);
    emit batchFinished(cancelled);
    appendLog(cancelled ? QStringLiteral("配置上传批次已中止")
                        : QStringLiteral("配置上传批次已完成"));
}

void ConfigUploader::appendLog(const QString &message)
{
    emit logMessage(QStringLiteral("[%1] %2")
                        .arg(QDateTime::currentDateTime().toString(
                                 QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")),
                             message));
}

QStringList ConfigUploader::wifiSsidCandidates(const QString &sn) const
{
    // 同时兼容当前LS200热点名和早期SLAM-完整SN热点名。
    const QString normalized = normalizedConfigDeviceSn(sn);
    QStringList candidates;
    candidates << deviceWifiSsidForSn(normalized)
               << QStringLiteral("SLAM-%1").arg(normalized);
    candidates.removeDuplicates();
    return candidates;
}

bool ConfigUploader::wifiMatchesSn(const QString &ssid, const QString &sn) const
{
    // 优先完整匹配候选SSID；同时兼容带其他型号前缀但包含唯一序列号的热点。
    if (ssid.isEmpty())
        return false;
    const QString normalized = normalizedConfigDeviceSn(sn);
    const QString suffix = normalized.startsWith(QStringLiteral("SN"))
                               ? normalized.mid(2)
                               : normalized;
    for (const QString &candidate : wifiSsidCandidates(normalized)) {
        if (ssid.compare(candidate, Qt::CaseInsensitive) == 0)
            return true;
    }
    // Match simple_qt's scan behavior: accept model prefixes such as LS200-
    // as long as the unique serial suffix is present.
    return !suffix.isEmpty() && ssid.contains(suffix, Qt::CaseInsensitive);
}

QString ConfigUploader::currentConnectedSsid() const
{
    // 通过Windows Native Wi-Fi API读取当前无线网卡实际连接的SSID。
#ifdef Q_OS_WIN
    HANDLE handle = nullptr;
    DWORD version = 0;
    if (WlanOpenHandle(2, nullptr, &version, &handle) != ERROR_SUCCESS)
        return QString();
    PWLAN_INTERFACE_INFO_LIST interfaces = nullptr;
    if (WlanEnumInterfaces(handle, nullptr, &interfaces) != ERROR_SUCCESS || !interfaces) {
        WlanCloseHandle(handle, nullptr);
        return QString();
    }
    QString result;
    for (DWORD i = 0; i < interfaces->dwNumberOfItems; ++i) {
        DWORD size = 0;
        WLAN_OPCODE_VALUE_TYPE type;
        PWLAN_CONNECTION_ATTRIBUTES attributes = nullptr;
        if (WlanQueryInterface(handle, &interfaces->InterfaceInfo[i].InterfaceGuid,
                               wlan_intf_opcode_current_connection, nullptr,
                               &size, reinterpret_cast<PVOID *>(&attributes), &type)
            == ERROR_SUCCESS && attributes) {
            const DOT11_SSID &ssid = attributes->wlanAssociationAttributes.dot11Ssid;
            result = QString::fromUtf8(reinterpret_cast<const char *>(ssid.ucSSID),
                                       static_cast<int>(ssid.uSSIDLength));
            WlanFreeMemory(attributes);
            if (!result.isEmpty())
                break;
        }
    }
    WlanFreeMemory(interfaces);
    WlanCloseHandle(handle, nullptr);
    return result;
#else
    return QString();
#endif
}

bool ConfigUploader::connectOpenWifi(const QString &ssid, QString *errorMessage) const
{
    // 给可用无线网卡安装开放热点Profile并发起连接；这里只表示连接请求已提交，
    // 是否真正连接成功由onTick()后续读取currentConnectedSsid()确认。
#ifdef Q_OS_WIN
    HANDLE handle = nullptr;
    DWORD version = 0;
    DWORD code = WlanOpenHandle(2, nullptr, &version, &handle);
    if (code != ERROR_SUCCESS) {
        if (errorMessage)
            *errorMessage = QStringLiteral("无法打开 Windows Wi-Fi 服务，错误码 %1").arg(code);
        return false;
    }
    PWLAN_INTERFACE_INFO_LIST interfaces = nullptr;
    code = WlanEnumInterfaces(handle, nullptr, &interfaces);
    if (code != ERROR_SUCCESS || !interfaces || interfaces->dwNumberOfItems == 0) {
        if (errorMessage)
            *errorMessage = QStringLiteral("未找到可用无线网卡，错误码 %1").arg(code);
        if (interfaces)
            WlanFreeMemory(interfaces);
        WlanCloseHandle(handle, nullptr);
        return false;
    }

    bool started = false;
    DWORD lastReason = 0;
    for (DWORD i = 0; i < interfaces->dwNumberOfItems && !started; ++i) {
        const GUID &guid = interfaces->InterfaceInfo[i].InterfaceGuid;
        if (!installOpenProfile(handle, guid, ssid, &lastReason))
            continue;
        WLAN_CONNECTION_PARAMETERS parameters = {};
        parameters.wlanConnectionMode = wlan_connection_mode_profile;
        parameters.strProfile = reinterpret_cast<LPCWSTR>(ssid.utf16());
        parameters.dot11BssType = dot11_BSS_type_infrastructure;
        code = WlanConnect(handle, &guid, &parameters, nullptr);
        started = code == ERROR_SUCCESS;
    }
    WlanFreeMemory(interfaces);
    WlanCloseHandle(handle, nullptr);
    if (!started && errorMessage)
        *errorMessage = QStringLiteral("WlanConnect 失败，错误码 %1，原因码 %2")
                            .arg(code).arg(lastReason);
    return started;
#else
    Q_UNUSED(ssid)
    if (errorMessage)
        *errorMessage = QStringLiteral("自动连接设备热点仅支持 Windows");
    return false;
#endif
}
