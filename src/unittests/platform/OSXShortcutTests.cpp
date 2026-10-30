// SPDX-FileCopyrightText: (C) 2026 LanBridge contributors
// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "arch/Arch.h"
#include "base/EventQueue.h"
#include "base/Log.h"
#include "platform/OSXKeyState.h"

#include <QTest>
#include <algorithm>
#include <iterator>
#include <set>

namespace {

constexpr KeyButton kLeftControl = 10;
constexpr KeyButton kRightControl = 11;
constexpr KeyButton kCommand = 12;
constexpr KeyButton kLetter = 20;
constexpr KeyButton kServerControl = 101;
constexpr KeyButton kServerLetter = 102;

struct RecordedKey
{
  KeyButton button;
  bool down;
  bool repeat;
  KeyModifierMask modifiers;
};

// Overrides the final injection hook: these tests never post native key events.
class RecordingKeyboard : public OSXKeyState
{
public:
  RecordingKeyboard(IEventQueue *events, deskflow::KeyMap &map) : OSXKeyState(events, map, {"en"}, false)
  {
  }

  int32_t pollActiveGroup() const override
  {
    return 0;
  }
  KeyModifierMask pollActiveModifiers() const override
  {
    return 0;
  }

  std::vector<RecordedKey> keys;
  std::set<KeyButton> pressed;

  KeyModifierMask modifiers() const
  {
    KeyModifierMask mask = 0;
    for (const auto &[button, modifier] : modifierButtons) {
      if (pressed.contains(button))
        mask |= modifier;
    }
    return mask;
  }

  static constexpr std::pair<KeyButton, KeyModifierMask> modifierButtons[] = {
      {kLeftControl, KeyModifierControl},
      {kRightControl, KeyModifierControl},
      {kCommand, KeyModifierSuper},
      {13, KeyModifierShift},
      {14, KeyModifierAlt},
      {15, KeyModifierMeta},
      {16, KeyModifierAltGr}
  };

protected:
  void fakeKey(const Keystroke &stroke) override
  {
    if (stroke.m_type != Keystroke::KeyType::Button)
      return;
    const auto &key = stroke.m_data.m_button;
    if (key.m_press)
      pressed.insert(key.m_button);
    else
      pressed.erase(key.m_button);
    keys.push_back({key.m_button, key.m_press, key.m_repeat, modifiers()});
  }
};

void buildKeyMap(deskflow::KeyMap &map)
{
  const KeyID modifiers[] = {kKeyControl_L, kKeyControl_R, kKeySuper_L, kKeyShift_L, kKeyAlt_L, kKeyMeta_L, kKeyAltGr};
  for (size_t index = 0; index < std::size(modifiers); ++index) {
    deskflow::KeyMap::KeyItem item;
    item.m_id = modifiers[index];
    item.m_button = RecordingKeyboard::modifierButtons[index].first;
    item.m_generates = RecordingKeyboard::modifierButtons[index].second;
    map.addKeyEntry(item);
  }
  const KeyID letters[] = {'c', 'C', 'v', 'V', 'd', 'z', 'x', 'a', kKeyEscape};
  for (const KeyID id : letters) {
    deskflow::KeyMap::KeyItem item;
    item.m_id = id;
    item.m_button = kLetter;
    map.addKeyEntry(item);
  }
  map.finish();
}

} // namespace

class OSXShortcutTests : public QObject
{
  Q_OBJECT

private Q_SLOTS:
  void initTestCase()
  {
    m_arch.init();
  }

  void copyAndPaste_data()
  {
    QTest::addColumn<uint>("id");
    QTest::addColumn<uint>("mask");
    QTest::addColumn<bool>("rightControl");
    QTest::newRow("copy") << uint('c') << uint(KeyModifierControl) << false;
    QTest::newRow("paste") << uint('v') << uint(KeyModifierControl) << false;
    QTest::newRow("uppercase-copy") << uint('C') << uint(KeyModifierControl | KeyModifierCapsLock) << false;
    QTest::newRow("uppercase-paste") << uint('V') << uint(KeyModifierControl | KeyModifierCapsLock) << false;
    QTest::newRow("right-control") << uint('c') << uint(KeyModifierControl) << true;
    QTest::newRow("lock-bits") << uint('v') << uint(KeyModifierControl | KeyModifierNumLock | KeyModifierScrollLock)
                               << false;
  }

  void copyAndPaste()
  {
    QFETCH(uint, id);
    QFETCH(uint, mask);
    QFETCH(bool, rightControl);
    deskflow::KeyMap map;
    buildKeyMap(map);
    EventQueue events;
    RecordingKeyboard keyboard(&events, map);
    keyboard.fakeKeyDown(rightControl ? kKeyControl_R : kKeyControl_L, KeyModifierControl, kServerControl, "en");
    keyboard.keys.clear();

    keyboard.fakeKeyDown(id, mask, kServerLetter, "en");
    QCOMPARE(keyboard.keys.size(), size_t(5));
    const auto control = rightControl ? kRightControl : kLeftControl;
    const std::pair<KeyButton, bool> expected[] = {
        {kCommand, true}, {control, false}, {kLetter, true}, {kCommand, false}, {control, true}
    };
    for (size_t index = 0; index < std::size(expected); ++index) {
      QCOMPARE(keyboard.keys[index].button, expected[index].first);
      QCOMPARE(keyboard.keys[index].down, expected[index].second);
    }
    QCOMPARE(keyboard.keys[2].modifiers, KeyModifierSuper);
    QCOMPARE(keyboard.modifiers(), KeyModifierControl);
    QCOMPARE(keyboard.getActiveModifiers(), KeyModifierControl);

    QVERIFY(keyboard.fakeKeyUp(kServerLetter));
    QVERIFY(keyboard.fakeKeyUp(kServerControl));
    QVERIFY(keyboard.pressed.empty());
    QCOMPARE(keyboard.getActiveModifiers(), KeyModifierMask(0));
  }

