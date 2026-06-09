#include "serialmanager.h"
#include <QDebug>
#include <QThread>

// ════════════════════════════════════════════════════════════════════════════
// Типы пакетов ASPEP (поле Type, 4 бита, в битах [3:0] заголовка)
// ════════════════════════════════════════════════════════════════════════════
static const uint8_t ASPEP_BEACON   = 5;   // обмен параметрами соединения
static const uint8_t ASPEP_PING     = 6;   // проверка соединения
static const uint8_t ASPEP_ERROR    = 15;  // ошибка от Performer (МК)
static const uint8_t ASPEP_REQUEST  = 9;   // данные от Controller(ПК) к Performer(МК)
static const uint8_t ASPEP_RESPONSE = 10;  // ответ от Performer (МК)
static const uint8_t ASPEP_ASYNC    = 9;   // асинхронные данные от Performer (МК)

// ASPEP Version
static const uint8_t ASPEP_VERSION = 0;

// Параметры BEACON которые передаются Performer-у (МК)
// RXS Max = 4 → max REQUEST payload = (4+1)*32 = 160 байт
// TXS Max = 4 → max RESPONSE payload = (4+1)*32 = 160 байт
// TXA Max = 2 → max ASYNC payload = 2*64 = 128 байт
// CRC = 0 → без CRC
static const uint8_t BEACON_RXS = 4;
static const uint8_t BEACON_TXS = 4;
static const uint8_t BEACON_TXA = 2;
static const bool    BEACON_CRC = false;

// ════════════════════════════════════════════════════════════════════════════
// MCP Commands (16-bit word = cmd_id << 3 | motor_num)
// ════════════════════════════════════════════════════════════════════════════
static const uint16_t MCP_GET_VERSION  = 0x0000; // id=0
static const uint16_t MCP_SET_REGISTER = 0x0008; // id=1
static const uint16_t MCP_GET_REGISTER = 0x0010; // id=2
static const uint16_t MCP_START_MOTOR  = 0x0018; // id=3
static const uint16_t MCP_STOP_MOTOR   = 0x0020; // id=4
static const uint16_t MCP_STOP_RAMP    = 0x0028; // id=5
static const uint16_t MCP_START_STOP   = 0x0030; // id=6
static const uint16_t MCP_FAULT_ACK    = 0x0038; // id=7

// MCP Response codes
static const uint8_t MCP_CMD_OK  = 0x00;
static const uint8_t MCP_CMD_NOK = 0x01;

// ════════════════════════════════════════════════════════════════════════════
// Регистры MCP
// Формула: (identifier << ELT_IDENTIFIER_POS=6) | (type << TYPE_POS=3) | motorNum
// TYPE_DATA_32BIT = 3 << 3 = 0x18
// TYPE_DATA_16BIT = 2 << 3 = 0x10
// TYPE_DATA_8BIT  = 1 << 3 = 0x08
// ════════════════════════════════════════════════════════════════════════════
// Регистры: базовый ID | Motor# в битах [2:0]
// MC_REG_SPEED_REF  = (2<<6)|(3<<3) = 0x98, мотор 1 = 0x99
// MC_REG_SPEED_MEAS = (1<<6)|(3<<3) = 0x58, мотор 1 = 0x59
// MC_REG_STATUS     = (1<<6)|(1<<3) = 0x48, мотор 1 = 0x49
static const uint16_t REG_SPEED_REF_M1  = 0x0099;  // задание скорости, мотор 1
static const uint16_t REG_SPEED_MEAS_M1 = 0x0059;  // измеренная скорость, мотор 1
static const uint16_t REG_STATUS_M1     = 0x0049;  // статус, мотор 1

// Intra Packet Pause: 1ms между заголовком и полезными данными REQUEST пакета
static const int INTRA_PACKET_PAUSE_MS = 1;

// Таймаут повторной отправки BEACON/PING
static const int RETRY_INTERVAL_MS = 200;

