#include <Arduino.h>
#include <Fonts/FreeMonoBold18pt7b.h>
#include <Fonts/FreeMonoBold12pt7b.h>
#include <GxEPD2_BW.h>
#include <SPI.h>
#include <FS.h>
#include <SD.h>

#include "image.h"
#include "BatteryMonitor.h"
#include "InputManager.h"

#define SPI_FQ 40000000
// Display SPI pins (custom pins for XteinkX4, not hardware SPI defaults)
#define EPD_SCLK 8  // SPI Clock
#define EPD_MOSI 10 // SPI MOSI (Master Out Slave In)
#define EPD_CS 21   // Chip Select
#define EPD_DC 4    // Data/Command
#define EPD_RST 5   // Reset
#define EPD_BUSY 6  // Busy

#define UART0_RXD 20 // Used for USB connection detection
#define BAT_GPIO0 0 // Battery voltage

#define SD_SPI_CS   12
#define SD_SPI_MISO 7

static bool g_sdReady = false;
static int rawBat = 0;
static BatteryMonitor g_battery(BAT_GPIO0);
static InputManager input_manager;

// Display command enum
enum DisplayCommand
{
  DISPLAY_NONE = 0,
  DISPLAY_INITIAL,
  DISPLAY_TEXT,
  DISPLAY_BATTERY,
  DISPLAY_SLEEP
};

volatile DisplayCommand displayCommand = DISPLAY_NONE;