  void otherCombinations_data()
  {
    QTest::addColumn<uint>("id");
    QTest::addColumn<uint>("mask");
    QTest::newRow("plain-copy-key") << uint('c') << uint(0);
    QTest::newRow("plain-paste-key") << uint('v') << uint(0);
    QTest::newRow("command-copy") << uint('c') << uint(KeyModifierSuper);
    QTest::newRow("command-paste") << uint('v') << uint(KeyModifierSuper);
    QTest::newRow("control-d") << uint('d') << uint(KeyModifierControl);
    QTest::newRow("control-z") << uint('z') << uint(KeyModifierControl);
    QTest::newRow("control-x") << uint('x') << uint(KeyModifierControl);
    QTest::newRow("control-a") << uint('a') << uint(KeyModifierControl);
    QTest::newRow("shift-copy") << uint('c') << uint(KeyModifierControl | KeyModifierShift);
    QTest::newRow("alt-paste") << uint('v') << uint(KeyModifierControl | KeyModifierAlt);
    QTest::newRow("altgr-copy") << uint('c') << uint(KeyModifierControl | KeyModifierAltGr);
    QTest::newRow("meta-copy") << uint('c') << uint(KeyModifierControl | KeyModifierMeta);
    QTest::newRow("command-control-copy") << uint('c') << uint(KeyModifierControl | KeyModifierSuper);
    QTest::newRow("emergency-return") << uint(kKeyEscape) << uint(KeyModifierControl | KeyModifierAlt);
  }

  void otherCombinations()
  {
    QFETCH(uint, id);
    QFETCH(uint, mask);
    deskflow::KeyMap map;
    buildKeyMap(map);
    EventQueue events;
    RecordingKeyboard keyboard(&events, map);
    keyboard.fakeKeyDown(id, mask, kServerLetter, "en");
    const auto key = std::find_if(keyboard.keys.begin(), keyboard.keys.end(), [](const auto &key) {
      return key.button == kLetter && key.down;
    });
    QVERIFY(key != keyboard.keys.end());
    // KeyMap already treats AltGr as optional for command keys; retain that behavior.
    QCOMPARE(key->modifiers, mask & ~KeyModifierAltGr);
    QVERIFY(keyboard.fakeKeyUp(kServerLetter));
    QVERIFY(keyboard.pressed.empty());
  }

  void repeatAndReleaseControlBeforeLetter()
  {
    deskflow::KeyMap map;
    buildKeyMap(map);
    EventQueue events;
    RecordingKeyboard keyboard(&events, map);
    keyboard.fakeKeyDown(kKeyControl_L, KeyModifierControl, kServerControl, "en");
    keyboard.fakeKeyDown('v', KeyModifierControl, kServerLetter, "en");
    keyboard.keys.clear();
    QVERIFY(keyboard.fakeKeyRepeat('v', KeyModifierControl, 2, kServerLetter, "en"));
    int repeats = 0;
    for (const auto &key : keyboard.keys) {
      if (key.button == kLetter && key.down && key.repeat) {
        QCOMPARE(key.modifiers, KeyModifierSuper);
        ++repeats;
      }
    }
    QCOMPARE(repeats, 2);
    QCOMPARE(keyboard.modifiers(), KeyModifierControl);
    QVERIFY(keyboard.fakeKeyUp(kServerControl));
    QVERIFY(keyboard.fakeKeyUp(kServerLetter));
    QVERIFY(keyboard.pressed.empty());
    QCOMPARE(keyboard.modifiers(), KeyModifierMask(0));
  }

  void forcedReleaseClearsAllKeys()
  {
    deskflow::KeyMap map;
    buildKeyMap(map);
    EventQueue events;
    RecordingKeyboard keyboard(&events, map);
    keyboard.fakeKeyDown(kKeyControl_R, KeyModifierControl, kServerControl, "en");
    keyboard.fakeKeyDown('c', KeyModifierControl, kServerLetter, "en");
    keyboard.fakeAllKeysUp();
    QVERIFY(keyboard.pressed.empty());
    QCOMPARE(keyboard.getActiveModifiers(), KeyModifierMask(0));
    QVERIFY(!keyboard.fakeKeyUp(kServerLetter));
    QVERIFY(!keyboard.fakeKeyUp(kServerControl));
  }

private:
  Arch m_arch;
  Log m_log;
};

QTEST_GUILESS_MAIN(OSXShortcutTests)
#include "OSXShortcutTests.moc"
