#!/usr/bin/env python3
"""Exercise the production banner selection and reset bodies with a host display double."""

import os
import re
import shlex
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def function_body(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start : index + 1]
    raise AssertionError(signature)


PREFIX = r"""
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <vector>

constexpr int MAX_LINES = 5, FONT_SMALL = 0, FONT_HEIGHT_SMALL = 10;
constexpr int TEXT_ALIGN_LEFT = 0, BLACK = 0, WHITE = 1;
const uint8_t bell_alert[] = {0};
enum Input { INPUT_BROKER_NONE, INPUT_BROKER_UP, INPUT_BROKER_LEFT,
 INPUT_BROKER_ALT_PRESS, INPUT_BROKER_UP_LONG, INPUT_BROKER_DOWN,
 INPUT_BROKER_RIGHT, INPUT_BROKER_USER_PRESS, INPUT_BROKER_DOWN_LONG,
 INPUT_BROKER_SELECT, INPUT_BROKER_CANCEL, INPUT_BROKER_ALT_LONG };
struct InputEvent { Input inputEvent = INPUT_BROKER_NONE; char kbchar = 0; };
enum class ScreenResolution { UltraLow, Low, High };
ScreenResolution currentResolution = ScreenResolution::Low;
struct OLEDDisplay {
 uint16_t screenHeight = 64;
 std::vector<std::string> drawn;
 unsigned getStringWidth(const char *, unsigned length, bool) { return length; }
 void setFont(int) {} uint16_t height() { return screenHeight; }
 unsigned width() { return 128; }
 int getWidth() { return width(); }
 void setTextAlignment(int) {} void setColor(int) {}
 void fillRect(int, int, int, int) {} void drawRect(int, int, int, int) {}
 void drawXbm(int, int, int, int, const uint8_t *) {}
 void drawString(int, int, const char *text) { drawn.emplace_back(text); }
};
struct OLEDDisplayUiState {};
using OverlayCallback = void (*)(OLEDDisplay *, OLEDDisplayUiState *);
struct OLEDDisplayUi {
 OLEDDisplay display;
 OLEDDisplayUiState state;
 OverlayCallback *overlays = nullptr;
 bool render = true;
 void setOverlays(OverlayCallback *value, int) { overlays = value; }
 void setTargetFPS(int) {}
};
void updateUiFrame(OLEDDisplayUi *ui) {
 if (ui->render) ui->overlays[1](&ui->display, &ui->state);
}
bool isDisplayDisabled() { return false; }
void setDisplayRailPower(bool) {}
uint32_t millis() { return 1; }
struct meshtastic_NodeInfoLite { uint32_t num = 0; char long_name[25] = {}, short_name[5] = {}; };
bool nodeInfoLiteHasUser(meshtastic_NodeInfoLite *node) { return node != nullptr; }
struct NodeDB {
 std::vector<meshtastic_NodeInfoLite> nodes;
 void pause_sort(bool) {}
 size_t getNumMeshNodes() { return nodes.size(); }
 meshtastic_NodeInfoLite *getMeshNodeByIndex(size_t i) { return &nodes.at(i); }
} db;
NodeDB *nodeDB = &db;
struct OnScreenKeyboardModule {
 static OnScreenKeyboardModule &instance() { static OnScreenKeyboardModule value; return value; }
 void stop(bool) {}
};
namespace graphics {
enum class notificationTypeEnum { none, text_input, text_banner, pairing_pin, selection_picker, node_picker };
@BANNER_OPTIONS@
struct Screen {
 enum { FOCUS_PRESERVE }; void setFrames(int) {}
 OLEDDisplayUi *ui;
 void showOverlayBanner(BannerOverlayOptions);
};
bool isCompactPanel(OLEDDisplay *) { return false; }
int bannerSignalBars = -1;
struct UIRenderer {
 static void drawNavigationBar(OLEDDisplay *, OLEDDisplayUiState *) {}
 static unsigned measureStringWithEmotes(OLEDDisplay *, const char *text) { return std::strlen(text); }
 static void truncateStringWithEmotes(OLEDDisplay *, const char *text, char *out, size_t size, int, const char *) {
  std::snprintf(out, size, "%s", text);
 }
 static void drawStringWithEmotes(OLEDDisplay *d, int x, int y, const char *text, int, int, bool) {
  d->drawString(x, y, text);
 }
};
class NotificationRenderer {
public:
 inline static InputEvent inEvent;
 @SELECTION_DECLARATIONS@
 inline static char alertBannerMessage[256] = {};
 inline static uint32_t alertBannerUntil = 0;
 inline static const char **optionsArrayPtr = nullptr;
 inline static const int *optionsEnumPtr = nullptr;
 inline static std::function<void(int)> alertBannerCallback;
 inline static bool pauseBanner = false;
 inline static uint32_t numDigits = 0, currentNumber = 0;
 inline static notificationTypeEnum current_notification_type = notificationTypeEnum::none;
 inline static uint8_t alertBannerLineCount = 0;
 inline static char alertBannerLines[MAX_LINES + 1][64] = {};
 enum BannerFont : uint8_t { BANNER_FONT_DEFAULT = 0 };
 inline static int alertBannerLineFonts[MAX_LINES + 1] = {};
 static const char *resolveBannerLine(uint16_t, const char *line, BannerFont &) { return line; }
 static void parseBannerMessageWithFonts(const char *) { alertBannerLineCount = 0; }
 static void resetBanner();
 static void drawAlertBannerOverlay(OLEDDisplay *, OLEDDisplayUiState *);
 static void drawNodePicker(OLEDDisplay *, OLEDDisplayUiState *);
 static void drawBannercallback(OLEDDisplay *d, OLEDDisplayUiState *s) { drawAlertBannerOverlay(d, s); }
 @BOX_DECLARATION@
};
@SELECTION_DEFINITIONS@
Screen *screen = nullptr;
int fontForBannerLine(int) { return FONT_SMALL; }
int effectiveLineHeightForBannerLine(int) { return FONT_HEIGHT_SMALL - 3; }
"""