// ════════════════════════════════════════════════════════════════════════════
// SerialManager
// ════════════════════════════════════════════════════════════════════════════
SerialManager::SerialManager(QObject *parent)
    : QObject(parent)
    , m_port(new QSerialPort(this))
    , m_retryTimer(new QTimer(this))
{
    connect(m_port, &QSerialPort::readyRead,
            this,   &SerialManager::onReadyRead);
    connect(m_port, &QSerialPort::errorOccurred,
            this,   &SerialManager::onPortError);
    connect(m_retryTimer, &QTimer::timeout,
            this,          &SerialManager::onRetryTimer);
}

SerialManager::~SerialManager()
{
    close();
}

QStringList SerialManager::availablePorts()
{
    QStringList list;
    for (const QSerialPortInfo &info : QSerialPortInfo::availablePorts())
        list << info.portName();
    return list;
}

bool SerialManager::open(const QString &portName, int baudRate)
{
    if (m_port->isOpen()) m_port->close();

    m_port->setPortName(portName);
    m_port->setBaudRate(baudRate);
    m_port->setDataBits(QSerialPort::Data8);
    m_port->setParity(QSerialPort::NoParity);
    m_port->setStopBits(QSerialPort::OneStop);
    m_port->setFlowControl(QSerialPort::NoFlowControl);

    if (!m_port->open(QIODevice::ReadWrite)) {
        m_lastError = m_port->errorString();
        emit errorOccurred(m_lastError);
        return false;
    }

    // Сбрасываем состояние
    m_aspepState = AspepState::Disconnected;
    m_rxState    = RxState::Header;
    m_headerCount = 0;
    m_payloadBuf.clear();
    m_retryCount = 0;

    emit portOpened(portName);

    // Начинаем процедуру установки ASPEP соединения
    sendBeacon();

    return true;
}

void SerialManager::close()
{
    m_wasConnected = false;
    m_retryTimer->stop();
    if (m_port->isOpen()) {
        if (m_aspepState == AspepState::Connected) {
            // Останавливаем мотор перед закрытием
            QByteArray stopCmd = mcpCommand(MCP_STOP_MOTOR, 1);
            sendRequest(stopCmd);
            QThread::msleep(50);
        }
        m_aspepState = AspepState::Disconnected;
        m_port->close();
        emit portClosed();
        emit aspepDisconnected();
    }
}

bool SerialManager::isOpen() const
{
    return m_port->isOpen();
}

// ════════════════════════════════════════════════════════════════════════════
// MCP команды
// ════════════════════════════════════════════════════════════════════════════
void SerialManager::sendStart()
{
    QByteArray cmd = mcpCommand(MCP_START_MOTOR, 1);
    if (m_aspepState == AspepState::Connected)
        sendRequest(cmd);
    else
        m_cmdQueue.enqueue(cmd);
}

void SerialManager::sendStop()
{
    QByteArray cmd = mcpCommand(MCP_STOP_MOTOR, 1);
    if (m_aspepState == AspepState::Connected)
        sendRequest(cmd);
    else
        m_cmdQueue.enqueue(cmd);
}

void SerialManager::sendFaultAck()
{
    QByteArray cmd = mcpCommand(MCP_FAULT_ACK, 1);
    if (m_aspepState == AspepState::Connected)
        sendRequest(cmd);
    else
        m_cmdQueue.enqueue(cmd);
}

void SerialManager::sendSpeedRpm(int16_t rpm)
{

    int32_t speedUnit = (int32_t)rpm;
    QByteArray val;
    val.append((char)( speedUnit        & 0xFF));
    val.append((char)((speedUnit >>  8) & 0xFF));
    val.append((char)((speedUnit >> 16) & 0xFF));
    val.append((char)((speedUnit >> 24) & 0xFF));
    QByteArray cmd = mcpSetRegister(REG_SPEED_REF_M1, val, 0);
    if (m_aspepState == AspepState::Connected)
        sendRequest(cmd);
    else
        m_cmdQueue.enqueue(cmd);
}

