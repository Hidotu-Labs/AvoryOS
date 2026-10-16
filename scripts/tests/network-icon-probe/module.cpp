#include <QCoreApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QTimer>
#include <cstdio>
int main(int argc,char **argv) {
 QCoreApplication app(argc,argv);
 QQmlEngine engine;
 QQmlComponent component(&engine);
 component.setData("import org.kde.plasma.networkmanagement\nConnectionIcon {}",QUrl());
 QObject *icon=component.create();
 if(!icon) { fprintf(stderr,"QML error: %s\n",qPrintable(component.errorString()));return 2; }
 QTimer::singleShot(2000,[&] {
  QString value=icon->property("connectionIcon").toString();
  printf("PLASMA_MODULE_ICON=%s\n",qPrintable(value));
  app.exit(value==QStringLiteral("network-wired-activated")?0:1);
 });
 int result=app.exec();delete icon;return result;
}
