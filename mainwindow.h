#pragma once

#include <QMainWindow>
#include <QThread>
#include <QTimer>
#include "joystickworker.h"
#include "serialmanager.h"

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

// Режимы управления
enum class DriveMode {
    Mode1_SameDirection,   // оба двигателя в одну сторону
    Mode2_Opposite,        // двигатели в разные стороны
    Mode3_Manual           // ручное задание скорости
};

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private slots:
    // Джойстик
    void onJoystickData(JoystickData data);
    void onJoystickConnected(QString name, int axes, int buttons);
    void onJoystickDisconnected();
    void onRawAxisEvent(int axis, int rawValue, double normalized);

    // Порты
    void onPort1Opened(QString portName);
    void onPort1Closed();
    void onPort1Error(QString error);
    void onAspep1Connected();
    void onSpeed1Received(int16_t rpm);

    void onPort2Opened(QString portName);
    void onPort2Closed();
    void onPort2Error(QString error);
    void onAspep2Connected();
    void onSpeed2Received(int16_t rpm);

    // UI кнопки
    void on_btnRefreshPorts_clicked();
    void on_btnRefreshPorts2_clicked();
    void on_btnConnect1_clicked();
    void on_btnDisconnect1_clicked();
    void on_btnConnect2_clicked();
    void on_btnDisconnect2_clicked();
    void on_btnStart_clicked();
    void on_btnStop_clicked();
    void on_chkEnable_toggled(bool checked);
    void on_btnApplyMode3_clicked();

    // Режимы
    void on_rbMode1_toggled(bool checked);
    void on_rbMode2_toggled(bool checked);
    void on_rbMode3_toggled(bool checked);

    // Таймеры
    void sendSpeed();
    void requestSpeeds();

private:
    Ui::MainWindow *ui;

    QThread        *m_joyThread;
    JoystickWorker *m_joyWorker;
    SerialManager  *m_serial1;
    SerialManager  *m_serial2;

    QTimer  *m_sendTimer;
    QTimer  *m_speedRequestTimer;

    // Скорости
    int16_t  m_speedRpm      = 0;   // задание с джойстика
    int16_t  m_speedM1       = 0;   // итоговое задание мотор 1
    int16_t  m_speedM2       = 0;   // итоговое задание мотор 2
    int16_t  m_measuredRpm1  = 0;   // энкодер 1
    int16_t  m_measuredRpm2  = 0;   // энкодер 2

    // Состояние
    bool      m_enabled      = false;
    bool      m_motorRunning = false;
    bool      m_speedLocked  = false;  // фиксация скорости (кнопка 0)
    int16_t   m_lockedSpeed  = 0;      // зафиксированное значение
    DriveMode m_mode         = DriveMode::Mode1_SameDirection;

    // Предыдущее состояние кнопок (для определения нажатия)
    QVector<bool> m_prevButtons;

    void refreshPorts();
    void appendLog(const QString &text, const QString &color = "#c0c0c0");
    void updateSpeedBars(int16_t rpm1, int16_t rpm2);
    void setMode(DriveMode mode);
    void handleJoystickButton(int btnIdx, bool pressed);
    void computeMotorSpeeds();  // вычислить m_speedM1, m_speedM2 из m_speedRpm
};
