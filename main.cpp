#include "mainwindow.h"

#include <QApplication>
#include <QFont>

// 程序总入口。MainWindow 构造函数会继续完成界面创建、数据库打开、
// BatchRunner 信号连接以及第一次设备扫描。
int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("SLAM META 批量解析验证工具"));
    app.setOrganizationName(QStringLiteral("LS Calibration"));
    app.setFont(QFont(QStringLiteral("Microsoft YaHei UI"), 10));//设置程序名、组织名、默认字体

    MainWindow window;
    window.show();
    // 进入 Qt 事件循环。按钮点击、Docker 输出、进程结束和定时器超时
    // 都会从这里分发到对应的信号槽函数。
    return app.exec();
}

