/**
 * Copyright 2022 Saminda Peramuna. All rights reserved.
 *
 * Portions of the code were adopted from ShaggyDog18s implementaion
 * https://github.com/ShaggyDog18/SignalGeneratorSD/blob/master/SignalGeneratorSD.ino
 *
 * Hardware by GreatScott!
 * https://www.instructables.com/id/DIY-FunctionWaveform-Generator/
 *
 **/

// Enum print function
#define EP(x) [x] = #x

// Save settings to EEPROM and recover them at start-up
// #define ENABLE_EEPROM

// Apply freq. value "on the fly" with 0.4s delay
#define RUNNING_FREQUENCY

// Start address in EEPROM to store settings
// Change to an unused area if an "EEPROM CRC Error" occurs
#define EEPROM_ADDRESS_SHIFT 0x00

// Enable Gyver watchdog
#define ENABLE_WATCHDOG

#include <MD_AD9833.h>
#include <LiquidCrystal_I2C.h>
#include <RotaryEncoder.h>
#include <OneButton.h>
#include <math.h>

#ifdef ENABLE_EEPROM
#include <avr/eeprom.h>
#endif

#ifdef ENABLE_WATCHDOG
#include <GyverWDT.h>
#include <avr/wdt.h>
#endif

#define ROT_A 3
#define ROT_B 2
#define ROT_PB 4

#define FSYNC 10

// LCD related settings
#define LCD_I2C_ADDR 0x27
#define LCD_COLS 16
#define LCD_ROWS 2

const char *verStr = "v2.0";
const char *loadMsgT = "Function";
const char *loadMsgB = "Generator";
const char *freqUnit = "Hz";

// Custome charactes for LCD
byte arrowLeft[] = {B00000, B00000, B00100, B01000, B11111, B01000, B00100, B00000};
byte arrowRight[] = {B00000, B00000, B00100, B00010, B11111, B00010, B00100, B00000};

unsigned long maxFreq = 1000000;
unsigned long maxFreqStep = 100000;
unsigned int encFreqStep = 1000;

// Whether the display needs to be redrawn
// Only used if the entire screen needs a refresh,
// otherwise individual areas are seperately updated.
bool isDispDirty = 1;

// Whether the output frequency of the AD9833 module should be update or not.
// Only activate if RUNNINIG_FREQUENCY is defined.
bool isFrequencyDirty = 1;

enum Waveform : byte
{
   SIN,
   CLK,
   TRI
};

enum OutputStatus : byte
{
   OFF,
   ON
};

enum MenuItem : byte
{
   Function,
   Frequency,
   Phase,
   Version
};

enum DisplayFrame : byte
{
   HOME_PAGE,
   MENU,
   SETTING
};

// Character arrays used on the LCD
const char *waveformStr[3] = {EP(SIN), EP(CLK), EP(TRI)};
const char *outputStatStr[2] = {EP(OFF), EP(ON)};
const char *menuItmStr[4] = {EP(Function), EP(Frequency), EP(Phase), EP(Version)};
const char *channelStr[2] = {"CH-1", "CH-2"};
const unsigned long channelFreqs[2] = {10000, 1000};

// Whether function output is turned on
OutputStatus outputStat = ON;

// Currently selected menu item
MenuItem curMenuSel = Function;

#ifdef RUNNING_FREQUENCY
// 400ms delay between application and the AD9833 module
// This is used to avoid every freq value change triggering a freq change in the module.
const unsigned long freqUpdateDelay = 400L;

// Last time freq value was sent to the module (in milliseconds)
unsigned long lastFreqUpdate;
#endif

// Currently selected display frame
DisplayFrame curDispFrame = HOME_PAGE;

// Holds LCD cursor row posion
byte curRow = 0;

/**
 * Structure to hold EEPROM settings if EEPROM is enabled
 */
struct Settings
{
   unsigned long frequency[2];
   unsigned long freqStep;
   Waveform waveform;
   bool channel;
   byte checksum;
#ifdef ENABLE_EEPROM
} settings;
#else
} settings = {channelFreqs[0], channelFreqs[1], encFreqStep, SIN, 0, 0};
#endif

