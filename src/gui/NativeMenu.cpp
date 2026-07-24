/*
        NativeMenu вЂ” implementation. See NativeMenu.h for the public API.

        Rendering uses only ScriptHookV UI/GRAPHICS natives. Input uses the
        game's "frontend" controls (which map keyboard arrows + Enter/Backspace
        AND the controller D-pad/A/B), read through the *disabled* control
        accessors so the menu keeps working even while it suppresses the rest of
        the game's input.

        Layout is authored on a fixed 1920x1080 design grid and converted to
        screen fractions at draw time, so it is resolution-independent and the
        single `scale_` multiplier resizes everything proportionally.
*/

#include "NativeMenu.h"
#include "MenuDraw.h"

// The ScriptHookV headers pull in <windows.h>, whose min/max macros collide
// with std::min/std::max. Suppress them before anything includes windows.h.
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "main.h"
#include "natives.h"
#include "types.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#pragma warning(disable : 4244 4305)

using gtam::draw::Align;
using gtam::draw::BASE_W;
using gtam::draw::BASE_H;

namespace gtam {

// ============================================================
//  Theme presets
// ============================================================

MenuTheme MenuTheme::GtaOnline() { return MenuTheme(); } // defaults already are this look

MenuTheme MenuTheme::Dark() {
  MenuTheme t;
  t.titleBg = Color(18, 20, 26, 255);
  t.accent = Color(80, 150, 235, 255);
  t.rowSelectedBg = Color(80, 150, 235, 255);
  t.rowSelectedText = Color(255, 255, 255, 255);
  t.checkboxFill = Color(80, 150, 235, 255);
  t.scrollThumb = Color(80, 150, 235, 255);
  t.rowBg = Color(14, 16, 20, 210);
  return t;
}

MenuTheme MenuTheme::Spotlight() {
  // Mirrors the CommandPalette's dark spotlight palette so the panel menu wears
  // the same skin: near-black translucent rows, cyan accent, and the outlined
  // teal selected row.
  MenuTheme t;
  t.titleBg = Color(17, 19, 24, 245);
  t.titleText = Color(238, 242, 247, 255);
  t.accent = Color(60, 200, 220, 255);

  // Subtitle bar: dark (not accent-filled), grey label + counter.
  t.accentSubtitleBar = false;
  t.subtitleBg = Color(20, 23, 28, 245);
  t.subtitleText = Color(150, 200, 210, 255);

  t.rowBg = Color(17, 19, 24, 230);
  t.rowText = Color(205, 210, 218, 255);
  t.rowSelectedBg = Color(30, 80, 95, 235);
  t.rowSelectedText = Color(240, 245, 250, 255);
  t.rowSelectedOutline = Color(60, 200, 220, 255);
  t.rowSelectedOutlinePx = 1.4f; // the signature cyan border
  t.disabledText = Color(110, 116, 124, 255);
  t.separatorText = Color(120, 126, 135, 255);

  t.descBg = Color(13, 15, 19, 240);
  t.descText = Color(205, 210, 218, 255);

  t.scrollTrack = Color(0, 0, 0, 120);
  t.scrollThumb = Color(60, 200, 220, 255);

  t.checkboxBorder = Color(90, 96, 104, 255);
  t.checkboxFill = Color(60, 200, 220, 255);

  t.valuePill = true;
  t.valuePillBgSelected = Color(0, 0, 0, 70);

  t.accentBarWidthPx = 3.0f;
  return t;
}

MenuTheme MenuTheme::Light() {
  MenuTheme t;
  t.titleBg = Color(235, 235, 235, 255);
  t.titleText = Color(20, 20, 20, 255);
  t.accent = Color(40, 120, 215, 255);
  t.subtitleText = Color(255, 255, 255, 255);
  t.rowBg = Color(245, 245, 245, 235);
  t.rowText = Color(25, 25, 25, 255);
  t.rowSelectedBg = Color(40, 120, 215, 255);
  t.rowSelectedText = Color(255, 255, 255, 255);
  t.disabledText = Color(150, 150, 150, 255);
  t.separatorText = Color(90, 90, 90, 255);
  t.descBg = Color(225, 225, 225, 245);
  t.descText = Color(25, 25, 25, 255);
  t.checkboxBorder = Color(90, 90, 90, 255);
  t.checkboxFill = Color(40, 120, 215, 255);
  return t;
}

// ============================================================
//  Menu builders
// ============================================================

MenuItem &Menu::Add(MenuItem item) {
  items.push_back(std::move(item));
  return items.back();
}

MenuItem &Menu::AddButton(const std::string &text,
                          std::function<void()> onClick,
                          const std::string &desc) {
  MenuItem it;
  it.type = ItemType::Button;
  it.text = text;
  it.description = desc;
  if (onClick) it.onActivate = [onClick](MenuItem &) { onClick(); };
  return Add(std::move(it));
}

MenuItem &Menu::AddToggle(const std::string &text, bool *value,
                          std::function<void(bool)> onChange,
                          const std::string &desc) {
  MenuItem it;
  it.type = ItemType::Toggle;
  it.text = text;
  it.description = desc;
  it.boolPtr = value;
  if (onChange)
    it.onChange = [onChange](MenuItem &i) {
      if (i.boolPtr) onChange(*i.boolPtr);
    };
  return Add(std::move(it));
}

MenuItem &Menu::AddFloat(const std::string &text, float *value, float mn,
                         float mx, float step, int decimals,
                         std::function<void(float)> onChange,
                         const std::string &desc) {
  MenuItem it;
  it.type = ItemType::Float;
  it.text = text;
  it.description = desc;
  it.floatPtr = value;
  it.fMin = mn;
  it.fMax = mx;
  it.fStep = step;
  it.fDecimals = decimals;
  if (onChange)
    it.onChange = [onChange](MenuItem &i) {
      if (i.floatPtr) onChange(*i.floatPtr);
    };
  return Add(std::move(it));
}

MenuItem &Menu::AddInt(const std::string &text, int *value, int mn, int mx,
                       int step, std::function<void(int)> onChange,
                       const std::string &desc) {
  MenuItem it;
  it.type = ItemType::Int;
  it.text = text;
  it.description = desc;
  it.intPtr = value;
  it.iMin = mn;
  it.iMax = mx;
  it.iStep = step;
  if (onChange)
    it.onChange = [onChange](MenuItem &i) {
      if (i.intPtr) onChange(*i.intPtr);
    };
  return Add(std::move(it));
}

MenuItem &Menu::AddList(const std::string &text, int *index,
                        std::vector<std::string> options,
                        std::function<void(int)> onChange,
                        const std::string &desc) {
  MenuItem it;
  it.type = ItemType::List;
  it.text = text;
  it.description = desc;
  it.listIndexPtr = index;
  it.listOptions = std::move(options);
  if (onChange)
    it.onChange = [onChange](MenuItem &i) {
      if (i.listIndexPtr) onChange(*i.listIndexPtr);
    };
  return Add(std::move(it));
}

MenuItem &Menu::AddSubmenu(const std::string &text, Menu *sub,
                           const std::string &desc) {
  MenuItem it;
  it.type = ItemType::Submenu;
  it.text = text;
  it.description = desc;
  it.submenu = sub;
  return Add(std::move(it));
}

MenuItem &Menu::AddLabel(const std::string &text) {
  MenuItem it;
  it.type = ItemType::Label;
  it.text = text;
  return Add(std::move(it));
}

MenuItem &Menu::AddSeparator(const std::string &caption) {
  MenuItem it;
  it.type = ItemType::Separator;
  it.text = caption;
  return Add(std::move(it));
}

void Menu::Clear() {
  items.clear();
  selected = 0;
  scrollOffset = 0;
}

// ============================================================
//  Drawing primitives (1080p design grid -> screen fractions)
// ============================================================

namespace {
// Shared primitives live in gtam::draw (MenuDraw.h). Thin local aliases keep
// the draw code below terse.
using gtam::draw::RectTL;
using gtam::draw::Text;

std::string FmtFloat(float v, int dec) {
  char buf[48];
  char fmt[8];
  sprintf_s(fmt, "%%.%df", dec < 0 ? 0 : (dec > 6 ? 6 : dec));
  sprintf_s(buf, fmt, v);
  return buf;
}
std::string FmtInt(int v) {
  char buf[24];
  sprintf_s(buf, "%d", v);
  return buf;
}
} // namespace

// ============================================================
//  Controller
// ============================================================

MenuController::MenuController() { theme_ = MenuTheme::GtaOnline(); }

void MenuController::SetPosition(float xFrac, float yFrac) {
  posX_ = std::max(0.0f, std::min(0.95f, xFrac));
  posY_ = std::max(0.0f, std::min(0.95f, yFrac));
}

void MenuController::SetScale(float s) {
  scale_ = std::max(0.5f, std::min(2.5f, s));
}

void MenuController::SetWidth(float widthPx) {
  widthOverridePx_ = std::max(180.0f, std::min(900.0f, widthPx));
}

float MenuController::Width() const {
  return widthOverridePx_ > 0.0f ? widthOverridePx_ : theme_.widthPx;
}

void MenuController::EnableInteractiveResize(bool on, int modifierVK) {
  resizeEnabled_ = on;
  resizeModifierVK_ = modifierVK;
}

void MenuController::Open(Menu *root) {
  cancelValueEdit();
  // Fire onPop for anything already open (clean teardown), then open fresh.
  while (!stack_.empty()) {
    Menu *m = stack_.back();
    stack_.pop_back();
    if (m->onPop) m->onPop();
  }
  if (root) {
    stack_.push_back(root); // push first so firstSelectable() sees `root`
    // Restore the row we left on (remember mode) when it's still valid;
    // otherwise start fresh at the first selectable row.
    int n = (int)root->items.size();
    bool restore = false;
    if (rememberRootCursor_ && root->selected >= 0 && root->selected < n) {
      const MenuItem &it = root->items[root->selected];
      restore = it.enabled && it.type != ItemType::Label &&
                it.type != ItemType::Separator;
    }
    if (!restore) {
      root->scrollOffset = 0;
      root->selected = root->items.empty() ? 0 : firstSelectable(0, +1);
    }
    if (root->onPush) root->onPush();
  }
}

void MenuController::Close() {
  cancelValueEdit();
  while (!stack_.empty()) {
    Menu *m = stack_.back();
    stack_.pop_back();
    if (m->onPop) m->onPop();
  }
}

void MenuController::Toggle(Menu *root) {
  if (IsOpen())
    Close();
  else
    Open(root);
}

void MenuController::Push(Menu *m) {
  if (!m) return;
  m->scrollOffset = 0;
  stack_.push_back(m); // push first so firstSelectable() / onPush see `m`
  if (m->onPush) m->onPush();
  m->selected = m->items.empty() ? 0 : firstSelectable(0, +1);
  playSound("NAV_UP_DOWN");
}

void MenuController::Back() {
  if (stack_.empty()) return;
  Menu *m = stack_.back();
  stack_.pop_back();
  if (m->onPop) m->onPop();
  playSound("BACK");
}

void MenuController::playSound(const char *name) {
  if (!theme_.playSounds) return;
  AUDIO::PLAY_SOUND_FRONTEND(-1, (char *)name,
                             (char *)"HUD_FRONTEND_DEFAULT_SOUNDSET", 0);
}

// Return the next selectable row index starting at `from`, stepping by `dir`,
// skipping labels/separators/disabled rows. Wraps if theme.wrapNavigation.
// Returns `from` if nothing selectable exists (caller tolerates that).
int MenuController::firstSelectable(int from, int dir) const {
  Menu *m = Current();
  if (!m || m->items.empty()) return 0;
  int n = (int)m->items.size();
  int idx = from;
  for (int guard = 0; guard < n; ++guard) {
    if (idx < 0) idx = theme_.wrapNavigation ? n - 1 : 0;
    if (idx >= n) idx = theme_.wrapNavigation ? 0 : n - 1;
    const MenuItem &it = m->items[idx];
    bool selectable = it.enabled && it.type != ItemType::Label &&
                      it.type != ItemType::Separator;
    if (selectable) return idx;
    idx += dir;
    if (!theme_.wrapNavigation && (idx < 0 || idx >= n)) break;
  }
  return std::max(0, std::min(n - 1, from));
}

// ============================================================
//  Input
// ============================================================

namespace {
// Frontend control ids (work for keyboard + pad through the disabled accessors)
constexpr int CTRL_UP = 188;
constexpr int CTRL_DOWN = 187;
constexpr int CTRL_LEFT = 189;
constexpr int CTRL_RIGHT = 190;
constexpr int CTRL_ACCEPT = 201; // Enter / A
constexpr int CTRL_CANCEL = 202; // Backspace / B

bool CtrlPressed(int c) { return CONTROLS::IS_DISABLED_CONTROL_PRESSED(0, c) != 0; }
} // namespace

// Edge + auto-repeat. Returns true on the initial press and again on each
// repeat tick while held.
bool MenuController::Repeat(Btn &b, bool downNow, unsigned long now,
                            unsigned long initialMs, unsigned long intervalMs) {
  bool fire = false;
  if (downNow) {
    if (!b.down) {
      fire = true;
      b.nextRepeat = now + initialMs;
    } else if (now >= b.nextRepeat) {
      fire = true;
      b.nextRepeat = now + intervalMs;
    }
  }
  b.down = downNow;
  return fire;
}

void MenuController::readInput() {
  DWORD now = GetTickCount();

  // While the resize modifier is held the arrows drive layout, not navigation,
  // so don't generate nav edges this frame.
  bool resizing = resizeEnabled_ && (GetAsyncKeyState(resizeModifierVK_) & 0x8000);

  bool upNow = !resizing && CtrlPressed(CTRL_UP);
  bool downNow = !resizing && CtrlPressed(CTRL_DOWN);
  bool leftNow = !resizing && CtrlPressed(CTRL_LEFT);
  bool rightNow = !resizing && CtrlPressed(CTRL_RIGHT);

  up_.pressed = Repeat(up_, upNow, now, 280, 95);
  down_.pressed = Repeat(down_, downNow, now, 280, 95);

  // Slider adjust uses an accelerating repeat: the longer you hold, the faster
  // and (via the multiplier in editCurrentItem) the bigger the step.
  bool adjusting = leftNow || rightNow;
  if (adjusting && !wasAdjusting_) holdStart_ = now;
  wasAdjusting_ = adjusting;
  DWORD heldMs = adjusting ? (now - holdStart_) : 0;
  DWORD interval = heldMs > 2500 ? 45 : heldMs > 1200 ? 70 : 110;

  left_.pressed = Repeat(left_, leftNow, now, 280, interval);
  right_.pressed = Repeat(right_, rightNow, now, 280, interval);

  // Select / back are single-shot (edge only, no repeat).
  bool selNow = CONTROLS::IS_DISABLED_CONTROL_JUST_PRESSED(0, CTRL_ACCEPT) != 0;
  bool backNow = CONTROLS::IS_DISABLED_CONTROL_JUST_PRESSED(0, CTRL_CANCEL) != 0;
  select_.pressed = selNow;
  back_.pressed = backNow;
}

void MenuController::applyNavigation() {
  Menu *m = Current();
  if (!m || m->items.empty()) return;

  if (up_.pressed) {
    int next = firstSelectable(m->selected - 1, -1);
    if (next != m->selected) {
      m->selected = next;
      playSound("NAV_UP_DOWN");
    }
  }
  if (down_.pressed) {
    int next = firstSelectable(m->selected + 1, +1);
    if (next != m->selected) {
      m->selected = next;
      playSound("NAV_UP_DOWN");
    }
  }
  if (left_.pressed) editCurrentItem(-1);
  if (right_.pressed) editCurrentItem(+1);
  if (select_.pressed) activateCurrentItem();
  if (back_.pressed) {
    if (stack_.size() > 1)
      Back();
    else
      Close();
  }
}

void MenuController::editCurrentItem(int dir) {
  Menu *m = Current();
  if (!m || m->selected < 0 || m->selected >= (int)m->items.size()) return;
  MenuItem &it = m->items[m->selected];
  if (!it.enabled) return;

  // Accelerating step multiplier (mirrors the repeat ramp in readInput).
  DWORD heldMs = wasAdjusting_ ? (GetTickCount() - holdStart_) : 0;
  float mult = heldMs > 2500 ? 10.0f : heldMs > 1200 ? 4.0f : heldMs > 500 ? 2.0f : 1.0f;

  switch (it.type) {
  case ItemType::Float:
    if (it.floatPtr) {
      *it.floatPtr += dir * it.fStep * mult;
      if (*it.floatPtr < it.fMin) *it.floatPtr = it.fMin;
      if (*it.floatPtr > it.fMax) *it.floatPtr = it.fMax;
      if (it.onChange) it.onChange(it);
      playSound("NAV_UP_DOWN");
    }
    break;
  case ItemType::Int:
    if (it.intPtr) {
      *it.intPtr += dir * it.iStep * (int)mult;
      if (*it.intPtr < it.iMin) *it.intPtr = it.iMin;
      if (*it.intPtr > it.iMax) *it.intPtr = it.iMax;
      if (it.onChange) it.onChange(it);
      playSound("NAV_UP_DOWN");
    }
    break;
  case ItemType::List:
    if (it.listIndexPtr && !it.listOptions.empty()) {
      int n = (int)it.listOptions.size();
      // Normalize the current index into [0,n) first: the host var may already
      // hold an out-of-range value (e.g. a hand-edited INI), and C++ % keeps the
      // sign of the dividend, so cycling a negative index could otherwise write
      // back a negative one.
      int cur = *it.listIndexPtr % n;
      if (cur < 0) cur += n;
      int next = (cur + dir) % n;
      if (next < 0) next += n;
      *it.listIndexPtr = next;
      if (it.onChange) it.onChange(it);
      playSound("NAV_LEFT_RIGHT");
    }
    break;
  case ItemType::Toggle:
    // Left/right also flips a toggle, matching GTA Online behaviour.
    if (it.boolPtr) {
      *it.boolPtr = !*it.boolPtr;
      if (it.onChange) it.onChange(it);
      playSound("NAV_LEFT_RIGHT");
    }
    break;
  default:
    break;
  }
}

void MenuController::activateCurrentItem() {
  Menu *m = Current();
  if (!m || m->selected < 0 || m->selected >= (int)m->items.size()) return;
  MenuItem &it = m->items[m->selected];
  if (!it.enabled) return;

  switch (it.type) {
  case ItemType::Submenu:
    if (it.submenu) {
      if (it.onActivate) it.onActivate(it);
      Push(it.submenu);
      return;
    }
    break;
  case ItemType::Float:
  case ItemType::Int:
    // Enter on a numeric row starts inline "type an exact value" editing.
    beginValueEdit(m->selected);
    return;
  case ItemType::Toggle:
    if (it.boolPtr) {
      *it.boolPtr = !*it.boolPtr;
      if (it.onChange) it.onChange(it);
    }
    playSound("SELECT");
    break;
  case ItemType::List:
    // Enter advances the choice (in addition to left/right).
    if (it.listIndexPtr && !it.listOptions.empty()) {
      int n = (int)it.listOptions.size();
      *it.listIndexPtr = (*it.listIndexPtr + 1) % n;
      if (it.onChange) it.onChange(it);
    }
    playSound("SELECT");
    break;
  default:
    playSound("SELECT");
    break;
  }
  if (it.onActivate && it.type != ItemType::Submenu) it.onActivate(it);
}

// ============================================================
//  Inline value typing (Float / Int rows)
// ============================================================

void MenuController::beginValueEdit(int index) {
  Menu *m = Current();
  if (!m || index < 0 || index >= (int)m->items.size()) return;
  const MenuItem &it = m->items[index];
  char buf[48] = {0}; // %.6f of a large-magnitude float can need ~46 chars
  if (it.type == ItemType::Float && it.floatPtr) {
    int dec = it.fDecimals < 0 ? 0 : (it.fDecimals > 6 ? 6 : it.fDecimals);
    char fmt[8];
    sprintf_s(fmt, "%%.%df", dec);
    sprintf_s(buf, fmt, *it.floatPtr);
  } else if (it.type == ItemType::Int && it.intPtr) {
    sprintf_s(buf, "%d", *it.intPtr);
  } else {
    return; // not a typable row
  }
  valueInput_.SetText(buf);
  valueInput_.SetFocused(true);
  editMenu_ = m;
  editIndex_ = index;
  prevEditEsc_ = true; // ignore an Esc still held from before
  playSound("SELECT");
}

void MenuController::commitValueEdit() {
  if (!editMenu_ || editIndex_ < 0 || editIndex_ >= (int)editMenu_->items.size()) {
    cancelValueEdit();
    return;
  }
  MenuItem &it = editMenu_->items[editIndex_];
  const std::string &s = valueInput_.Text();
  if (!s.empty()) {
    if (it.type == ItemType::Float && it.floatPtr) {
      float v = (float)atof(s.c_str());
      if (v < it.fMin) v = it.fMin;
      if (v > it.fMax) v = it.fMax;
      *it.floatPtr = v;
      if (it.onChange) it.onChange(it);
    } else if (it.type == ItemType::Int && it.intPtr) {
      int v = atoi(s.c_str());
      if (v < it.iMin) v = it.iMin;
      if (v > it.iMax) v = it.iMax;
      *it.intPtr = v;
      if (it.onChange) it.onChange(it);
    }
  }
  playSound("SELECT");
  cancelValueEdit();
}

void MenuController::cancelValueEdit() {
  valueInput_.SetFocused(false);
  valueInput_.Clear();
  editMenu_ = nullptr;
  editIndex_ = -1;
}

void MenuController::updateValueEdit() {
  // Bail if the edited row went away (menu changed / rebuilt to a different type).
  if (editMenu_ != Current() || editIndex_ < 0 ||
      editIndex_ >= (int)editMenu_->items.size()) {
    cancelValueEdit();
    return;
  }
  const MenuItem &it = editMenu_->items[editIndex_];
  if (it.type != ItemType::Float && it.type != ItemType::Int) {
    cancelValueEdit();
    return;
  }
  // Enter (frontend accept, also the Enter key) commits.
  if (CONTROLS::IS_DISABLED_CONTROL_JUST_PRESSED(0, CTRL_ACCEPT)) {
    commitValueEdit();
    return;
  }
  // Esc cancels (edge-detected). Typed digits/backspace flow through the
  // keyboard handler into valueInput_.
  bool esc = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
  if (esc && !prevEditEsc_) {
    playSound("BACK");
    cancelValueEdit();
    return;
  }
  prevEditEsc_ = esc;
}

void MenuController::ensureVisible() {
  Menu *m = Current();
  if (!m) return;
  int maxRows = std::max(1, theme_.maxVisibleRows);
  int n = (int)m->items.size();
  int maxOffset = std::max(0, n - maxRows);
  if (m->selected < m->scrollOffset) m->scrollOffset = m->selected;
  if (m->selected >= m->scrollOffset + maxRows)
    m->scrollOffset = m->selected - maxRows + 1;
  m->scrollOffset = std::max(0, std::min(maxOffset, m->scrollOffset));
}

// ============================================================
//  Interactive resize / move
// ============================================================

void MenuController::handleInteractiveResize() {
  if (!resizeEnabled_) return;
  if (!(GetAsyncKeyState(resizeModifierVK_) & 0x8000)) return;

  auto held = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };

