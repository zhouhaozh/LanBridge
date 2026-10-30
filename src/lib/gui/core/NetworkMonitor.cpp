/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 - 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "NetworkMonitor.h"

#include <QAbstractSocket>
#include <QFile>
#include <QList>
#include <QNetworkInterface>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>

#include <algorithm>

namespace deskflow::gui {

bool NetworkMonitor::isVirtualInterface(const QString &interfaceName)
{
  // Common virtual network interface patterns
  static const auto virtualRegEx = QRegularExpression(
      QStringLiteral("^vboxnet|vmnet|docker|virbr|veth|br\\-|tun|utun|awdl|p2p|llw|anpi|tap|vEth"),
      QRegularExpression::CaseInsensitiveOption
  );
  return virtualRegEx.match(interfaceName).hasMatch();
}

NetworkMonitor::NetworkMonitor(QObject *parent) : QObject(parent), m_checkTimer(new QTimer(this))
{
  connect(m_checkTimer, &QTimer::timeout, this, &NetworkMonitor::updateNetworkState);
}

void NetworkMonitor::startMonitoring(int intervalMs)
{
  if (m_isMonitoring) {
    return;
  }

  updateNetworkState();

  m_checkTimer->start(intervalMs);
  m_isMonitoring = true;
}

void NetworkMonitor::stopMonitoring()
{
  if (!m_isMonitoring) {
    return;
  }

  m_checkTimer->stop();
  m_isMonitoring = false;
}

QStringList NetworkMonitor::validAddresses()
{
  QList<QHostAddress> physicalIP4;
  QList<QHostAddress> physicalIP6;
  QList<QHostAddress> virtualIP4;
  QList<QHostAddress> virtualIP6;
  QSet<QHostAddress> uniqueAddresses;
  QHash<QString, QStringList> interfaceAddresses;

  const auto allInterfaces = QNetworkInterface::allInterfaces();
  for (const auto &interface : allInterfaces) {
    if (!(interface.flags() & QNetworkInterface::IsUp) || !(interface.flags() & QNetworkInterface::IsRunning) ||
        (interface.flags() & QNetworkInterface::IsLoopBack)) {
      continue;
    }

    const bool isP2P = (interface.flags() & QNetworkInterface::IsPointToPoint);
    const bool isVirtualType = interface.type() == QNetworkInterface::Virtual;
    const bool isVirtual = isVirtualInterface(interface.humanReadableName()) || isP2P || isVirtualType;
    const auto addressEntries = interface.addressEntries();
    // link-local is only worth offering when it's a physical interface's only address, e.g. a direct ethernet cable
    const bool hasNonLinkLocal = std::ranges::any_of(addressEntries, [](const QNetworkAddressEntry &entry) {
      return !entry.ip().isLoopback() && !entry.ip().isLinkLocal();
    });
    const bool skipLinkLocal = isVirtual || hasNonLinkLocal;

    for (const auto &entry : addressEntries) {
      const QHostAddress address = entry.ip();

      if (address.isLoopback() || (skipLinkLocal && address.isLinkLocal())) {
        continue;
      }

      if (!isVirtual && address.protocol() == QHostAddress::IPv4Protocol)
        interfaceAddresses[interface.name()].append(address.toString());
      if (uniqueAddresses.contains(address))
        continue;
      uniqueAddresses.insert(address);

      if (address.protocol() == QHostAddress::IPv6Protocol) {
        if (isVirtual)
          virtualIP6.append(address);
        else
          physicalIP6.append(address);
      } else {
        if (isVirtual)
          virtualIP4.append(address);
        else
          physicalIP4.append(address);
      }
    }
  }

  std::ranges::sort(physicalIP4, [](const QHostAddress &a, const QHostAddress &b) {
    if (a.isPrivateUse() != b.isPrivateUse())
      return a.isPrivateUse();
    if (a.isLinkLocal() != b.isLinkLocal())
      return a.isLinkLocal();
    return a.toIPv4Address() < b.toIPv4Address();
  });

  std::ranges::sort(virtualIP4, [](const QHostAddress &a, const QHostAddress &b) {
    return a.toIPv4Address() < b.toIPv4Address();
  });

  std::ranges::sort(physicalIP6, [](const QHostAddress &a, const QHostAddress &b) {
    if (a.isPrivateUse() != b.isPrivateUse())
      return a.isPrivateUse();
    if (a.isLinkLocal() != b.isLinkLocal())
      return a.isLinkLocal();
    return a.toString() < b.toString();
  });

  std::ranges::sort(virtualIP6, [](const QHostAddress &a, const QHostAddress &b) {
    return a.toString() < b.toString();
  });

  auto result = physicalIP4;
  result.append(virtualIP4);
  result.append(physicalIP6);
  result.append(virtualIP6);

  QStringList ipList;
  for (const auto &host : result) {
    ipList.append(host.toString());
  }
#if defined(Q_OS_LINUX)
  QFile routes(QStringLiteral("/proc/net/route"));
  if (routes.open(QIODevice::ReadOnly))
    return preferDefaultIpv4Route(ipList, routes.readAll(), interfaceAddresses);
#endif
  return ipList;
}

QStringList NetworkMonitor::preferDefaultIpv4Route(
    QStringList addresses, const QByteArray &routeTable, const QHash<QString, QStringList> &interfaceAddresses
)
{
  QSet<QString> preferred;
  quint32 lowestMetric = 0;
  bool foundRoute = false;
  for (const auto &line : routeTable.split('\n')) {
    const auto fields = line.simplified().split(' ');
    if (fields.size() < 11)
      continue;

    bool destinationOk, gatewayOk, flagsOk, metricOk, maskOk;
    const auto destination = fields[1].toUInt(&destinationOk, 16);
    fields[2].toUInt(&gatewayOk, 16);
    const auto flags = fields[3].toUInt(&flagsOk, 16);
    const auto metric = fields[6].toUInt(&metricOk, 10);
    const auto mask = fields[7].toUInt(&maskOk, 16);
    // /proc/net/route uses hexadecimal addresses/flags and decimal metrics.
    // RTF_UP is required; RTF_REJECT must never become a recommendation.
    if (!destinationOk || !gatewayOk || !flagsOk || !metricOk || !maskOk || destination || mask || !(flags & 0x1) ||
        (flags & 0x200))
      continue;

    QStringList candidates;
    for (const auto &address : interfaceAddresses.value(QString::fromUtf8(fields[0]))) {
      if (QHostAddress(address).protocol() == QHostAddress::IPv4Protocol && addresses.contains(address))
        candidates.append(address);
    }
    if (candidates.isEmpty() || (foundRoute && metric > lowestMetric))
      continue;
    if (!foundRoute || metric < lowestMetric)
      preferred.clear();
    foundRoute = true;
    lowestMetric = metric;
    for (const auto &address : candidates)
      preferred.insert(address);
  }

  if (preferred.isEmpty())
    return addresses;
  std::stable_partition(addresses.begin(), addresses.end(), [&preferred](const QString &address) {
    return preferred.contains(address);
  });
  return addresses;
}

void NetworkMonitor::setIpAddresses(const QStringList &newAddresses)
{
  if (newAddresses == m_lastAddresses)
    return;
  m_lastAddresses = newAddresses;
  Q_EMIT ipAddressesChanged(m_lastAddresses);
}

void NetworkMonitor::updateNetworkState()
{
  setIpAddresses(validAddresses());
}

} // namespace deskflow::gui