LiquidCrystal_I2C lcd(LCD_I2C_ADDR, LCD_COLS, LCD_ROWS);
RotaryEncoder enc(ROT_B, ROT_A, RotaryEncoder::LatchMode::TWO03);
OneButton encBtn(ROT_PB, true, true);
MD_AD9833 sigGen(FSYNC);

void setup()
{
   lcd.init();
   lcd.backlight();
   lcd.createChar(0, arrowLeft);
   lcd.createChar(1, arrowRight);

   // Add interrupts for encoder A and B pins
   attachInterrupt(digitalPinToInterrupt(ROT_A), checkPosition, CHANGE);
   attachInterrupt(digitalPinToInterrupt(ROT_B), checkPosition, CHANGE);

   // Add event handlers for encoder button
   encBtn.attachClick(encPress);
   encBtn.attachDoubleClick(encDoublePress);
   encBtn.attachLongPressStart(encLongPress);

#ifdef ENABLE_EEPROM
   bool resetSettings = false;

   if (!readFromEEPROM())
   {
      lcd.setCursor(0, 0);
      lcd.print(F("Error:CRC EEPROM"));
      printResetMsg(1);

      _delay_ms(3000);

      resetSettings = true;

      lcd.clear();
   }

   if (resetSettings)
   {
      resetSettings();
      writeToEEPROM();
   }
#endif

   showStartScreen();

   sigGen.begin();

   setADFreq(0, settings.frequency[0]);
   setADFreq(1, settings.frequency[1]);
   setADChannel(settings.channel);

   toggleOutput();
#ifdef ENABLE_WATCHDOG
   // Enable watchdog with 2s timeout
   Watchdog.enable(RESET_MODE, WDT_PRESCALER_256);
#endif

   Serial.begin(115200);
}

void loop()
{
   encBtn.tick();

#ifdef RUNNING_FREQUENCY
   if (isFrequencyDirty)
   {
      if (millis() - lastFreqUpdate > freqUpdateDelay)
      {
         setADFreq(settings.channel, settings.frequency[settings.channel]);
         isFrequencyDirty = false;
      }
   }
#endif

   static int encPos = 0;
   int newPos = enc.getPosition();
   if (encPos != newPos)
   {
      encRotate();

      encPos = newPos;
   }

   if (isDispDirty)
      updateDisplay();

#ifdef ENABLE_WATCHDOG
   // Reset watchdog, because the loop is running OK.
   Watchdog.reset();
#endif
}

/**
 * Prints start message on the LCD
 */
void showStartScreen()
{
   lcd.setCursor(4, 0);
   lcd.print(loadMsgT);
   lcd.setCursor(1, 1);
   lcd.print(loadMsgB);
   lcd.print(' ');
   lcd.print(verStr);

   delay(2000);

   lcd.clear();
}

/**
 * Display the home screen
 */
void showHome()
{
   printFreq();

   printOutputStat();

   printChannel();

   printWaveform();
}

/**
 * Display the settings menu
 */
void showMenu()
{
   // Remove blink in case comming from the settings.
   lcd.noBlink();

   printOnCenter(menuItmStr[curMenuSel], 0);

   unsigned int arrLen = sizeof(menuItmStr) / sizeof(char *);

   if (curMenuSel > 0)
   {
      lcd.setCursor(0, 0);
      lcd.write(0);
   }

   if (curMenuSel < arrLen - 1)
   {
      lcd.setCursor(15, 0);
      lcd.write(1);
   }
}

/**
 * Show the selected setting
 */
void showSetting()
{
   Waveform curWaveform = settings.waveform;
   unsigned short len = sizeof(channelStr) / sizeof(char *);

   switch (curMenuSel)
   {
   case MenuItem::Function:
      lcd.setCursor(0, 0);
      lcd.print(waveformStr[0]);
      printOnCenter(waveformStr[1], 0);
      printOnRight(waveformStr[2], 0);

      if (curWaveform == 0)
         lcd.setCursor(0, 0);
      else if (curWaveform == 1)
         lcd.setCursor(7, 0);
      else
         lcd.setCursor(13, 0);

      lcd.blink();

      break;
   case MenuItem::Frequency:
      printFreq();
      printStep();

      lcd.setCursor(0, curRow);
      lcd.blink();

      break;
   case MenuItem::Phase:
      printOnCenter("N/A", 0);

      break;
   case MenuItem::Version:
      printOnCenter(verStr, 0);

      break;
   }
}