void SerialManager::requestSpeed()
{
    QByteArray cmd = mcpGetRegister(REG_SPEED_MEAS_M1, 0);
    if (m_aspepState == AspepState::Connected)
        sendRequest(cmd);
    // не ставим в очередь — периодический запрос
}

// ════════════════════════════════════════════════════════════════════════════
// ASPEP: вычисление CRCH (4-bit CRC заголовка)
// ════════════════════════════════════════════════════════════════════════════
uint8_t SerialManager::computeCRCH(uint32_t header28)
{
    // Полином: x^4 + x^2 + x + 1 = 0x7
    // Документ указывает x^4 + x + 1 (0x3
    uint8_t crc = 0;
    for (int nibble = 0; nibble < 7; nibble++) {
        uint8_t n = (header28 >> (nibble * 4)) & 0x0F;
        for (int bit = 3; bit >= 0; bit--) {  // MSB нибла первым
            uint8_t b = (n >> bit) & 1;
            uint8_t msb = (crc >> 3) & 1;
            uint8_t feedback = msb ^ b;
            crc = (crc << 1) & 0x0F;
            if (feedback)
                crc ^= 0x07;  // x^4 + x^2 + x + 1
        }
    }
    return crc & 0x0F;
}

// ════════════════════════════════════════════════════════════════════════════
// ASPEP: построение пакетов
// ════════════════════════════════════════════════════════════════════════════

// BEACON пакет (4 байта, без полезных данных):
// bits [3:0]  = Type = 5
// bits [6:4]  = Version = 0
// bit  [7]    = CRC support = 0
// bits [13:8] = RXS Max
// bits [20:14]= TXS Max
// bits [27:21]= TXA Max
// bits [31:28]= CRCH
QByteArray SerialManager::buildBeacon()
{
    uint32_t h = ASPEP_BEACON;
    h |= (uint32_t)ASPEP_VERSION        <<  4;
    h |= (uint32_t)(BEACON_CRC ? 1 : 0) <<  7;
    h |= (uint32_t)BEACON_RXS           <<  8;
    h |= (uint32_t)BEACON_TXS           << 14;
    h |= (uint32_t)BEACON_TXA           << 21;
    uint8_t crch = computeCRCH(h & 0x0FFFFFFF);
    h |= (uint32_t)crch << 28;

    QByteArray pkt(4, 0);
    pkt[0] = (h      ) & 0xFF;
    pkt[1] = (h >>  8) & 0xFF;
    pkt[2] = (h >> 16) & 0xFF;
    pkt[3] = (h >> 24) & 0xFF;
    return pkt;
}

// PING пакет (4 байта, без полезных данных):
// bits [3:0]  = Type = 6
// bits [4]    = C = 0 (Controller не устанавливает)
// bits [5]    = C (дубль)
// bits [6]    = N = 0
// bits [7]    = N (дубль)
// bits [11:8] = LIID = 0
// bits [27:12]= Packet Number = 0
// bits [31:28]= CRCH
QByteArray SerialManager::buildPing()
{
    uint32_t h = ASPEP_PING;
    // C, N, LIID, PacketNumber все = 0 для Controller
    uint8_t crch = computeCRCH(h & 0x0FFFFFFF);
    h |= (uint32_t)crch << 28;

    QByteArray pkt(4, 0);
    pkt[0] = (h      ) & 0xFF;
    pkt[1] = (h >>  8) & 0xFF;
    pkt[2] = (h >> 16) & 0xFF;
    pkt[3] = (h >> 24) & 0xFF;
    return pkt;
}

// REQUEST заголовок (4 байта):
// bits [3:0]   = Type = 9
// bits [16:4]  = Payload Length (13 бит)
// bits [27:17] = Reserved = 0
// bits [31:28] = CRCH
QByteArray SerialManager::buildRequestHeader(uint16_t payloadLen)
{
    uint32_t h = ASPEP_REQUEST;
    h |= (uint32_t)(payloadLen & 0x1FFF) << 4;
    uint8_t crch = computeCRCH(h & 0x0FFFFFFF);
    h |= (uint32_t)crch << 28;

    QByteArray hdr(4, 0);
    hdr[0] = (h      ) & 0xFF;
    hdr[1] = (h >>  8) & 0xFF;
    hdr[2] = (h >> 16) & 0xFF;
    hdr[3] = (h >> 24) & 0xFF;
    return hdr;
}

