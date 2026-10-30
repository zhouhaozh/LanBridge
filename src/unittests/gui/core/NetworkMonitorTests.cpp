/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Chris Rizzitello <sithlord48@gmail.com>
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "NetworkMonitorTests.h"

#include "gui/core/NetworkMonitor.h"

#include <QSignalSpy>

using namespace deskflow::gui;
void NetworkMonitorTests::testVirtualInterface()
{
  QVERIFY(NetworkMonitor::isVirtualInterface(QStringLiteral("vboxnet0")));
  QVERIFY(NetworkMonitor::isVirtualInterface(QStringLiteral("vboXnet0")));
  QVERIFY(NetworkMonitor::isVirtualInterface(QStringLiteral("vmnet0")));
  QVERIFY(NetworkMonitor::isVirtualInterface(QStringLiteral("docker-bridge")));
  QVERIFY(NetworkMonitor::isVirtualInterface(QStringLiteral("virbr0")));
  QVERIFY(NetworkMonitor::isVirtualInterface(QStringLiteral("veth0")));
  QVERIFY(NetworkMonitor::isVirtualInterface(QStringLiteral("br-0")));
  QVERIFY(NetworkMonitor::isVirtualInterface(QStringLiteral("tun0")));
  QVERIFY(NetworkMonitor::isVirtualInterface(QStringLiteral("utun0")));
  QVERIFY(NetworkMonitor::isVirtualInterface(QStringLiteral("awdl0")));
  QVERIFY(NetworkMonitor::isVirtualInterface(QStringLiteral("p2p0")));
  QVERIFY(NetworkMonitor::isVirtualInterface(QStringLiteral("llw0")));
  QVERIFY(NetworkMonitor::isVirtualInterface(QStringLiteral("anpi5")));
  QVERIFY(NetworkMonitor::isVirtualInterface(QStringLiteral("tap1")));

  QVERIFY(!NetworkMonitor::isVirtualInterface(QStringLiteral("eth0")));
  QVERIFY(!NetworkMonitor::isVirtualInterface(QStringLiteral("enp0s0f")));
  QVERIFY(!NetworkMonitor::isVirtualInterface(QStringLiteral("wifi")));
  QVERIFY(!NetworkMonitor::isVirtualInterface(QStringLiteral("wlan0")));
}

void NetworkMonitorTests::defaultRoutePrefersWifiOverLowerNumberedEthernet()
{
  const QStringList addresses{"192.168.10.1", "192.168.50.219", "172.17.0.1", "fd00::1"};
  const QHash<QString, QStringList> interfaces{
      {"eno2", {"192.168.10.1"}}, {"wlp3s0", {"192.168.50.219", "fd00::1"}}, {"docker0", {"172.17.0.1"}}
  };
  const QByteArray routes = "Iface Destination Gateway Flags RefCnt Use Metric Mask MTU Window IRTT\n"
                            "eno2 000AA8C0 00000000 0001 0 0 100 00FFFFFF 0 0 0\n"
                            "wlp3s0 00000000 0132A8C0 0003 0 0 600 00000000 0 0 0\n";
  QCOMPARE(
      NetworkMonitor::preferDefaultIpv4Route(addresses, routes, interfaces),
      (QStringList{"192.168.50.219", "192.168.10.1", "172.17.0.1", "fd00::1"})
  );
}

void NetworkMonitorTests::defaultRouteUsesLowestMetricAndPreservesTies()
{
  const QStringList addresses{"192.168.10.1", "192.168.50.219"};
  const QHash<QString, QStringList> interfaces{{"eno2", {addresses[0]}}, {"wlp3s0", {addresses[1]}}};
  const QByteArray routes = "eno2 00000000 010AA8C0 0003 0 0 900 00000000 0 0 0\n"
                            "wlp3s0 00000000 0132A8C0 0003 0 0 600 00000000 0 0 0\n";
  QCOMPARE(
      NetworkMonitor::preferDefaultIpv4Route(addresses, routes, interfaces),
      (QStringList{"192.168.50.219", "192.168.10.1"})
  );
  QCOMPARE(
      NetworkMonitor::preferDefaultIpv4Route(addresses, QByteArray(routes).replace("900", "100"), interfaces), addresses
  );
  QCOMPARE(
      NetworkMonitor::preferDefaultIpv4Route(addresses, QByteArray(routes).replace("900", "600"), interfaces), addresses
  );
}

