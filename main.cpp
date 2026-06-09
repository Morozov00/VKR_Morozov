#include "mainwindow.h"
#include <QApplication>

#ifdef main
#undef main
#endif

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    MainWindow w;
    w.show();
    return a.exec();
}