// ════════════════════════════════════════════════════════════════════════════
// Отправка ASPEP пакетов
// ════════════════════════════════════════════════════════════════════════════
void SerialManager::sendBeacon()
{
    m_lastBeacon = buildBeacon();
    m_port->write(m_lastBeacon);
    m_port->flush();
    m_aspepState = AspepState::BeaconWait;
    m_retryTimer->start(RETRY_INTERVAL_MS);
    qDebug() << "ASPEP TX BEACON:" << m_lastBeacon.toHex(' ').toUpper();
}

void SerialManager::sendPing()
{
    QByteArray ping = buildPing();
    m_port->write(ping);
    m_port->flush();
    m_aspepState = AspepState::PingWait;
    m_retryTimer->start(RETRY_INTERVAL_MS);
    qDebug() << "ASPEP TX PING:" << ping.toHex(' ').toUpper();
}

// REQUEST пакет = заголовок + 1ms пауза + полезные данные
bool SerialManager::sendRequest(const QByteArray &mcpPayload)
{
    if (!m_port->isOpen()) return false;
    if (mcpPayload.isEmpty()) return false;

    QByteArray header = buildRequestHeader(static_cast<uint16_t>(mcpPayload.size()));

    // Отправляем заголовок
    m_port->write(header);
    m_port->flush();

    // Intra Packet Pause: 1ms (нужен Performer-у для настройки DMA)
    QThread::msleep(INTRA_PACKET_PAUSE_MS);

    // Отправляем полезные данные
    m_port->write(mcpPayload);
    m_port->flush();

    qDebug() << "ASPEP TX REQUEST hdr:" << header.toHex(' ').toUpper()
             << "payload:" << mcpPayload.toHex(' ').toUpper();
    return true;
}

// ════════════════════════════════════════════════════════════════════════════
// Таймер повторной отправки
// ════════════════════════════════════════════════════════════════════════════
void SerialManager::onRetryTimer()
{
    m_retryCount++;
    if (m_retryCount > MAX_RETRIES) {
        m_retryTimer->stop();
        m_retryCount = 0;
        emit errorOccurred("ASPEP: нет ответа от МК после " +
                           QString::number(MAX_RETRIES) + " попыток");
        return;
    }

    switch (m_aspepState) {
    case AspepState::BeaconWait:
        qDebug() << "ASPEP retry BEACON #" << m_retryCount;
        m_port->write(m_lastBeacon);
        m_port->flush();
        break;
    case AspepState::PingWait:
        qDebug() << "ASPEP retry PING #" << m_retryCount;
        sendPing();
        break;
    default:
        m_retryTimer->stop();
        break;
    }
}

// ════════════════════════════════════════════════════════════════════════════
// Парсер входящих байт
// ════════════════════════════════════════════════════════════════════════════
void SerialManager::onReadyRead()
{
    QByteArray data = m_port->readAll();
    emit dataReceived(data);
    for (unsigned char b : data)
        feedByte(b);
}