// GxEPD2 display - Using GxEPD2_426_GDEQ0426T82
// Note: XteinkX4 has 4.26" 800x480 display
GxEPD2_BW<GxEPD2_426_GDEQ0426T82, GxEPD2_426_GDEQ0426T82::HEIGHT> display(
  GxEPD2_426_GDEQ0426T82(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

// FreeRTOS task for non-blocking display updates
TaskHandle_t displayTaskHandle = NULL;

// Power button timing
const unsigned long POWER_BUTTON_WAKEUP_MS = 1000; // Time required to confirm boot from sleep
const unsigned long POWER_BUTTON_SLEEP_MS = 1000;  // Time required to enter sleep mode

// Check if charging
bool isCharging()
{
  // U0RXD/GPIO20 reads HIGH when USB is connected
  return digitalRead(UART0_RXD) == HIGH;
}

// Draw battery information on display
void drawBatteryInfo()
{
  display.setFont(&FreeMonoBold12pt7b);
  display.setCursor(20, 160);

  bool charging = isCharging();
  display.printf("Power: %s", charging ? "Charging" : "Battery");

  display.setCursor(40, 200);
  display.printf("Raw: %i", g_battery.readRawMillivolts());
  display.setCursor(40, 240);
  display.printf("Volts: %.2f V", g_battery.readVolts());
  display.setCursor(40, 280);
  display.printf("Charge: %i%%", g_battery.readPercentage());
}

// Draw up to top file names from SD on the display, below battery info
static void drawSdTopFiles()
{
  // Layout constants aligned with drawBatteryInfo() block
  const int startX = 40;
  const int startY = 350;
  const int lineHeight = 26;
  const int maxLines = 5;
  const int maxChars = 30;

  display.setFont(&FreeMonoBold12pt7b);

  display.setCursor(20, 320);
  display.print("Top 5 files on SD:");

  auto drawTruncated = [&](int lineIdx, const char *text)
  {
    // Render a single line, truncating with ellipsis if needed
    String s(text ? text : "");
    if ((int) s.length() > maxChars)
    {
      s.remove(maxChars - 1);
      s += "…";
    }
    display.setCursor(startX, startY + lineIdx * lineHeight);
    display.print(s);
  };

  // Ensure SD is initialized using global flag; try to init if needed
  if (!g_sdReady)
  {
    if (SD.begin(SD_SPI_CS, SPI, SPI_FQ))
    {
      g_sdReady = true;
    }
  }

  if (!g_sdReady)
  {
    drawTruncated(0, "No card");
    return;
  }

  File root = SD.open("/");
  if (!root || !root.isDirectory())
  {
    drawTruncated(0, "No card");
    if (root) root.close();
    return;
  }

  int count = 0;
  for (File f = root.openNextFile(); f && count < maxLines; f = root.openNextFile())
  {
    if (!f.isDirectory())
    {
      const char *name = f.name();
      // Ensure only name + extension, no leading path
      const char *basename = name;
      if (basename)
      {
        const char *slash = strrchr(basename, '/');
        if (slash && *(slash + 1))
          basename = slash + 1;
      }
      drawTruncated(count, basename ? basename : "");
      count++;
    }
    f.close();
  }

  if (count == 0)
  {
    drawTruncated(0, "Empty");
  }

  root.close();
}

// Display update task running on separate core
void displayUpdateTask(void *parameter)
{
  while (1)
  {
    if (displayCommand != DISPLAY_NONE)
    {
      DisplayCommand cmd = displayCommand;
      displayCommand = DISPLAY_NONE;

      if (cmd == DISPLAY_INITIAL)
      {
        // Use full window for initial welcome screen
        display.setFullWindow();
        display.firstPage();
        do
        {
          display.fillScreen(GxEPD_WHITE);

          // Header font
          display.setFont(&FreeMonoBold18pt7b);
          display.setCursor(20, 50);
          display.print("Xteink X4 Sample");

          // Button text with smaller font
          display.setFont(&FreeMonoBold12pt7b);
          display.setCursor(20, 100);
          bool anyPressed = false;
          for (int i = 0; i <= 6; i++)
          {
            if (input_manager.isPressed(i))
            {
              if (!anyPressed)
              {
                display.print("Pressing:");
                anyPressed = true;
              }
              display.print(" ");
              display.print(InputManager::getButtonName(i));
            }
          }
          if (!anyPressed)
          {
            display.print("Press any button");
          }

          // Draw battery information
          drawBatteryInfo();
          // Draw top 3 SD files below the battery block
          drawSdTopFiles();

          // Draw image at bottom right
          int16_t imgWidth = 263;
          int16_t imgHeight = 280;
          int16_t imgMargin = 20;
          int16_t imgX = 480 - imgMargin - imgWidth;
          int16_t imgY = 800 - imgMargin - imgHeight;
          display.drawBitmap(imgX, imgY, dr_mario, imgWidth, imgHeight, GxEPD_BLACK);
        } while (display.nextPage());
      }
      else if (cmd == DISPLAY_TEXT)
      {
        // Use partial refresh for text updates
        display.setPartialWindow(0, 75, display.width(), 225);
        display.firstPage();
        do
        {
          display.fillScreen(GxEPD_WHITE);
          display.setFont(&FreeMonoBold12pt7b);
          display.setCursor(20, 100);
          bool anyPressed = false;
          for (int i = 0; i <= 6; i++)
          {
            if (input_manager.isPressed(i))
            {
              if (!anyPressed)
              {
                display.print("Pressing:");
                anyPressed = true;
              }
              display.print(" ");
              display.print(InputManager::getButtonName(i));
            }
          }
          if (!anyPressed)
          {
            display.print("Press any button");
          }
          drawBatteryInfo();
        } while (display.nextPage());
      }
      else if (cmd == DISPLAY_BATTERY)
      {
        // Use partial refresh for battery updates
        display.setPartialWindow(0, 135, display.width(), 200);
        display.firstPage();
        do
        {
          display.fillScreen(GxEPD_WHITE);
          drawBatteryInfo();
        } while (display.nextPage());
      }
      else if (cmd == DISPLAY_SLEEP)
      {
        // Use full window for sleep screen
        display.setFullWindow();
        display.firstPage();
        do
        {
          display.fillScreen(GxEPD_WHITE);
          // Header font
          display.setFont(&FreeMonoBold18pt7b);
          display.setCursor(120, 380);
          display.print("Sleeping...");
        } while (display.nextPage());
      }
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

// Verify long press on wake-up from deep sleep
void verifyWakeupLongPress()
{
  bool abortBoot = false;
  input_manager.update();

  while (input_manager.getHeldTime() < POWER_BUTTON_WAKEUP_MS)
  {
    delay(10);
    input_manager.update();
    if (!input_manager.isPressed(InputManager::BTN_POWER))
    {
      abortBoot = true;
      break;
    }
  }

  if (abortBoot)
  {
    // Button released too early. Returning to sleep.
    // IMPORTANT: Re-arm the wakeup trigger before sleeping again
    esp_deep_sleep_enable_gpio_wakeup(1ULL << InputManager::POWER_BUTTON_PIN, ESP_GPIO_WAKEUP_GPIO_LOW);
    esp_deep_sleep_start();
  }
}

// Enter deep sleep mode
void enterDeepSleep()
{
  displayCommand = DISPLAY_SLEEP;
  delay(2000); // Allow Serial buffer to empty and display to update

  // Enable Wakeup on LOW (button press)
  esp_deep_sleep_enable_gpio_wakeup(1ULL << InputManager::POWER_BUTTON_PIN, ESP_GPIO_WAKEUP_GPIO_LOW);

  // Enter Deep Sleep
  esp_deep_sleep_start();
}

void setup()
{
  // Initialize inputs
  input_manager.begin();

  // Check if boot was triggered by the Power Button (Deep Sleep Wakeup)
  // If triggered by RST pin or Battery insertion, this will be false, allowing normal boot.
  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_GPIO)
  {
    verifyWakeupLongPress();
  }

  Serial.begin(115200);

  // Wait for serial monitor
  unsigned long start = millis();
  while (!Serial && (millis() - start) < 3000)
  {
    delay(10);
  }

  if (Serial)
  {
    // delay for monitor to start reading
    delay(1000);
  }


  Serial.println("\n=================================");
  Serial.println("  xteink x4 sample");
  Serial.println("=================================");
  Serial.println();

  // Initialize SPI with custom pins
  SPI.begin(EPD_SCLK,SD_SPI_MISO, EPD_MOSI, EPD_CS);
  // Initialize display
  SPISettings spi_settings(SPI_FQ, MSBFIRST, SPI_MODE0);
  display.init(115200, true, 2, false, SPI, spi_settings);

  // SD Card Initialization
  if (!SD.begin(SD_SPI_CS, SPI, SPI_FQ))
  {
    Serial.print("\n SD card not detected\n");
  }
  else
  {
    Serial.print("\n SD card detected\n");
    g_sdReady = true;
  }

  // Setup display properties
  display.setRotation(3); // 270 degrees
  display.setTextColor(GxEPD_BLACK);

  Serial.println("Display initialized");


  // Draw initial welcome screen
  displayCommand = DISPLAY_INITIAL;

  // Create display update task on core 0 (main loop runs on core 1)
  xTaskCreatePinnedToCore(displayUpdateTask,  // Task function
                          "DisplayUpdate",    // Task name
                          4096,               // Stack size
                          NULL,               // Parameters
                          1,                  // Priority
                          &displayTaskHandle, // Task handle
                          0                   // Core 0
  );

  Serial.println("Display task created");
  Serial.println("Setup complete!\n");

  // Avoid entering main loop while still holding power on button
  while (input_manager.isPressed(InputManager::BTN_POWER))
  {
    delay(10);
    input_manager.update();
  }
}

#ifdef DEBUG_IO
void debugIO()
{
  // log each button
  Serial.println("== Buttons ==");
  for (int i = 0; i <= 6; i++)
  {
    Serial.printf(
      "%s - wasPressed: %s, wasReleased: %s, isPressed: %s\n",
      InputManager::getButtonName(i),
      input_manager.wasPressed(i) ? "yes" : "no",
      input_manager.wasReleased(i) ? "yes" : "no",
      input_manager.isPressed(i) ? "yes" : "no"
    );
  }

  // log battery info
  rawBat = analogRead(BAT_GPIO0);
  Serial.printf("== Battery (charging: %s) ==\n", isCharging() ? "yes" : "no");
  Serial.print("Value from pin (raw/calibrated): ");
  Serial.print(rawBat);
  Serial.print(" / ");
  Serial.println(BatteryMonitor::millivoltsFromRawAdc(rawBat));
  Serial.print("Volts: ");
  Serial.println(g_battery.readVolts());
  Serial.print("Charge level: ");
  Serial.println(g_battery.readPercentage());
  Serial.println("");

  // SD card
}
#endif


void loop()
{
  input_manager.update();

  if (input_manager.wasAnyPressed() || input_manager.wasAnyReleased())
  {
    displayCommand = DISPLAY_TEXT;

#ifdef DEBUG_IO
    debugIO();
#endif

    if (input_manager.wasReleased(InputManager::BTN_POWER)) {
      // Power button long pressed => go to sleep
      if (input_manager.getHeldTime() > POWER_BUTTON_SLEEP_MS) {
        Serial.printf("Power button released after %lums. Entering deep sleep.\n", input_manager.getHeldTime());
        enterDeepSleep();
      }
    }
  }

  delay(50);
}
