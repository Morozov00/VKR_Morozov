#pragma once

#include <QObject>
#include <QSerialPort>
#include <QSerialPortInfo>
#include <QTimer>
#include <QQueue>

// ─── Состояния ASPEP соединения ───────────────────────────────────────────────
enum class AspepState {
    Disconnected,   // порт закрыт
    BeaconWait,     // отправили BEACON, ждём ответа
    PingWait,       // отправили PING, ждём ответа
    Connected       // соединение установлено, можно слать команды
};

// ─── Состояния парсера входящих байт ─────────────────────────────────────────
enum class RxState {
    Header,   // собираем 4 байта заголовка
    Payload   // собираем N байт полезных данных
};

class SerialManager : public QObject
{
    Q_OBJECT
public:
    explicit SerialManager(QObject *parent = nullptr);
    ~SerialManager();

    static QStringList availablePorts();
    bool open(const QString &portName, int baudRate = 921600);
    void close();
    bool isOpen() const;
    bool isConnected() const { return m_aspepState == AspepState::Connected; }

    // MCP команды (ставятся в очередь, отправляются после установки ASPEP соединения)
    void sendStart();                 // START_MOTOR
    void sendStop();                  // STOP_MOTOR
    void sendFaultAck();              // FAULT_ACK — сброс ошибок
    void sendSpeedRpm(int16_t rpm);   // SET_REGISTER(SPEED_REF)
    void requestSpeed();              // GET_REGISTER(SPEED_MEAS)

    QString lastError() const { return m_lastError; }

signals:
    void dataReceived(QByteArray rawData);
    void portOpened(QString portName);
    void portClosed();
    void errorOccurred(QString error);
    void aspepConnected();            // ASPEP соединение установлено
    void aspepDisconnected();
    void speedReceived(int16_t rpm);  // получена скорость от энкодера
    void commandOk();                 // МК принял команду
    void commandFailed(uint8_t code); // МК отклонил команду

private slots:
    void onReadyRead();
    void onPortError(QSerialPort::SerialPortError error);
    void onRetryTimer();  // повторная отправка BEACON/PING

private:
    QSerialPort   *m_port;
    QString        m_lastError;
    AspepState     m_aspepState = AspepState::Disconnected;


    // Параметры соединения (согласованные с Performer)
    bool     m_crcEnabled = false;
    uint8_t  m_rxsMax = 4;   // max REQUEST payload = (rxsMax+1)*32 байт
    uint8_t  m_txsMax = 4;   // max RESPONSE payload
    uint8_t  m_txaMax = 2;   // max ASYNC payload = txaMax*64 байт

    // Парсер входящих байт
    RxState       m_rxState = RxState::Header;
    uint8_t       m_headerBuf[4];
    int           m_headerCount = 0;
    QByteArray    m_payloadBuf;
    uint16_t      m_payloadLen = 0;
    QByteArray    m_lastBeacon;  // BEACON который мы отправили

    // Очередь команд до установки соединения
    QQueue<QByteArray> m_cmdQueue;

    // Таймер повторной отправки BEACON/PING
    QTimer *m_retryTimer;
    int     m_retryCount = 0;
    static const int MAX_RETRIES = 10;

    // ─── ASPEP ────────────────────────────────────────────────────────────────
    uint8_t    computeCRCH(uint32_t header28);
    QByteArray buildBeacon();
    QByteArray buildPing();
    QByteArray buildRequestHeader(uint16_t payloadLen);

    void sendBeacon();
    void sendPing();
    bool sendRequest(const QByteArray &mcpPayload);
    bool m_wasConnected = false;

    // ─── Парсер ───────────────────────────────────────────────────────────────
    void feedByte(uint8_t byte);
    void processPacket(uint8_t type, const QByteArray &payload);
    void processBeacon(uint32_t header);
    void processPing(uint32_t header);
    void processResponse(const QByteArray &payload);
    void processAsync(const QByteArray &payload);

    // ─── MCP payload builders ─────────────────────────────────────────────────
    QByteArray mcpCommand(uint16_t cmdBase, uint8_t motorNum = 1,
                          const QByteArray &data = QByteArray());
    QByteArray mcpSetRegister(uint16_t regId, const QByteArray &value,
                              uint8_t motorNum = 1);
    QByteArray mcpGetRegister(uint16_t regId, uint8_t motorNum = 1);
};