TESTS = r"""
}
using Banner = graphics::NotificationRenderer;
using graphics::notificationTypeEnum;

OLEDDisplayUi ui;
graphics::Screen screenUnderTest{&ui};
OLEDDisplay &display = ui.display;
OLEDDisplayUiState &state = ui.state;
const char *options[] = {"Back", "US"};

void openMenu(const char **labels, size_t count, int initial = 0, const int *values = nullptr) {
 Banner::resetBanner();
 graphics::BannerOverlayOptions settings;
 settings.message = "Set the LoRa region";
 settings.notificationType = notificationTypeEnum::selection_picker;
 settings.optionsArrayPtr = labels;
 settings.optionsEnumPtr = values;
 settings.optionsCount = count;
 settings.InitialSelected = initial;
 screenUnderTest.showOverlayBanner(settings);
}

void openPicker() {
 openMenu(options, 2, 1);
 Banner::inEvent.inputEvent = INPUT_BROKER_SELECT;
}

void testReentrantCallbacks() {
 // A failed save replaces the picker with a warning and immediately redraws it.
 openPicker();
 int calls = 0;
 auto lifetime = std::make_shared<int>(42);
 std::weak_ptr<int> retained = lifetime;
 Banner::alertBannerCallback = [&, lifetime](int selected) {
  ++calls;
  assert(selected == 1);
  assert(Banner::inEvent.inputEvent == INPUT_BROKER_NONE);
  assert(Banner::alertBannerMessage[0] == '\0');
  assert(!Banner::alertBannerCallback);
  assert(!retained.expired());
  std::strcpy(Banner::alertBannerMessage, "Change not saved");
  Banner::current_notification_type = notificationTypeEnum::text_banner;
  Banner::drawAlertBannerOverlay(&display, &state);
  assert(*lifetime == 42);
 };
 lifetime.reset();
 Banner::drawAlertBannerOverlay(&display, &state);
 assert(calls == 1);
 assert(std::strcmp(Banner::alertBannerMessage, "Change not saved") == 0);
 assert(Banner::current_notification_type == notificationTypeEnum::text_banner);
 assert(retained.expired());

 // Capture the mapped value before reset, preserving a newly opened menu's callback.
 openPicker();
 const int values[] = {7, 19};
 Banner::optionsEnumPtr = values;
 Banner::alertBannerCallback = [&](int selected) {
  ++calls;
  assert(selected == 19);
  assert(Banner::optionsEnumPtr == nullptr);
  std::strcpy(Banner::alertBannerMessage, "Retry");
  Banner::optionsArrayPtr = options;
  Banner::alertBannerOptions = 2;
  Banner::optionsEnumPtr = values;
  Banner::alertBannerCallback = [&](int next) { assert(next == 7); ++calls; };
  Banner::drawAlertBannerOverlay(&display, &state);
 };
 Banner::drawAlertBannerOverlay(&display, &state);
 assert(calls == 2);
 assert(Banner::alertBannerCallback);
 assert(Banner::optionsEnumPtr == values);
 Banner::inEvent.inputEvent = INPUT_BROKER_SELECT;
 Banner::drawAlertBannerOverlay(&display, &state);
 assert(calls == 3);
 assert(Banner::alertBannerMessage[0] == '\0');

 openPicker();
 Banner::drawAlertBannerOverlay(&display, &state);
 assert(Banner::inEvent.inputEvent == INPUT_BROKER_NONE);
 assert(Banner::alertBannerMessage[0] == '\0');
}

void press(Input input) {
 display.drawn.clear();
 Banner::inEvent.inputEvent = input;
 Banner::drawAlertBannerOverlay(&display, &state);
}

void assertSelected(int selected) {
 if (Banner::curSelected != selected)
  std::fprintf(stderr, "selection: expected %d, actual %d, options %d\n", selected, Banner::curSelected, Banner::alertBannerOptions);
 assert(Banner::curSelected == selected);
 const std::string label = currentResolution == ScreenResolution::High
     ? "> item " + std::to_string(selected) + " <" : ">item " + std::to_string(selected) + "<";
 assert(std::find(display.drawn.begin(), display.drawn.end(), label) != display.drawn.end());
 assert(display.drawn.size() <= 8);
}

void testLongMenus() {
 for (int count : {1, 2, 127, 128, 129, 202, 254, 255, 256, 257, 300}) {
  std::vector<std::string> labels;
  for (int i = 0; i < count; ++i) labels.push_back("item " + std::to_string(i));
  std::vector<const char *> pointers;
  std::vector<int> values;
  for (int i = 0; i < count; ++i) { pointers.push_back(labels[i].c_str()); values.push_back(1000 + i); }
  display.drawn.clear();
  openMenu(pointers.data(), pointers.size());
  assert(Banner::alertBannerOptions == count);
  assertSelected(0);
  for (int selected = 1; selected < count; ++selected) {
   press(INPUT_BROKER_DOWN);
   assertSelected(selected);
  }
  press(INPUT_BROKER_DOWN);
  assertSelected(0);
  for (int selected = count - 1; selected >= 0; --selected) {
   press(INPUT_BROKER_UP);
   assertSelected(selected);
  }

  for (int initial : {-999, -1, 0, 127, 128, 254, 255, 256, count - 1, count, count + 1}) {
   const int expected = initial >= 0 && initial < count ? initial : 0;
   display.drawn.clear();
   openMenu(pointers.data(), pointers.size(), initial, values.data());
   assertSelected(expected);
   int received = -1;
   Banner::alertBannerCallback = [&](int selected) { received = selected; };
   press(INPUT_BROKER_SELECT);
   assert(received == 1000 + expected);
  }

  // SELECT must also validate an initial index before the first frame is drawn.
  ui.render = false;
  openMenu(pointers.data(), pointers.size(), -1, values.data());
  ui.render = true;
  int received = -1;
  Banner::alertBannerCallback = [&](int selected) { received = selected; };
  press(INPUT_BROKER_SELECT);
  assert(received == 1000);

  openMenu(pointers.data(), pointers.size(), count - 1);
  for (Input up : {INPUT_BROKER_UP, INPUT_BROKER_LEFT, INPUT_BROKER_ALT_PRESS, INPUT_BROKER_UP_LONG}) {
   Banner::curSelected = 0;
   press(up);
   assertSelected(count - 1);
  }
  for (Input down : {INPUT_BROKER_DOWN, INPUT_BROKER_RIGHT, INPUT_BROKER_USER_PRESS, INPUT_BROKER_DOWN_LONG}) {
   Banner::curSelected = count - 1;
   press(down);
   assertSelected(0);
  }
  int receivedIndex = -1;
  Banner::curSelected = count - 1;
  Banner::alertBannerCallback = [&](int selected) { receivedIndex = selected; };
  press(INPUT_BROKER_SELECT);
  assert(receivedIndex == count - 1);
 }
}

void testBoundedWindow() {
 Banner::resetBanner();
 ui.render = false;
 openMenu(options, static_cast<size_t>(std::numeric_limits<int>::max()) + 1);
 assert(Banner::alertBannerOptions == std::numeric_limits<int>::max());
 ui.render = true;
 Banner::resetBanner();
 // Scroll metadata must not allocate scratch arrays for every option in the menu.
 const char *window[] = {"Title", "Selected", nullptr};
 Banner::drawNotificationBox(&display, &state, window, std::numeric_limits<int>::max(), 300);
 Banner::drawNotificationBox(&display, &state, window, std::numeric_limits<int>::max(), std::numeric_limits<int>::max() - 1);
 for (uint16_t height : {0, 4, 11, 18}) {
  display.screenHeight = height;
  openMenu(options, 2);
  press(INPUT_BROKER_DOWN);
  assert(Banner::curSelected == 1);
 }
 display.screenHeight = 64;
 openMenu(nullptr, 0, 255);
 press(INPUT_BROKER_SELECT);
 assert(Banner::alertBannerMessage[0] == '\0');
}

void testLongLabels() {
 for (int length : {35, 36, 37, 38, 39, 40, 255}) {
  const std::string text(length, 'x');
  const char *labels[] = {text.c_str()};
  display.drawn.clear();
  openMenu(labels, 1);
  const bool high = currentResolution == ScreenResolution::High;
  const std::string expected = (high ? "> " : ">") + text.substr(0, high ? 35 : 37) + (high ? " <" : "<");
  assert(std::find(display.drawn.begin(), display.drawn.end(), expected) != display.drawn.end());
 }
}

void testNodeMenus() {
 for (size_t count : {0, 1, 127, 128, 129, 255, 256, 257, 300}) {
  Banner::resetBanner();
  Banner::current_notification_type = notificationTypeEnum::node_picker;
  std::strcpy(Banner::alertBannerMessage, "Select node");
  db.nodes.assign(count + 1, {});
  for (size_t i = 1; i <= count; ++i) {
   db.nodes[i].num = 1000 + i;
   std::snprintf(db.nodes[i].long_name, sizeof(db.nodes[i].long_name), "item %zu", i - 1);
  }
  for (size_t i = 0; i < count; ++i) {
   display.drawn.clear();
   Banner::inEvent.inputEvent = i == 0 ? INPUT_BROKER_NONE : INPUT_BROKER_DOWN;
   Banner::drawNodePicker(&display, &state);
   assertSelected(i);
  }
  display.drawn.clear();
  Banner::inEvent.inputEvent = INPUT_BROKER_DOWN;
  Banner::drawNodePicker(&display, &state);
  assert(Banner::curSelected == 0);
  if (count > 0) {
   display.drawn.clear();
   Banner::inEvent.inputEvent = INPUT_BROKER_UP;
   Banner::drawNodePicker(&display, &state);
   assertSelected(count - 1);
   int received = -1;
   Banner::alertBannerCallback = [&](int selected) { received = selected; };
   Banner::inEvent.inputEvent = INPUT_BROKER_SELECT;
   Banner::drawNodePicker(&display, &state);
   assert(received == static_cast<int>(1000 + count));
  }
 }
 db.nodes.clear();
 Banner::resetBanner();
 std::strcpy(Banner::alertBannerMessage, "Select node");
 Banner::drawNodePicker(&display, &state);
 assert(Banner::alertBannerOptions == 0);
}

void testReentrantNodeCallback() {
 Banner::resetBanner();
 Banner::current_notification_type = notificationTypeEnum::node_picker;
 std::strcpy(Banner::alertBannerMessage, "Select node");
 db.nodes.assign(2, {});
 db.nodes[1].num = 1001;
 std::strcpy(db.nodes[1].long_name, "Peer");
 Banner::drawNodePicker(&display, &state);
 int calls = 0;
 auto lifetime = std::make_shared<int>(42);
 std::weak_ptr<int> retained = lifetime;
 Banner::alertBannerCallback = [&, lifetime](int selected) {
  ++calls;
  assert(selected == 1001);
  assert(Banner::inEvent.inputEvent == INPUT_BROKER_NONE);
  assert(!Banner::alertBannerCallback);
  assert(!retained.expired());
  openMenu(options, 2, 1);
  Banner::alertBannerCallback = [&](int next) { assert(next == 1); ++calls; };
  Banner::drawAlertBannerOverlay(&display, &state);
  assert(*lifetime == 42);
 };
 lifetime.reset();
 Banner::inEvent.inputEvent = INPUT_BROKER_SELECT;
 Banner::drawNodePicker(&display, &state);
 assert(calls == 1);
 assert(retained.expired());
 assert(Banner::current_notification_type == notificationTypeEnum::selection_picker);
 assert(Banner::alertBannerCallback);
 press(INPUT_BROKER_SELECT);
 assert(calls == 2);
 assert(Banner::alertBannerMessage[0] == '\0');
}

int main() {
 testReentrantCallbacks();
 testReentrantNodeCallback();
 for (auto resolution : {ScreenResolution::Low, ScreenResolution::High}) {
  currentResolution = resolution;
  testLongMenus();
  testLongLabels();
  testNodeMenus();
 }
 testBoundedWindow();
}
"""