  // Move (arrow keys) вЂ” small steps for fine placement.
  const float moveStep = 0.0035f;
  if (held(VK_LEFT)) SetPosition(posX_ - moveStep, posY_);
  if (held(VK_RIGHT)) SetPosition(posX_ + moveStep, posY_);
  if (held(VK_UP)) SetPosition(posX_, posY_ - moveStep);
  if (held(VK_DOWN)) SetPosition(posX_, posY_ + moveStep);

  // Scale ( - / = like browser zoom; also numpad +/- ).
  if (held(VK_OEM_MINUS) || held(VK_SUBTRACT)) SetScale(scale_ - 0.01f);
  if (held(VK_OEM_PLUS) || held(VK_ADD)) SetScale(scale_ + 0.01f);

  // Width ( [ / ] ).
  if (held(VK_OEM_4)) SetWidth(Width() - 3.0f); // [
  if (held(VK_OEM_6)) SetWidth(Width() + 3.0f); // ]

  // Reset ( 0 ).
  if (held('0')) {
    scale_ = 1.0f;
    widthOverridePx_ = -1.0f;
    SetPosition(0.02f, 0.07f);
  }
}

// ============================================================
//  Draw
// ============================================================

void MenuController::draw() {
  Menu *m = Current();
  if (!m) return;
  const MenuTheme &th = m->themeOverride ? *m->themeOverride : theme_;

  const float s = scale_;
  const float width = (widthOverridePx_ > 0.0f ? widthOverridePx_ : th.widthPx) * s;
  const float wFrac = width / BASE_W;
  const float xFrac = posX_;

  const float padX = gtam::draw::PxX(12.0f * s); // inner horizontal padding
  const float leftEdge = xFrac + padX;
  const float rightEdge = xFrac + wFrac - padX;

  // Reserve a left icon column only if at least one item actually has an icon,
  // so icon-less menus don't waste the space.
  bool menuHasIcons = th.iconColumnPx > 0.0f;
  if (menuHasIcons) {
    menuHasIcons = false;
    for (const auto &it : m->items)
      if (!it.iconName.empty()) { menuHasIcons = true; break; }
  }
  const float iconCol = menuHasIcons ? gtam::draw::PxX(th.iconColumnPx * s) : 0.0f;
  const float leftText = leftEdge + iconCol;

  float y = posY_;

  // --- Title banner ---
  const float titleH = gtam::draw::PxY(th.titleHeightPx * s);
  RectTL(xFrac, y, wFrac, titleH, th.titleBg);
  {
    float titleTH = gtam::draw::TextLineHeight(th.titleScale * s, th.titleFont);
    float ty = y + (titleH - titleTH) * 0.5f - titleTH * 0.14f;
    if (th.titleCentered)
      Text(m->title, xFrac + wFrac * 0.5f, ty, th.titleScale * s, th.titleFont,
           th.titleText, Align::Center);
    else
      Text(m->title, leftEdge, ty, th.titleScale * s, th.titleFont,
           th.titleText, Align::Left);
  }
  y += titleH;

  // --- Subtitle / counter bar ---
  const float subH = gtam::draw::PxY(th.subtitleHeightPx * s);
  RectTL(xFrac, y, wFrac, subH, th.accentSubtitleBar ? th.accent : th.subtitleBg);
  {
    float subTH = gtam::draw::TextLineHeight(th.subtitleScale * s, th.subtitleFont);
    float ty = y + (subH - subTH) * 0.5f - subTH * 0.14f;
    Color stc = th.subtitleText;
    if (!m->subtitle.empty())
      Text(m->subtitle, leftEdge, ty, th.subtitleScale * s, th.subtitleFont, stc,
           Align::Left);
    // Count only real, selectable options (skip separators / labels / disabled
    // rows) so the "x / y" reads like a normal menu.
    int selCount = 0, selPos = 0;
    for (int i = 0; i < (int)m->items.size(); ++i) {
      const MenuItem &it = m->items[i];
      if (!it.enabled || it.type == ItemType::Label ||
          it.type == ItemType::Separator)
        continue;
      ++selCount;
      if (i == m->selected) selPos = selCount;
    }
    char counter[32];
    sprintf_s(counter, "%d / %d", selPos, selCount);
    Text(counter, rightEdge, ty, th.subtitleScale * s, th.subtitleFont, stc,
         Align::Right, rightEdge);
  }
  y += subH;

  // --- Rows (windowed) ---
  const float rowH = gtam::draw::PxY(th.rowHeightPx * s);
  // Exact single-line text height for this font/scale, so every row's caption +
  // value is vertically centred in its block (no magic offsets).
  const float rowTextH = gtam::draw::TextLineHeight(th.rowTextScale * s, th.rowFont);
  // GTA's _DRAW_TEXT seats the glyph a little below the box top and the scale
  // height under-reports the real glyph extent, so pure box-centering reads a
  // touch low. Nudge up ~14% of the text height to visually centre it.
  const float rowTextY = (rowH - rowTextH) * 0.5f - rowTextH * 0.14f;
  int maxRows = std::max(1, th.maxVisibleRows);
  int n = (int)m->items.size();
  int start = m->scrollOffset;
  int end = std::min(n, start + maxRows);

  const bool hasScrollbar = th.showScrollbar && n > maxRows;
  const float sbW = hasScrollbar ? gtam::draw::PxX(th.scrollbarWidthPx * s) : 0.0f;
  const float valueRight = rightEdge - sbW - gtam::draw::PxX(2.0f);

  float listTop = y;
  for (int i = start; i < end; ++i) {
    const MenuItem &it = m->items[i];
    bool sel = (i == m->selected);

    Color bg = sel ? th.rowSelectedBg : th.rowBg;
    Color fg = sel ? th.rowSelectedText : th.rowText;
    if (!it.enabled) fg = th.disabledText;

    if (it.type == ItemType::Separator) {
      RectTL(xFrac, y, wFrac, rowH, th.rowBg);
      if (!it.text.empty())
        Text(it.text, xFrac + wFrac * 0.5f,
             y + (rowH - gtam::draw::TextLineHeight(th.rowTextScale * 0.9f * s,
                                                    th.rowFont)) * 0.5f -
                 gtam::draw::TextLineHeight(th.rowTextScale * 0.9f * s,
                                            th.rowFont) * 0.14f,
             th.rowTextScale * 0.9f * s, th.rowFont, th.separatorText,
             Align::Center);
      else
        RectTL(leftEdge, y + rowH * 0.5f, wFrac - 2 * padX, gtam::draw::PxY(1.0f),
               th.separatorText);
      y += rowH;
      continue;
    }

    // Row background + selected accent bar on the left edge.
    RectTL(xFrac, y, wFrac, rowH, bg);
    if (sel && th.rowSelectedOutlinePx > 0.0f)
      gtam::draw::RectOutline(xFrac, y, wFrac, rowH, th.rowSelectedOutline,
                              th.rowSelectedOutlinePx * s);
    if (sel && th.accentBarWidthPx > 0.0f)
      RectTL(xFrac, y, gtam::draw::PxX(th.accentBarWidthPx * s), rowH, th.accent);

    float ty = y + rowTextY;

    if (it.type == ItemType::Label) {
      Text(it.text, leftText, ty, th.rowTextScale * s, th.rowFont,
           th.separatorText, Align::Left);
      y += rowH;
      continue;
    }

    // Icon (game sprite) in the reserved column.
    if (menuHasIcons && !it.iconName.empty() && !it.iconDict.empty()) {
      gtam::draw::EnsureTxd(it.iconDict.c_str());
      float isz = th.iconSizePx * s;
      float iconCx = leftEdge + iconCol * 0.5f - padX * 0.25f;
      float iconCy = y + rowH * 0.5f;
      Color itint = it.iconTint;
      if (sel) { itint = th.rowSelectedText; } // tint to match selected text
      gtam::draw::Sprite(it.iconDict.c_str(), it.iconName.c_str(), iconCx, iconCy,
                         gtam::draw::PxX(isz), gtam::draw::PxY(isz), itint);
    }

    // Caption
    Text(it.text, leftText, ty, th.rowTextScale * s, th.rowFont, fg, Align::Left);

    // Resolve the right-hand value / control type.
    std::string rv;
    bool drawChevrons = sel;
    bool isCheckbox = false;
    bool isAdjustable = false; // float/int/list -> eligible for value pill
    switch (it.type) {
    case ItemType::Toggle:
      isCheckbox = true;
      break;
    case ItemType::Float:
      if (it.floatPtr) rv = FmtFloat(*it.floatPtr, it.fDecimals);
      isAdjustable = true;
      break;
    case ItemType::Int:
      if (it.intPtr) rv = FmtInt(*it.intPtr);
      isAdjustable = true;
      break;
    case ItemType::List:
      if (it.listIndexPtr && !it.listOptions.empty()) {
        int idx = *it.listIndexPtr;
        if (idx >= 0 && idx < (int)it.listOptions.size()) rv = it.listOptions[idx];
      }
      isAdjustable = true;
      break;
    case ItemType::Submenu:
      rv = ">";
      drawChevrons = false;
      break;
    case ItemType::Button:
      drawChevrons = false;
      rv = it.valueGetter ? it.valueGetter() : it.rightLabel;
      break;
    default:
      break;
    }
    if (it.valueGetter && it.type != ItemType::Button) rv = it.valueGetter();

    // If this row is being typed into, show the live input buffer + a blinking
    // caret instead of the bound value, as plain right-aligned text.
    bool typingThisRow = (editMenu_ == m && editIndex_ == i);
    if (typingThisRow) {
      rv = valueInput_.Text();
      if (valueInput_.CaretVisible()) rv += "|";
      drawChevrons = false;
      isCheckbox = false;
      isAdjustable = false;
    }

    if (isCheckbox) {
      float boxPx = th.rowHeightPx * 0.46f * s;
      float boxW = gtam::draw::PxX(boxPx), boxH = gtam::draw::PxY(boxPx);
      float bx = valueRight - boxW;
      float by = y + (rowH - boxH) * 0.5f;
      RectTL(bx, by, boxW, boxH, th.checkboxBorder);
      float ix = gtam::draw::PxX(2.0f), iy = gtam::draw::PxY(2.0f);
      bool on = it.boolPtr && *it.boolPtr;
      Color inner = on ? th.checkboxFill : bg;
      RectTL(bx + ix, by + iy, boxW - 2 * ix, boxH - 2 * iy, inner);
    } else if (!rv.empty()) {
      std::string shown = drawChevrons ? (std::string("< ") + rv + " >") : rv;
      // Value pill behind the text on selected adjustable rows.
      if (th.valuePill && sel && isAdjustable) {
        float tw = gtam::draw::TextWidth(shown, th.rowTextScale * s, th.rowFont);
        float pillPad = gtam::draw::PxX(6.0f);
        float pillX = valueRight - tw - pillPad;
        float pillY = y + rowH * 0.16f;
        float pillH = rowH * 0.68f;
        RectTL(pillX, pillY, tw + 2 * pillPad, pillH, th.valuePillBgSelected);
      }
      Text(shown, valueRight, ty, th.rowTextScale * s, th.rowFont, fg,
           Align::Right, valueRight);
    }

    y += rowH;
  }

  // --- Scrollbar ---
  if (hasScrollbar) {
    float listH = rowH * maxRows;
    float sbX = xFrac + wFrac - sbW;
    RectTL(sbX, listTop, sbW, listH, th.scrollTrack);
    float frac = (float)maxRows / (float)n;
    float thumbH = listH * frac;
    float maxOffset = (float)(n - maxRows);
    float t = maxOffset > 0 ? (float)m->scrollOffset / maxOffset : 0.0f;
    float thumbY = listTop + t * (listH - thumbH);
    RectTL(sbX, thumbY, sbW, thumbH, th.scrollThumb);
  }

  // --- Description panel ---
  float contentBottom = y; // tracks the lowest drawn element for the footer
  if (th.showDescription && m->selected >= 0 && m->selected < n) {
    const std::string &d = m->items[m->selected].description;
    if (!d.empty()) {
      // Wrap the text OURSELVES rather than via SET_TEXT_WRAP. GTA's substring
      // text component (_ADD_TEXT_COMPONENT_STRING) hard-truncates each DRAW at
      // ~99 chars and does NOT reliably concatenate multiple components in this
      // build, so a long description drawn in one call loses its tail (e.g.
      // "...not used in Synce"). TextWrap measures with our own TextWidth and
      // returns short per-line strings; we draw each on its own row — no line is
      // anywhere near the 99-char limit, and the line count is exact, so the box
      // hugs the text with no clipping and no empty slab.
      float descScale = th.descScale * s;
      int descFont = th.subtitleFont;
      float availW = rightEdge - leftEdge;
      std::vector<std::string> wrapped =
          gtam::draw::TextWrap(d, descScale, descFont, availW);
      int lines = (int)wrapped.size();
      if (lines < 1) lines = 1;

      float lineH = gtam::draw::TextLineHeight(descScale, descFont);
      float padY = gtam::draw::PxY(9.0f * s);
      // GTA's real per-line advance runs a touch taller than GET_TEXT_SCALE_HEIGHT.
      const float kLineAdvance = 1.4f;
      float dH = 2.0f * padY + lines * lineH * kLineAdvance;
      float minH = gtam::draw::PxY(th.descHeightPx * s);
      if (dH < minH) dH = minH;
      float dy = y + gtam::draw::PxY(4.0f);
      RectTL(xFrac, dy, wFrac, dH, th.descBg);
      float ly = dy + padY;
      for (const std::string &lineStr : wrapped) {
        Text(lineStr, leftEdge, ly, descScale, descFont, th.descText, Align::Left);
        ly += lineH * kLineAdvance;
      }
      contentBottom = dy + dH;
    }
  }

  // --- Footer: a single app status note (e.g. the active mode). ---
  if (th.showFooter && !footerNote_.empty()) {
    float fy = contentBottom + gtam::draw::PxY(4.0f);
    float fH = gtam::draw::PxY(th.footerHeightPx * s);
    RectTL(xFrac, fy, wFrac, fH, th.footerBg);
    float fScale = th.subtitleScale * 0.92f * s;
    float fH2 = gtam::draw::TextLineHeight(fScale, th.subtitleFont);
    float fty = fy + (fH - fH2) * 0.5f - fH2 * 0.14f;
    Text(footerNote_, leftEdge, fty, fScale, th.subtitleFont, th.footerText,
         Align::Left);
  }
}