/**
 * Display different screens depending on the situation
 */
void updateDisplay()
{
   lcd.clear();

   switch (curDispFrame)
   {
   case DisplayFrame::HOME_PAGE:
      showHome();

      break;
   case DisplayFrame::MENU:
      showMenu();

      break;
   case DisplayFrame::SETTING:
      showSetting();

      break;
   }

   isDispDirty = 0;
}

/**
 * Print the current frequency on the home screen
 */
void printFreq()
{
   lcd.setCursor(0, 0);
   lcd.print("F=");

   for (int i = 2; i < 11; i++)
   {
      lcd.setCursor(i, 0);
      lcd.print(" ");
   }

   lcd.setCursor(2, 0);
   lcd.print(settings.frequency[settings.channel]);
   lcd.print(freqUnit);
}

void printStep()
{
   lcd.setCursor(0, 1);
   lcd.print("S=");

   for (int i = 2; i < 8; i++)
   {
      lcd.setCursor(i, 1);
      lcd.print(" ");
   }

   lcd.setCursor(2, 1);
   lcd.print(settings.freqStep);
}

/**
 * Print the output status on the home screen
 */
void printOutputStat()
{
   for (int i = 13; i < 16; i++)
   {
      lcd.setCursor(i, 0);
      lcd.print(" ");
   }

   lcd.setCursor(13, 0);
   lcd.print(outputStatStr[outputStat]);
}

/**
 * Print currently selected membank on main screen
 */
void printChannel()
{
   for (int i = 0; i < 5; i++)
   {
      lcd.setCursor(i, 1);
      lcd.print(" ");
   }

   lcd.setCursor(0, 1);
   lcd.print(channelStr[settings.channel]);
}

/**
 * Print the waveform for currently selected channel on main screen
 */
void printWaveform()
{
   lcd.setCursor(13, 1);
   lcd.print(waveformStr[settings.waveform]);
}

/**
 * Interrupt handler function for encoder
 */
void checkPosition()
{
   enc.tick();
}

/**
 * Handles encoder click event
 */
void encPress()
{
   switch (curDispFrame)
   {
   case HOME_PAGE:
      toggleOutput();
      printOutputStat();

      break;
   case MENU:
      curDispFrame = SETTING;
      isDispDirty = 1;

      break;
   case SETTING:
      switch (curMenuSel)
      {
      case Frequency:
         curRow = !curRow;

         lcd.setCursor(0, curRow);
         lcd.blink();

         break;
      default:
         curDispFrame = MENU;
         isDispDirty = 1;

         break;
      }

      break;
   }
}

/**
 * Handles encoder double click event
 */
void encDoublePress()
{
   switch (curDispFrame)
   {
   case HOME_PAGE:
      settings.channel = !settings.channel;

      setADChannel(settings.channel);

      printFreq();
      printChannel();
      printWaveform();

      break;
   }
}

/**
 * Handles encoder long press event
 */
void encLongPress()
{
   switch (curDispFrame)
   {
   case HOME_PAGE:
      curDispFrame = MENU;

      break;
   case MENU:
      // Reset menu selection
      curMenuSel = (MenuItem)0;
      curDispFrame = HOME_PAGE;

      break;
   case SETTING:
      switch (curMenuSel)
      {
      case Frequency:
         curDispFrame = MENU;
         curRow = 0;

         break;
      }

      break;
   }

   isDispDirty = 1;
}

/**
 * Handles encoder rotation sequence
 */
void encRotate()
{
   RotaryEncoder::Direction rotDir = enc.getDirection();

   switch (curDispFrame)
   {
   case HOME_PAGE:
      changeFreq(rotDir);

      break;
   case MENU:
      changeMenu(rotDir);

      break;
   case SETTING:
      handleSettingChange(rotDir);

      break;
   }
}

/**
 * Change frequency of currently selected channel when encoder is rotated.
 *
 * @param rotDir Direction of rotation of encoder
 */
