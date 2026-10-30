/*
 * SPDX-FileCopyrightText: (C) 2026 LanBridge contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once

#include <QTest>

class CoreProcessTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void staleReceiverBlocksStart();
  void uncleanExitBlocksRetry();
};