void SerialManager::feedByte(uint8_t byte)
{
    switch (m_rxState) {
    case RxState::Header:
        m_headerBuf[m_headerCount++] = byte;
        if (m_headerCount == 4) {
            m_headerCount = 0;

            // Разбираем заголовок
            uint32_t h = (uint32_t)m_headerBuf[0]
                         | ((uint32_t)m_headerBuf[1] <<  8)
                         | ((uint32_t)m_headerBuf[2] << 16)
                         | ((uint32_t)m_headerBuf[3] << 24);

            uint8_t type = h & 0x0F;
            uint8_t crch = (h >> 28) & 0x0F;

            // Проверка CRCH
            if (computeCRCH(h & 0x0FFFFFFF) != crch) {
                qWarning() << "ASPEP RX: плохой CRCH! Игнорируем.";
                // Не сбрасываем — продолжаем искать следующий пакет
                return;
            }

            qDebug() << "ASPEP RX header type=" << type
                     << "bytes:" << QByteArray((char*)m_headerBuf, 4).toHex(' ').toUpper();

            // Пакеты без полезных данных (только заголовок)
            if (type == ASPEP_BEACON) {
                processBeacon(h);
                return;
            }
            if (type == ASPEP_PING) {
                processPing(h);
                return;
            }
            if (type == ASPEP_ERROR) {
                uint8_t errCode = (h >> 8) & 0xFF;
                qWarning() << "ASPEP RX ERROR code=" << errCode;
                // При ошибке перезапускаем соединение
                sendBeacon();
                return;
            }

            // Пакеты с полезными данными (RESPONSE=10 или ASYNC=9 от Performer)
            if (type == ASPEP_RESPONSE || type == ASPEP_REQUEST) {
                // Payload length = bits [16:4] = 13 бит
                m_payloadLen = (h >> 4) & 0x1FFF;
                if (m_payloadLen == 0) {
                    // Пустой payload — неожиданно, игнорируем
                    return;
                }
                m_payloadBuf.clear();
                m_payloadBuf.reserve(m_payloadLen);
                m_rxState = RxState::Payload;
                // Сохраняем тип для обработки после приёма payload
                m_headerBuf[0] = type;
            }
        }
        break;

    case RxState::Payload:
        m_payloadBuf.append((char)byte);
        if (m_payloadBuf.size() >= m_payloadLen) {
            // Payload собран
            // CRC на payload не согласован (BEACON_CRC=false) → пропускаем
            uint8_t pktType = m_headerBuf[0];
            processPacket(pktType, m_payloadBuf);
            m_payloadBuf.clear();
            m_rxState = RxState::Header;
        }
        break;
    }
}

void SerialManager::processPacket(uint8_t type, const QByteArray &payload)
{
    if (type == ASPEP_RESPONSE) {
        processResponse(payload);
    } else if (type == ASPEP_REQUEST) {
        // ASYNC от Performer (тип 9 входящий = ASYNC)
        processAsync(payload);
    }
}

// ════════════════════════════════════════════════════════════════════════════
// Обработка входящих пакетов
// ════════════════════════════════════════════════════════════════════════════

// Обработка BEACON от Performer
// Процедура соединения из документации:
// 1. Controller шлёт BEACON
// 2. Performer отвечает BEACON с согласованными параметрами
// 3. Если BEACON идентичны → Controller шлёт PING
// 4. Если нет → Controller шлёт BEACON Performer-а обратно и ждёт
void SerialManager::processBeacon(uint32_t header)
{
    if (m_aspepState != AspepState::BeaconWait &&
        m_aspepState != AspepState::Connected) return;

    m_retryTimer->stop();
    m_retryCount = 0;

    QByteArray receivedBeacon(4, 0);
    receivedBeacon[0] = (header      ) & 0xFF;
    receivedBeacon[1] = (header >>  8) & 0xFF;
    receivedBeacon[2] = (header >> 16) & 0xFF;
    receivedBeacon[3] = (header >> 24) & 0xFF;

    qDebug() << "ASPEP RX BEACON:" << receivedBeacon.toHex(' ').toUpper();

    if (receivedBeacon == m_lastBeacon) {
        // Performer принял наши параметры → переходим к PING
        qDebug() << "ASPEP BEACON совпадает → отправляем PING";
        sendPing();
    } else {
        // Performer предложил другие параметры → отправляем его BEACON обратно
        qDebug() << "ASPEP BEACON отличается → отправляем BEACON Performer-а обратно";
        m_lastBeacon = receivedBeacon;
        m_port->write(m_lastBeacon);
        m_port->flush();
        m_aspepState = AspepState::BeaconWait;
        m_retryTimer->start(RETRY_INTERVAL_MS);
    }
}