void changeFreq(RotaryEncoder::Direction rotDir)
{
   unsigned long *curFreq = &settings.frequency[settings.channel];

   if (rotDir == RotaryEncoder::Direction::CLOCKWISE)
   {
      long newFreq = *curFreq + settings.freqStep;
      *curFreq = newFreq < maxFreq ? newFreq : maxFreq;
   }
   else if (rotDir == RotaryEncoder::Direction::COUNTERCLOCKWISE)
   {
      long newFreq = *curFreq - settings.freqStep;
      *curFreq = newFreq > 0 ? newFreq : settings.freqStep;
   }

#ifdef RUNNING_FREQUENCY
   lastFreqUpdate = millis();
   isFrequencyDirty = true;
#endif

   printFreq();
}

/**
 * Change frequency step
 *
 * @param rotDir Direction of rotation of encoder
 */
void changeStep(RotaryEncoder::Direction rotDir)
{
   unsigned long *curStep = &settings.freqStep;
   unsigned int idc = log10(*curStep);

   if (rotDir == RotaryEncoder::Direction::CLOCKWISE)
      idc = idc < log10(maxFreqStep) ? idc + 1 : log10(maxFreqStep);
   else
      idc = idc > 1 ? idc - 1 : 0;

   *curStep = round(pow(10, idc));

   printStep();
}

/**
 * Change selected menu item on encoder rotation
 *
 * @param rotDir Direction of rotation of encoder
 */
void changeMenu(RotaryEncoder::Direction rotDir)
{
   unsigned int arrLen = sizeof(menuItmStr) / sizeof(char *);

   if (rotDir == RotaryEncoder::Direction::CLOCKWISE)
   {
      if (curMenuSel < arrLen - 1)
         curMenuSel = (MenuItem)(curMenuSel + 1);
      else
         curMenuSel = Version;
   }
   else if (rotDir == RotaryEncoder::Direction::COUNTERCLOCKWISE)
   {
      if (curMenuSel > 0)
         curMenuSel = (MenuItem)(curMenuSel - 1);
      else
         curMenuSel = Function;
   }

   isDispDirty = 1;
}

/**
 * Handle menu item settings change
 *
 * @param rotDir Direction of rotation of encoder
 */
void handleSettingChange(RotaryEncoder::Direction rotDir)
{
   // Fetch the waveform for the current channel
   Waveform *curWaveform = &settings.waveform;
   unsigned short len = sizeof(waveformStr) / sizeof(char *);

   switch (curMenuSel)
   {
   case Function:

      if (rotDir == RotaryEncoder::Direction::CLOCKWISE)
         *curWaveform = *curWaveform + 1 < len ? *curWaveform + 1 : 0;

      else if (rotDir == RotaryEncoder::Direction::COUNTERCLOCKWISE)
         *curWaveform = *curWaveform > 0 ? *curWaveform - 1 : len - 1;

      toggleOutput();
      setWaveform(curWaveform);

      break;
   case Frequency:
      if (curRow == 0)
         changeFreq(rotDir);
      else
         changeStep(rotDir);

      lcd.setCursor(0, curRow);

      break;
   }
}

/**
 * Emulate the selection by blinking cursor on the appropriate choice
 * and set waveform on generator.
 *
 * @param newWaveform Waveform on which the cursor should blink
 */
void setWaveform(Waveform *newWaveform)
{
   switch (*newWaveform)
   {
   case SIN:
      lcd.setCursor(0, 0);
      lcd.blink();

      break;
   case CLK:
      lcd.setCursor(7, 0);
      lcd.blink();

      break;
   case TRI:
      lcd.setCursor(13, 0);
      lcd.blink();

      break;
   }
}

/**
 * Toggle the output status of the AD9833 module.
 */
void toggleOutput()
{
   outputStat = (OutputStatus) !(bool)outputStat;

   if (outputStat == OFF)
      sigGen.setMode(MD_AD9833::mode_t::MODE_OFF);
   else
      setADSigMode(settings.waveform);
}

/**
 * Prints the given text on the right side of the LCD.
 *
 * @param text Text to be printed on the LCD
 * @param row Row number to print the text on
 */
void printOnRight(const char *text, unsigned int row)
{
   unsigned int txtLen = strlen(text);
   unsigned int startPos = LCD_COLS - txtLen;

   lcd.setCursor(startPos, row);
   lcd.print(text);
}

