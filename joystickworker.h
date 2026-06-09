#pragma once

#include <QObject>
#include <QThread>
#include <QMutex>
#include <QTimer>
#include <SDL.h>

// Данные джойстика (нормализованы от -1.0 до 1.0)
struct JoystickData {
    double axisX    = 0.0;   // Горизонталь (поворот)
    double axisY    = 0.0;   // Вертикаль   (скорость)
    double axisZ    = 0.0;   // Дополнительная ось (throttle/twist)
    double axisRX   = 0.0;
    double axisRY   = 0.0;
    double axisRZ   = 0.0;
    int    numAxes  = 0;
    int    numBtns  = 0;
    QVector<bool> buttons;
    bool connected  = false;
    QString name;
};

class JoystickWorker : public QObject
{
    Q_OBJECT
public:
    explicit JoystickWorker(QObject *parent = nullptr);
    ~JoystickWorker();

public slots:
    void start();
    void stop();

signals:
    void dataUpdated(JoystickData data);
    void joystickConnected(QString name, int numAxes, int numButtons);
    void joystickDisconnected();
    void rawAxisEvent(int axis, int rawValue, double normalized);

private slots:
    void poll();

private:
    SDL_Joystick *m_joystick = nullptr;
    QTimer       *m_timer    = nullptr;
    bool          m_running  = false;

    double normalizeAxis(int rawValue);
    void tryOpenJoystick();
    void closeJoystick();
};