// Обработка PING от Performer (подтверждение соединения)
void SerialManager::processPing(uint32_t header)
{
    if (m_aspepState != AspepState::PingWait) return;
    m_retryTimer->stop();
    m_retryCount = 0;
    m_aspepState = AspepState::Connected;

    if (!m_wasConnected) {       // только первый раз
        m_wasConnected = true;
        emit aspepConnected();
    }

    while (!m_cmdQueue.isEmpty()) {
        sendRequest(m_cmdQueue.dequeue());
        QThread::msleep(10);
    }
}

// Обработка RESPONSE (ответ на нашу команду)
void SerialManager::processResponse(const QByteArray &payload)
{
    if (payload.isEmpty()) return;

    // Последний байт RESPONSE = статус код
    uint8_t statusCode = (uint8_t)payload[payload.size() - 1];

    qDebug() << "MCP RESPONSE status=" << statusCode
             << "payload:" << payload.toHex(' ').toUpper();

    if (statusCode == MCP_CMD_OK) {
        // Если есть данные перед статусом — это ответ на GET_REGISTER
        if (payload.size() > 1) {
            // Данные = payload без последнего байта (статус)
            QByteArray data = payload.left(payload.size() - 1);

            // Если получили 4 байта — это int32 скорость
            if (data.size() >= 4) {
                int32_t speedUnit = (int32_t)(
                    ((uint32_t)(uint8_t)data[0]      ) |
                    ((uint32_t)(uint8_t)data[1] <<  8) |
                    ((uint32_t)(uint8_t)data[2] << 16) |
                    ((uint32_t)(uint8_t)data[3] << 24));
                int16_t rpm = static_cast<int16_t>(speedUnit);
                qDebug() << "MCP GET_REGISTER speed:" << rpm << "об/мин";
                emit speedReceived(rpm);
            }
        }
        emit commandOk();
    } else {
        qWarning() << "MCP CMD_NOK code=" << statusCode;
        emit commandFailed(statusCode);
    }
}

// Обработка ASYNC (данные datalog от Performer)
void SerialManager::processAsync(const QByteArray &payload)
{
    qDebug() << "ASPEP RX ASYNC:" << payload.toHex(' ').toUpper();
    // Datalog формат: [Timestamp 4б][HF/MF данные...]
    emit dataReceived(payload);
}

// ════════════════════════════════════════════════════════════════════════════
// MCP Payload builders
// ════════════════════════════════════════════════════════════════════════════

// Общий построитель MCP команды
// cmd_word = cmdBase | motorNum (cmdBase содержит cmd_id<<3, motorNum в bits[2:0])
QByteArray SerialManager::mcpCommand(uint16_t cmdBase, uint8_t motorNum,
                                     const QByteArray &data)
{
    uint16_t cmdWord = cmdBase | (motorNum & 0x07);
    QByteArray payload;
    payload.append((char)(cmdWord & 0xFF));
    payload.append((char)((cmdWord >> 8) & 0xFF));
    payload.append(data);
    return payload;
}

// SET_REGISTER команда
QByteArray SerialManager::mcpSetRegister(uint16_t regId, const QByteArray &value,
                                         uint8_t motorNum)
{
    QByteArray regData;
    regData.append((char)(regId & 0xFF));
    regData.append((char)((regId >> 8) & 0xFF));
    regData.append(value);
    return mcpCommand(MCP_SET_REGISTER, motorNum, regData);
}

// GET_REGISTER команда
QByteArray SerialManager::mcpGetRegister(uint16_t regId, uint8_t motorNum)
{
    QByteArray regData;
    regData.append((char)(regId & 0xFF));
    regData.append((char)((regId >> 8) & 0xFF));
    return mcpCommand(MCP_GET_REGISTER, motorNum, regData);
}

// ════════════════════════════════════════════════════════════════════════════
// Обработка ошибок порта
// ════════════════════════════════════════════════════════════════════════════
void SerialManager::onPortError(QSerialPort::SerialPortError error)
{
    if (error == QSerialPort::NoError) return;
    m_lastError = m_port->errorString();
    emit errorOccurred(m_lastError);
}
