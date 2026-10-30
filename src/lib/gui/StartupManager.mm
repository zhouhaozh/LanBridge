/*
 * SPDX-FileCopyrightText: (C) 2026 LanBridge contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "StartupManager.h"

#include <QPointer>

#import <Foundation/Foundation.h>
#import <ServiceManagement/ServiceManagement.h>

namespace deskflow::gui {
namespace {
QString systemErrorDescription(NSError *error)
{
  if (!error)
    return QObject::tr("系统未提供具体错误信息");
  return QStringLiteral("%1（%2，错误码 %3）")
      .arg(QString::fromNSString(error.localizedDescription), QString::fromNSString(error.domain))
      .arg(error.code);
}
} // namespace

void StartupManager::refreshMac()
{
  if (@available(macOS 13.0, *)) {
    updateMacStatus(static_cast<int>(SMAppService.mainAppService.status));
    return;
  }
  update(State::Unavailable, tr("登录自启需要 macOS 13 或更新版本。"));
}

void StartupManager::updateMacStatus(int status)
{
  switch (status) {
  case SMAppServiceStatusEnabled:
    update(State::Enabled, tr("系统已开启登录自启。"));
    return;
  case SMAppServiceStatusRequiresApproval:
    update(State::NeedsApproval, tr("请在系统设置 → 通用 → 登录项中允许 LanBridge。"));
    return;
  case SMAppServiceStatusNotRegistered:
    update(State::Disabled, tr("登录自启未开启。"));
    return;
  case SMAppServiceStatusNotFound:
    // NotFound describes a service lookup failure, not a missing .app or invalid signature.
    // Keep the user-initiated registration entry available; registerAndReturnError performs
    // the actual signature and consent validation and returns a useful NSError if rejected.
    update(State::Error, tr("系统暂未找到登录项服务。可勾选“登录后启动”尝试系统注册；若失败，会显示具体原因。"));
    return;
  default:
    update(State::Error, tr("系统返回未知登录项状态（%1）；可重新查询或尝试系统注册。").arg(status));
    return;
  }
}

void StartupManager::setMacEnabled(bool enabled)
{
  if (@available(macOS 13.0, *)) {
    // Register is synchronous in the system API. Queue it so the GUI can show the pending state first.
    QTimer::singleShot(0, this, [this, enabled] {
      if (@available(macOS 13.0, *)) {
        const auto service = SMAppService.mainAppService;
        if ((enabled && service.status == SMAppServiceStatusEnabled) ||
            (!enabled && service.status == SMAppServiceStatusNotRegistered)) {
          refreshMac();
          return;
        }
        if (enabled) {
          NSError *error = nil;
          if (![service registerAndReturnError:&error]) {
            if (service.status == SMAppServiceStatusRequiresApproval)
              refreshMac();
            else
              update(State::Error, tr("系统未能开启登录自启：%1").arg(systemErrorDescription(error)));
            return;
          }
          refreshMac();
          return;
        }
        const QPointer<StartupManager> guard(this);
        [service unregisterWithCompletionHandler:^(NSError *error) {
          const QString description = error ? systemErrorDescription(error) : QString();
          dispatch_async(dispatch_get_main_queue(), ^{
            if (!guard)
              return;
            if (description.isEmpty())
              guard->refreshMac();
            else
              guard->update(State::Error, tr("系统未能关闭登录自启：%1").arg(description));
          });
        }];
      }
    });
    return;
  }
  update(State::Unavailable, tr("登录自启需要 macOS 13 或更新版本。"));
}

} // namespace deskflow::gui