def main():
    source = (ROOT / "src/graphics/draw/NotificationRenderer.cpp").read_text()
    header = (ROOT / "src/graphics/draw/NotificationRenderer.h").read_text()
    screen_header = (ROOT / "src/graphics/Screen.h").read_text()
    screen_source = (ROOT / "src/graphics/Screen.cpp").read_text()
    declarations = []
    definitions = []
    for name in ("curSelected", "alertBannerOptions"):
        declarations.append(re.search(rf"static \w+ {name};", header)[0])
        definitions.append(
            re.search(rf"\w+ NotificationRenderer::{name} = 0;", source)[0]
        )
    prefix = PREFIX.replace(
        "@BANNER_OPTIONS@",
        function_body(screen_header, "struct BannerOverlayOptions") + ";",
    ).replace("@SELECTION_DECLARATIONS@", "\n".join(declarations))
    prefix = prefix.replace("@SELECTION_DEFINITIONS@", "\n".join(definitions))
    prefix = prefix.replace(
        "@BOX_DECLARATION@",
        re.search(r"static void drawNotificationBox\([^;]+;", header)[0],
    )
    methods = "\n".join(
        function_body(source, signature)
        for signature in (
            "void NotificationRenderer::resetBanner()",
            "void NotificationRenderer::drawAlertBannerOverlay(",
            "void NotificationRenderer::drawNodePicker(",
            "void NotificationRenderer::drawNotificationBox(",
        )
    )
    methods += "\n" + function_body(screen_source, "void Screen::showOverlayBanner(")
    with tempfile.TemporaryDirectory(prefix="banner-callback-") as directory:
        cpp = Path(directory) / "test.cpp"
        executable = Path(directory) / ("test.exe" if os.name == "nt" else "test")
        cpp.write_text(prefix + methods + TESTS)
        command = shlex.split(os.environ.get("CXX", "g++"))
        command += ["-std=c++17", "-O1", "-g", "-Wall", "-Wextra", "-Werror"]
        command += ["-Wno-unused-parameter", "-Wno-unused-variable"]
        sanitizer_flags = (
            "-fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie"
            if os.name == "posix"
            else ""
        )
        command += shlex.split(os.environ.get("BANNER_TEST_CXXFLAGS", sanitizer_flags))
        subprocess.run(command + [str(cpp), "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True, timeout=30)
    print(
        "Banner regression: PASS (production API, selection, window, reset and callback bodies)"
    )


if __name__ == "__main__":
    main()
