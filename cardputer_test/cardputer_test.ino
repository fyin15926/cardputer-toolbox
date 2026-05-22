// Minimal toolchain test for M5Stack Cardputer-Adv
// Shows a hello message and echoes whatever key you press.
#include <M5Cardputer.h>

void setup() {
  auto cfg = M5.config();
  M5Cardputer.begin(cfg, true);  // true = enable keyboard

  M5Cardputer.Display.setRotation(1);
  M5Cardputer.Display.fillScreen(BLACK);
  M5Cardputer.Display.setTextColor(GREEN, BLACK);
  M5Cardputer.Display.setTextSize(2);
  M5Cardputer.Display.setCursor(0, 0);
  M5Cardputer.Display.println("Hello Cardputer-Adv!");
  M5Cardputer.Display.setTextSize(1);
  M5Cardputer.Display.println("");
  M5Cardputer.Display.println("Press any key to test...");
}

void loop() {
  M5Cardputer.update();

  if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
    auto status = M5Cardputer.Keyboard.keysState();
    M5Cardputer.Display.setTextColor(YELLOW, BLACK);
    M5Cardputer.Display.print("Key: ");
    for (auto c : status.word) {
      M5Cardputer.Display.print(c);
    }
    M5Cardputer.Display.println("");
  }
}