// ============================================================
//  Per-frame entry point
// ============================================================

bool MenuController::Update() {
  if (!IsOpen()) return false;

  if (disableControls_) {
    if (isEditingValue()) {
      // While typing a value, disable EVERYTHING so number-row keys can't also
      // trigger weapon-select etc. Typing flows through the WndProc keyboard
      // handler, which is unaffected by control disabling.
      CONTROLS::DISABLE_ALL_CONTROL_ACTIONS(0);
    } else {
      // Otherwise suppress ONLY the controls the menu consumes plus the "escape
      // sideways" actions (phone, weapon/character wheels). Movement/look stay
      // enabled, so opening the menu on foot doesn't freeze the player.
      static const int kDisable[] = {
          27,                 // INPUT_PHONE
          19,                 // INPUT_CHARACTER_WHEEL
          37,                 // INPUT_SELECT_WEAPON (weapon wheel)
          166, 167, 168, 169, // INPUT_SELECT_CHARACTER_*
          172, 173, 174, 175, // INPUT_PHONE_UP/DOWN/LEFT/RIGHT
          188, 187, 189, 190, // INPUT_FRONTEND_UP/DOWN/LEFT/RIGHT
          201, 202,           // INPUT_FRONTEND_ACCEPT/CANCEL
          241, 242,           // INPUT_FRONTEND_(scroll) — cursor wheel
      };
      for (int c : kDisable) CONTROLS::DISABLE_CONTROL_ACTION(0, c, TRUE);
    }
  }

  if (isEditingValue()) {
    updateValueEdit(); // commit/cancel; typed chars arrive via TextInput
  } else {
    handleInteractiveResize();
    readInput();
    applyNavigation();
  }

  // Current() may have changed (push/pop/close) — re-check before drawing.
  if (!IsOpen()) return false;
  ensureVisible();
  draw();
  return true;
}

} // namespace gtam
