#include "mainwindow.h"
#include "ui_mainwindow.h"
#include <QScrollBar>
#include <QDateTime>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent),
    ui(new Ui::MainWindow),
    m_joyThread(new QThread(this)),
    m_joyWorker(new JoystickWorker()),
    m_serial1(new SerialManager(this)),
    m_serial2(new SerialManager(this)),
    m_sendTimer(new QTimer(this)),
    m_speedRequestTimer(new QTimer(this))
{
    ui->setupUi(this);
    ui->lblSpeedLock->setText("");
    setWindowTitle("Стенд электропривода — Управление");

    setStyleSheet(R"(
        QMainWindow, QWidget {
            background-color: #1a1d23; color: #e0e0e0;
            font-family: 'Segoe UI', sans-serif; font-size: 12px;
        }
        QGroupBox {
            border: 1px solid #3a3f4b; border-radius: 6px;
            margin-top: 8px; padding-top: 4px; color: #7eb8f7; font-weight: bold;
        }
        QGroupBox::title { subcontrol-origin: margin; left: 10px; }
        QPushButton {
            background-color: #2a2f3a; border: 1px solid #4a5060;
            border-radius: 4px; padding: 5px 12px; color: #d0d8f0;
        }
        QPushButton:hover { background-color: #3a4050; border-color: #7eb8f7; }
        QPushButton#btnStart {
            background-color: #1a4a1a; border-color: #40a040;
            color: #80ff80; font-weight: bold;
        }
        QPushButton#btnStop {
            background-color: #5a1a1a; border-color: #c04040;
            color: #ff8080; font-weight: bold;
        }
        QPushButton#btnApplyMode3 {
            background-color: #1a3a5a; border-color: #4080c0; color: #80c0ff;
        }
        QComboBox {
            background-color: #2a2f3a; border: 1px solid #4a5060;
            border-radius: 4px; padding: 3px 8px; color: #d0d8f0;
        }
        QProgressBar {
            border: 1px solid #3a3f4b; border-radius: 3px;
            background-color: #12151a; text-align: center; color: #fff;
        }
        QProgressBar::chunk { background-color: #3a7fd5; border-radius: 2px; }
        QTextEdit {
            background-color: #10131a; border: 1px solid #2a2f3a;
            color: #90c0a0; font-family: 'Consolas', monospace; font-size: 11px;
        }
        QCheckBox { color: #a0d0a0; }
        QRadioButton { color: #d0d8f0; padding: 3px; }
        QRadioButton::indicator:checked { background-color: #3a7fd5; border: 2px solid #7eb8f7; border-radius: 6px; }
        QSpinBox {
            background-color: #2a2f3a; border: 1px solid #4a5060;
            border-radius: 4px; padding: 3px 8px; color: #d0d8f0;
        }
    )");

    refreshPorts();
    ui->panelMode3->setVisible(false);

    // Джойстик в отдельном потоке
    m_joyWorker->moveToThread(m_joyThread);
    connect(m_joyThread, &QThread::started,  m_joyWorker, &JoystickWorker::start);
    connect(m_joyThread, &QThread::finished, m_joyWorker, &QObject::deleteLater);
    connect(m_joyWorker, &JoystickWorker::dataUpdated,
            this, &MainWindow::onJoystickData, Qt::QueuedConnection);
    connect(m_joyWorker, &JoystickWorker::joystickConnected,
            this, &MainWindow::onJoystickConnected, Qt::QueuedConnection);
    connect(m_joyWorker, &JoystickWorker::joystickDisconnected,
            this, &MainWindow::onJoystickDisconnected, Qt::QueuedConnection);
    connect(m_joyWorker, &JoystickWorker::rawAxisEvent,
            this, &MainWindow::onRawAxisEvent, Qt::QueuedConnection);
    m_joyThread->start();

    // Порт 1
    connect(m_serial1, &SerialManager::portOpened,    this, &MainWindow::onPort1Opened);
    connect(m_serial1, &SerialManager::portClosed,    this, &MainWindow::onPort1Closed);
    connect(m_serial1, &SerialManager::errorOccurred, this, &MainWindow::onPort1Error);
    connect(m_serial1, &SerialManager::aspepConnected,this, &MainWindow::onAspep1Connected);
    // connect(m_serial1, &SerialManager::commandOk,     this, [this](){
    //     appendLog("М1: команда принята ✓", "#60e060"); });
    connect(m_serial1, &SerialManager::commandFailed, this, [this](uint8_t code){
        appendLog(QString("М1: команда отклонена (0x%1)").arg(code,2,16,QChar('0')), "#ff4040"); });
    connect(m_serial1, &SerialManager::speedReceived, this, &MainWindow::onSpeed1Received);

    // Порт 2
    connect(m_serial2, &SerialManager::portOpened,    this, &MainWindow::onPort2Opened);
    connect(m_serial2, &SerialManager::portClosed,    this, &MainWindow::onPort2Closed);
    connect(m_serial2, &SerialManager::errorOccurred, this, &MainWindow::onPort2Error);
    connect(m_serial2, &SerialManager::aspepConnected,this, &MainWindow::onAspep2Connected);
    // connect(m_serial2, &SerialManager::commandOk,     this, [this](){
    //     appendLog("М2: команда принята ✓", "#60e060"); });
    connect(m_serial2, &SerialManager::commandFailed, this, [this](uint8_t code){
        appendLog(QString("М2: команда отклонена (0x%1)").arg(code,2,16,QChar('0')), "#ff4040"); });
    connect(m_serial2, &SerialManager::speedReceived, this, &MainWindow::onSpeed2Received);

    // Таймер отправки скорости 20 Гц
    m_sendTimer->setInterval(50);
    connect(m_sendTimer, &QTimer::timeout, this, &MainWindow::sendSpeed);
    m_sendTimer->start();

    // Таймер запроса скорости 5 Гц
    m_speedRequestTimer->setInterval(200);
    connect(m_speedRequestTimer, &QTimer::timeout, this, &MainWindow::requestSpeeds);
    m_speedRequestTimer->start();

    appendLog("=== Стенд электропривода запущен ===", "#7eb8f7");
    appendLog("Протокол: ASPEP + MCP | Baudrate: 921600", "#7eb8f7");
}

MainWindow::~MainWindow()
{
    m_sendTimer->stop();
    m_speedRequestTimer->stop();
    m_serial1->close();
    m_serial2->close();
    m_joyWorker->stop();
    m_joyThread->quit();
    m_joyThread->wait(2000);
    delete ui;
}

// ─── Режимы ──────────────────────────────────────────────────────────────────
void MainWindow::setMode(DriveMode mode)
{
    if (m_mode == mode) return;
    m_mode = mode;
    ui->panelMode3->setVisible(mode == DriveMode::Mode3_Manual);

    // Синхронизируем кнопки
    ui->rbMode1->setChecked(mode == DriveMode::Mode1_SameDirection);
    ui->rbMode2->setChecked(mode == DriveMode::Mode2_Opposite);
    ui->rbMode3->setChecked(mode == DriveMode::Mode3_Manual);

    QString modeStr;
    switch (mode) {
    case DriveMode::Mode1_SameDirection: modeStr = "Режим 1: двигатели в одном направлении"; break;
    case DriveMode::Mode2_Opposite:      modeStr = "Режим 2: противоположные направления"; break;
    case DriveMode::Mode3_Manual:        modeStr = "Режим 3: ручное управление"; break;
    }
    appendLog("Переключён: " + modeStr, "#f0d060");
}

void MainWindow::computeMotorSpeeds()
{
    int16_t speed = m_speedLocked ? m_lockedSpeed : m_speedRpm;

    switch (m_mode) {
    case DriveMode::Mode1_SameDirection:
        m_speedM1 = speed;
        m_speedM2 = speed;
        break;
    case DriveMode::Mode2_Opposite:
        m_speedM1 =  speed;
        m_speedM2 = -speed;
        break;
    case DriveMode::Mode3_Manual:
        // В режиме 3 скорости задаются вручную через спинбоксы
        m_speedM1 = static_cast<int16_t>(ui->spinSpeedM1->value());
        m_speedM2 = static_cast<int16_t>(ui->spinSpeedM2->value());
        break;
    }
}

// ─── Джойстик ────────────────────────────────────────────────────────────────
void MainWindow::onJoystickConnected(QString name, int axes, int buttons)
{
    ui->lblJoyStatus->setText(QString("Подключён: %1").arg(name));
    ui->lblJoyStatus->setStyleSheet("color: #60e060; font-weight: bold;");
    m_prevButtons.fill(false, buttons);
    appendLog(QString("Джойстик: %1 | Осей: %2 | Кнопок: %3")
                  .arg(name).arg(axes).arg(buttons), "#60e060");
}

void MainWindow::onJoystickDisconnected()
{
    ui->lblJoyStatus->setText("Не подключён");
    ui->lblJoyStatus->setStyleSheet("color: #e06060; font-weight: bold;");
    appendLog("Джойстик отключён!", "#e06060");
    m_speedRpm = 0;
    updateSpeedBars(0, 0);
}

void MainWindow::onRawAxisEvent(int axis, int rawValue, double normalized)
{
    // Только ось Y (axis 1)
    if (axis != 1) return;
    ui->lblAxis1->setText(QString("Ось Y: raw=%1  norm=%2")
                              .arg(rawValue).arg(normalized, 0, 'f', 3));
    ui->barAxis1->setValue(static_cast<int>((normalized + 1.0) * 100.0));
}

void MainWindow::onJoystickData(JoystickData data)
{
    if (!data.connected) return;

    // Обновляем скорость только если не заблокирована и не режим 3
    if (!m_speedLocked && m_mode != DriveMode::Mode3_Manual) {
        double speed = -data.axisY;
        m_speedRpm = static_cast<int16_t>(speed * 400.0);

        // Минимальная скорость
        static const int16_t MIN_SPEED = 50;
        if (m_speedRpm != 0 && qAbs(m_speedRpm) < MIN_SPEED)
            m_speedRpm = (m_speedRpm > 0) ? MIN_SPEED : -MIN_SPEED;
    }

    // Показываем кнопки
    QString btns;
    for (int i = 0; i < data.buttons.size(); ++i)
        if (data.buttons[i]) btns += QString("[%1] ").arg(i);
    ui->lblButtons->setText(btns.isEmpty() ? "Кнопки: -" : "Кнопки: " + btns);

    // Обрабатываем нажатия кнопок (только момент нажатия, не удержание)
    if (m_prevButtons.size() != data.buttons.size())
        m_prevButtons.resize(data.buttons.size(), false);

    for (int i = 0; i < data.buttons.size(); ++i) {
        bool pressed = data.buttons[i] && !m_prevButtons[i];  // именно нажатие
        if (pressed)
            handleJoystickButton(i, true);
        m_prevButtons[i] = data.buttons[i];
    }
}

void MainWindow::handleJoystickButton(int btnIdx, bool pressed)
{
    if (!pressed) return;

    switch (btnIdx) {
    case 0:
        // Фиксация / снятие фиксации скорости
        m_speedLocked = !m_speedLocked;
        if (m_speedLocked) {
            m_lockedSpeed = m_speedRpm;
            ui->lblSpeedLock->setText(QString("Скорость зафиксирована: %1 об/мин").arg(m_lockedSpeed));
            ui->lblSpeedLock->setStyleSheet("color: #ffaa00; font-weight: bold;");
            appendLog(QString("Скорость зафиксирована: %1 об/мин").arg(m_lockedSpeed), "#ffaa00");
        } else {
            ui->lblSpeedLock->setText("");
            ui->lblSpeedLock->setStyleSheet("");
            appendLog("Фиксация снята", "#a0d0a0");
        }
        break;

    case 1:
        setMode(DriveMode::Mode1_SameDirection);
        break;

    case 2:
        setMode(DriveMode::Mode2_Opposite);
        break;

    case 3:
        setMode(DriveMode::Mode3_Manual);
        break;

    default:
        break;
    }
}

// ─── Отправка скорости (20 Гц) ───────────────────────────────────────────────
void MainWindow::sendSpeed()
{
    if (!m_enabled || !m_motorRunning) return;

    computeMotorSpeeds();

    if (m_serial1->isConnected()) m_serial1->sendSpeedRpm(m_speedM1);
    if (m_serial2->isConnected()) m_serial2->sendSpeedRpm(m_speedM2);

    static int counter = 0;
    if (++counter >= 10) {
        counter = 0;
        appendLog(QString("TX М1:%1 М2:%2 об/мин").arg(m_speedM1).arg(m_speedM2), "#5090ff");
    }
}

void MainWindow::requestSpeeds()
{
    if (m_serial1->isConnected()) m_serial1->requestSpeed();
    if (m_serial2->isConnected()) m_serial2->requestSpeed();
}

// ─── Энкодеры ─────────────────────────────────────────────────────────────────
void MainWindow::onSpeed1Received(int16_t rpm)
{
    m_measuredRpm1 = rpm;
    updateSpeedBars(m_measuredRpm1, m_measuredRpm2);
}

void MainWindow::onSpeed2Received(int16_t rpm)
{
    m_measuredRpm2 = rpm;
    updateSpeedBars(m_measuredRpm1, m_measuredRpm2);
}

void MainWindow::updateSpeedBars(int16_t rpm1, int16_t rpm2)
{
    auto clamp = [](int v){ return qBound(0, v, 100); };
    ui->barSpeed1->setValue(clamp(50 + rpm1 * 50 / 420));
    ui->barSpeed2->setValue(clamp(50 + rpm2 * 50 / 420));
    ui->lblSpeed1->setText(QString("Задание: %1\nЭнкодер: %2 об/мин")
                               .arg(m_speedM1).arg(rpm1));
    ui->lblSpeed2->setText(QString("Задание: %1\nЭнкодер: %2 об/мин")
                               .arg(m_speedM2).arg(rpm2));
}

// ─── ASPEP ────────────────────────────────────────────────────────────────────
void MainWindow::onAspep1Connected()
{
    ui->lblPort1Status->setText("ASPEP соединён ✓");
    ui->lblPort1Status->setStyleSheet("color: #60e060; font-weight: bold;");
    appendLog("Двигатель 1: ASPEP соединение установлено!", "#60e060");
}

void MainWindow::onAspep2Connected()
{
    ui->lblPort2Status->setText("ASPEP соединён ✓");
    ui->lblPort2Status->setStyleSheet("color: #60e060; font-weight: bold;");
    appendLog("Двигатель 2: ASPEP соединение установлено!", "#60e060");
}

// ─── Порт 1 ──────────────────────────────────────────────────────────────────
void MainWindow::onPort1Opened(QString portName)
{
    ui->lblPort1Status->setText(QString("Открыт: %1 (ASPEP...)").arg(portName));
    ui->lblPort1Status->setStyleSheet("color: #f0a040; font-weight: bold;");
    ui->btnConnect1->setEnabled(false);
    ui->btnDisconnect1->setEnabled(true);
    appendLog("Двигатель 1: порт открыт — " + portName, "#f0a040");
}

void MainWindow::onPort1Closed()
{
    ui->lblPort1Status->setText("Не подключён");
    ui->lblPort1Status->setStyleSheet("color: #e06060; font-weight: bold;");
    ui->btnConnect1->setEnabled(true);
    ui->btnDisconnect1->setEnabled(false);
    appendLog("Двигатель 1: порт закрыт.", "#f0a040");
}

void MainWindow::onPort1Error(QString error)
{
    appendLog("Двигатель 1 — ошибка: " + error, "#ff4040");
}

// ─── Порт 2 ──────────────────────────────────────────────────────────────────
void MainWindow::onPort2Opened(QString portName)
{
    ui->lblPort2Status->setText(QString("Открыт: %1 (ASPEP...)").arg(portName));
    ui->lblPort2Status->setStyleSheet("color: #f0a040; font-weight: bold;");
    ui->btnConnect2->setEnabled(false);
    ui->btnDisconnect2->setEnabled(true);
    appendLog("Двигатель 2: порт открыт — " + portName, "#f0a040");
}

void MainWindow::onPort2Closed()
{
    ui->lblPort2Status->setText("Не подключён");
    ui->lblPort2Status->setStyleSheet("color: #e06060; font-weight: bold;");
    ui->btnConnect2->setEnabled(true);
    ui->btnDisconnect2->setEnabled(false);
    appendLog("Двигатель 2: порт закрыт.", "#f0a040");
}

void MainWindow::onPort2Error(QString error)
{
    appendLog("Двигатель 2 — ошибка: " + error, "#ff4040");
}

// ─── UI кнопки ───────────────────────────────────────────────────────────────
void MainWindow::on_btnRefreshPorts_clicked()
{
    refreshPorts();
    appendLog("Список портов обновлён.", "#808080");
}

void MainWindow::on_btnRefreshPorts2_clicked()
{
    refreshPorts();
    appendLog("Список портов обновлён.", "#808080");
}

void MainWindow::on_btnConnect1_clicked()
{
    QString port = ui->comboPorts1->currentText();
    if (port.isEmpty()) { appendLog("Выберите порт!", "#ff8040"); return; }
    m_serial1->open(port, 921600);
}

void MainWindow::on_btnDisconnect1_clicked() { m_serial1->close(); }

void MainWindow::on_btnConnect2_clicked()
{
    QString port = ui->comboPorts2->currentText();
    if (port.isEmpty()) { appendLog("Выберите порт!", "#ff8040"); return; }
    m_serial2->open(port, 921600);
}

void MainWindow::on_btnDisconnect2_clicked() { m_serial2->close(); }

void MainWindow::on_btnStart_clicked()
{
    m_serial1->sendFaultAck();
    m_serial2->sendFaultAck();
    QTimer::singleShot(200, this, [this](){
        m_serial1->sendStart();
        m_serial2->sendStart();
        m_motorRunning = true;
        m_enabled = true;
        ui->chkEnable->setChecked(true);
        appendLog(">>> ПУСК: FAULT_ACK + START_MOTOR", "#60e060");
    });
}

void MainWindow::on_btnStop_clicked()
{
    m_serial1->sendStop();
    m_serial2->sendStop();
    m_motorRunning = false;
    m_speedRpm = 0;
    m_speedM1 = 0;
    m_speedM2 = 0;
    m_enabled = false;
    m_speedLocked = false;
    ui->chkEnable->setChecked(false);
    ui->lblSpeedLock->setText("");
    ui->lblSpeedLock->setStyleSheet("");
    updateSpeedBars(m_measuredRpm1, m_measuredRpm2);
    appendLog(">>> СТОП: STOP_MOTOR", "#ff8080");
}

void MainWindow::on_chkEnable_toggled(bool checked)
{
    m_enabled = checked;
    if (!checked) {
        m_speedRpm = 0;
        m_serial1->sendSpeedRpm(0);
        m_serial2->sendSpeedRpm(0);
    }
    appendLog(checked ? "Управление ВКЛЮЧЕНО" : "Управление ВЫКЛЮЧЕНО",
              checked ? "#60e060" : "#f0a040");
}

void MainWindow::on_btnApplyMode3_clicked()
{
    // В режиме 3 сразу применяем значения из спинбоксов
    m_speedM1 = static_cast<int16_t>(ui->spinSpeedM1->value());
    m_speedM2 = static_cast<int16_t>(ui->spinSpeedM2->value());
    appendLog(QString("Режим 3: задание М1=%1 М2=%2 об/мин")
                  .arg(m_speedM1).arg(m_speedM2), "#f0d060");
}

// ─── Режимы через кнопки ────────────────────────────────────────────────
void MainWindow::on_rbMode1_toggled(bool checked)
{
    if (!checked) return;
    setMode(DriveMode::Mode1_SameDirection);
}

void MainWindow::on_rbMode2_toggled(bool checked)
{
    if (!checked) return;
    setMode(DriveMode::Mode2_Opposite);
}

void MainWindow::on_rbMode3_toggled(bool checked)
{
    if (!checked) return;
    setMode(DriveMode::Mode3_Manual);
}

// ─── Вспомогательные ─────────────────────────────────────────────────────────
void MainWindow::refreshPorts()
{
    QString cur1 = ui->comboPorts1->currentText();
    QString cur2 = ui->comboPorts2->currentText();
    ui->comboPorts1->clear();
    ui->comboPorts2->clear();
    for (const QString &p : SerialManager::availablePorts()) {
        ui->comboPorts1->addItem(p);
        ui->comboPorts2->addItem(p);
    }
    if (!cur1.isEmpty()) ui->comboPorts1->setCurrentText(cur1);
    if (!cur2.isEmpty()) ui->comboPorts2->setCurrentText(cur2);
}

void MainWindow::appendLog(const QString &text, const QString &color)
{
    QString ts = QDateTime::currentDateTime().toString("hh:mm:ss.zzz");
    QString html = QString("<span style='color:#505870;'>[%1]</span> "
                           "<span style='color:%2;'>%3</span>")
                       .arg(ts).arg(color).arg(text.toHtmlEscaped());
    ui->logView->append(html);
    ui->logView->verticalScrollBar()->setValue(
        ui->logView->verticalScrollBar()->maximum());
}
