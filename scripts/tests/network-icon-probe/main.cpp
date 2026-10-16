#include "connectionicon.h"
#include <QCoreApplication>
#include <QTimer>
#include <cstdio>
int main(int argc,char **argv) {
 QCoreApplication app(argc,argv);
 ConnectionIcon icon;
 QTimer::singleShot(2000,[&] {
  printf("PLASMA_ICON=%s\n",qPrintable(icon.connectionIcon()));
  printf("PRIMARY_EMPTY=%d\n",!NetworkManager::primaryConnection());
  auto connections=NetworkManager::activeConnections();
  printf("ACTIVE_CONNECTIONS=%lld\n",(long long)connections.size());
  app.exit(icon.connectionIcon()==QStringLiteral("network-wired-activated")?0:1);
 });
 return app.exec();
}