/**
 * Prints the given text on the center of the LCD.
 *
 * @param text Text to be printed on the LCD
 * @param row Row number to print the text on
 */
void printOnCenter(const char *text, unsigned int row)
{
   unsigned int txtLen = strlen(text);
   unsigned int startPos = (LCD_COLS / 2) - (txtLen / 2);

   lcd.setCursor(startPos, row);
   lcd.print(text);
}

/**
 * Read settings from the EEPROM.
 *
 * @return true If CRC checksum is OK
 * @return false If CRC checksum fails
 */
bool readFromEEPROM()
{
   eeprom_read_block((void *)&settings, EEPROM_ADDRESS_SHIFT, sizeof(Settings));

   // Check the CRC8 checksum of the read memory block
   // Last enum member is a byte of checksum, so passing one less byte
   byte checksum = crc8block((byte *)&settings, sizeof(Settings) - 1);
   if (checksum == settings.checksum)
      return true;

   return false;
}

/**
 * Write settings to EEPROM.
 */
void writeToEEPROM()
{
   // Calculate CRC8 checksum and save it with the settings
   byte checksum = crc8block((byte *)&settings, sizeof(Settings) - 1);

   settings.checksum = checksum;

   eeprom_update_block((void *)&settings, EEPROM_ADDRESS_SHIFT, sizeof(Settings));
}

/**
 * Generate a CRC8 checksum using the given memory block.
 *
 * @param pcBlock Memory block to be used
 * @param len Length of memory block
 * @return unsigned char Generated CRC8 block
 */
unsigned char crc8block(byte *pcBlock, unsigned short len)
{
   byte crc = 0xFF;
   byte i;

   while (len--)
   {
      crc ^= *pcBlock++;
      for (i = 0; i < 8; i++)
         crc = (crc & 0x80) ? (crc << 1) ^ 0x31 : crc << 1;
   }

   return crc;
}

/**
 * Blinks the LED backlight
 *
 * @param nBlinks No of blinks
 */
void blinkDisplayBacklight(const byte nBlinks)
{
   for (byte i = 0; i < nBlinks; i++)
   {
      lcd.noBacklight();
      _delay_ms(300);

      lcd.backlight();
      _delay_ms(300);
   }
}

/**
 * Set the waveform on the AD9833 device
 *
 * @param curMode Current waveform selected
 */
void setADSigMode(const Waveform curMode)
{
   switch (curMode)
   {
   case SIN:
      sigGen.setMode(MD_AD9833::mode_t::MODE_SINE);

      break;
   case CLK:
      sigGen.setMode(MD_AD9833::mode_t::MODE_SQUARE1);

      break;
   case TRI:
      sigGen.setMode(MD_AD9833::mode_t::MODE_TRIANGLE);

      break;
   }
}

/**
 * Set the frequency on the AD9833 device
 *
 * @param channel Channel to affect on the device
 * @param freq Currently selected frequency
 */
void setADFreq(const bool channel, const unsigned long freq)
{
   sigGen.setFrequency(channel ? MD_AD9833::channel_t::CHAN_1 : MD_AD9833::channel_t::CHAN_0, freq);
}

/**
 * Set the channel on the AD9833 device
 *
 * @param channel Channel to change to
 */
void setADChannel(const bool channel)
{
   if (channel)
   {
      sigGen.setActiveFrequency(MD_AD9833::channel_t::CHAN_1);
   }
   else
   {
      sigGen.setActiveFrequency(MD_AD9833::channel_t::CHAN_0);
   }
}

/**
 * Prints the reset message on the given line
 *
 * @param line Line on which the error message is printed
 */
void printResetMsg(const byte line)
{
   lcd.setCursor(0, line);
   lcd.print(F("Reset to default"));
}

/**
 * Reset settings to default
 */
void resetSettings()
{
   printResetMsg(0);

   settings.frequency[0] = channelFreqs[0];
   settings.frequency[0] = channelFreqs[1];
   settings.freqStep = encFreqStep;
   settings.waveform = CLK;
   settings.channel = false;

   blinkDisplayBacklight(3);
}