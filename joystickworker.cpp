#include "joystickworker.h"
#include <QDebug>

// Мёртвая зона — значения оси в пределах [-DEADZONE, DEADZONE] считаются нулём
static const double DEADZONE = 0.04;

JoystickWorker::JoystickWorker(QObject *parent)
    : QObject(parent)
{
}

JoystickWorker::~JoystickWorker()
{
    stop();
}

void JoystickWorker::start()
{
    if (SDL_Init(SDL_INIT_JOYSTICK) < 0) {
        qWarning() << "SDL_Init failed:" << SDL_GetError();
        return;
    }
    SDL_JoystickEventState(SDL_ENABLE);

    m_timer = new QTimer(this);
    m_timer->setInterval(20); // 50 Гц опрос
    connect(m_timer, &QTimer::timeout, this, &JoystickWorker::poll);

    m_running = true;
    tryOpenJoystick();
    m_timer->start();
}

void JoystickWorker::stop()
{
    m_running = false;
    if (m_timer) {
        m_timer->stop();
        m_timer->deleteLater();
        m_timer = nullptr;
    }
    closeJoystick();
    SDL_Quit();
}

void JoystickWorker::poll()
{
    // Обрабатываем события SDL (нужно для корректной работы hot-plug)
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_JOYDEVICEADDED) {
            if (!m_joystick) {
                tryOpenJoystick();
            }
        } else if (event.type == SDL_JOYDEVICEREMOVED) {
            closeJoystick();
            emit joystickDisconnected();
        }
    }

    if (!m_joystick) {
        // Продолжаем пытаться найти джойстик
        if (SDL_NumJoysticks() > 0) {
            tryOpenJoystick();
        }
        return;
    }

    SDL_JoystickUpdate();

    JoystickData data;
    data.connected = true;
    data.name      = SDL_JoystickName(m_joystick);
    data.numAxes   = SDL_JoystickNumAxes(m_joystick);
    data.numBtns   = SDL_JoystickNumButtons(m_joystick);

    // Читаем все оси и сигналим об изменениях
    auto readAxis = [&](int idx) -> double {
        if (idx >= data.numAxes) return 0.0;
        int raw = SDL_JoystickGetAxis(m_joystick, idx);
        double norm = normalizeAxis(raw);
        emit rawAxisEvent(idx, raw, norm);
        return norm;
    };

    data.axisX  = readAxis(0);
    data.axisY  = readAxis(1);
    data.axisZ  = readAxis(2);
    data.axisRX = readAxis(3);
    data.axisRY = readAxis(4);
    data.axisRZ = readAxis(5);

    // Кнопки
    data.buttons.resize(data.numBtns);
    for (int i = 0; i < data.numBtns; ++i) {
        data.buttons[i] = SDL_JoystickGetButton(m_joystick, i) != 0;
    }

    emit dataUpdated(data);
}

double JoystickWorker::normalizeAxis(int rawValue)
{
    // SDL возвращает -32768..32767
    double norm = rawValue / 32767.0;
    if (norm < -1.0) norm = -1.0;
    if (norm >  1.0) norm =  1.0;

    // Применяем мёртвую зону
    if (qAbs(norm) < DEADZONE) norm = 0.0;

    return norm;
}

void JoystickWorker::tryOpenJoystick()
{
    int n = SDL_NumJoysticks();
    if (n <= 0) return;

    m_joystick = SDL_JoystickOpen(0);
    if (!m_joystick) {
        qWarning() << "Cannot open joystick:" << SDL_GetError();
        return;
    }

    QString name    = SDL_JoystickName(m_joystick);
    int     axes    = SDL_JoystickNumAxes(m_joystick);
    int     buttons = SDL_JoystickNumButtons(m_joystick);

    qDebug() << "Joystick opened:" << name
             << "axes:" << axes
             << "buttons:" << buttons;

    emit joystickConnected(name, axes, buttons);
}

void JoystickWorker::closeJoystick()
{
    if (m_joystick) {
        SDL_JoystickClose(m_joystick);
        m_joystick = nullptr;
    }
}