void NetworkMonitorTests::virtualDefaultRouteKeepsPhysicalAddressesFirst()
{
  const QStringList addresses{"192.168.10.1", "192.168.50.219", "10.8.0.2"};
  const QHash<QString, QStringList> physicalInterfaces{{"eno2", {addresses[0]}}, {"wlp3s0", {addresses[1]}}};
  QByteArray routes = "tun0 00000000 00000000 0001 0 0 1 00000000 0 0 0\n";
  QCOMPARE(NetworkMonitor::preferDefaultIpv4Route(addresses, routes, physicalInterfaces), addresses);
  routes.append("wlp3s0 00000000 0132A8C0 0003 0 0 600 00000000 0 0 0\n");
  QCOMPARE(
      NetworkMonitor::preferDefaultIpv4Route(addresses, routes, physicalInterfaces),
      (QStringList{"192.168.50.219", "192.168.10.1", "10.8.0.2"})
  );
}

void NetworkMonitorTests::missingOrMalformedRoutesPreserveOrder_data()
{
  QTest::addColumn<QByteArray>("routes");
  QTest::newRow("empty") << QByteArray();
  QTest::newRow("no-default") << QByteArray("wlp3s0 0032A8C0 00000000 0001 0 0 1 00FFFFFF 0 0 0\n");
  QTest::newRow("inactive-or-unknown-interface")
      << QByteArray("offline0 00000000 0132A8C0 0003 0 0 1 00000000 0 0 0\n");
  QTest::newRow("not-up") << QByteArray("wlp3s0 00000000 0132A8C0 0002 0 0 1 00000000 0 0 0\n");
  QTest::newRow("rejected") << QByteArray("wlp3s0 00000000 0132A8C0 0201 0 0 1 00000000 0 0 0\n");
  QTest::newRow("short-row") << QByteArray("wlp3s0 00000000 0132A8C0 0003 0 0 1 00000000\n");
  QTest::newRow("invalid-gateway") << QByteArray("wlp3s0 00000000 invalid 0003 0 0 1 00000000 0 0 0\n");
  QTest::newRow("invalid-flags") << QByteArray("wlp3s0 00000000 0132A8C0 badflags 0 0 1 00000000 0 0 0\n");
  QTest::newRow("invalid-metric") << QByteArray("wlp3s0 00000000 0132A8C0 0003 0 0 badmetric 00000000 0 0 0\n");
  QTest::newRow("overflow-metric") << QByteArray("wlp3s0 00000000 0132A8C0 0003 0 0 4294967296 00000000 0 0 0\n");
  QTest::newRow("invalid-mask") << QByteArray("wlp3s0 00000000 0132A8C0 0003 0 0 1 invalid 0 0 0\n");
}

void NetworkMonitorTests::missingOrMalformedRoutesPreserveOrder()
{
  QFETCH(QByteArray, routes);
  const QStringList addresses{"192.168.10.1", "192.168.50.219"};
  const QHash<QString, QStringList> interfaces{{"eno2", {addresses[0]}}, {"wlp3s0", {addresses[1]}}};
  QCOMPARE(NetworkMonitor::preferDefaultIpv4Route(addresses, routes, interfaces), addresses);
}

void NetworkMonitorTests::malformedRouteCannotOverrideValidDefault()
{
  const QStringList addresses{"192.168.10.1", "192.168.50.219"};
  const QHash<QString, QStringList> interfaces{{"eno2", {addresses[0]}}, {"wlp3s0", {addresses[1]}}};
  const QByteArray routes = "eno2 00000000 010AA8C0 0003 0 0 invalid 00000000 0 0 0\n"
                            "wlp3s0\t00000000\t0132A8C0\t0003\t0\t0\t600\t00000000\t0\t0\t0\n";
  QCOMPARE(
      NetworkMonitor::preferDefaultIpv4Route(addresses, routes, interfaces),
      (QStringList{"192.168.50.219", "192.168.10.1"})
  );
}

QTEST_MAIN(NetworkMonitorTests)
