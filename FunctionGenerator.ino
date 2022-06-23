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
#define ENABLE_EEPROM

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

#ifdef ENABLE_EEPROM
#include <avr/eeprom.h>
#endif

#ifdef ENABLE_WATCHDOG
#include <GyverWDT.h>
#include <avr/wdt.h>
#endif

#define ROT_B 2
#define ROT_A 3
#define ROT_PB 4

#define FSYNC 10

const char *verStr = "v2.0";
const char *loadMsgT = "Function";
const char *loadMsgB = "Generator";
const char *freqUnit = "Hz";

// Custome charactes for LCD
byte arrowLeft[] = {B00000, B00000, B00100, B01000, B11111, B01000, B00100, B00000};
byte arrowRight[] = {B00000, B00000, B00100, B00010, B11111, B00010, B00100, B00000};

unsigned long maxFreq = 1000000;
unsigned int encFreqStep = 1000;

// Whether the display needs to be redrawn
// Only used if the entire screen needs a refresh,
// otherwise individual areas are seperately updated.
unsigned char isDispDirty = 1;

typedef enum Waveform : unsigned char
{
  SIN = 0,
  CLK,
  TRI
};

typedef enum OutputStatus : unsigned char
{
  OFF = 0,
  ON
};

typedef enum MenuItem : unsigned char
{
  Function = 0,
  Frequency,
  Phase,
  Version
};

typedef enum DisplayFrame : unsigned char
{
  HOME_PAGE = 0,
  MENU,
  SETTING
};

// Character arrays used on the LCD
const char *waveformStr[3] = {EP(SIN), EP(CLK), EP(TRI)};
const char *outputStatStr[2] = {EP(OFF), EP(ON)};
const char *menuItmStr[4] = {EP(Function), EP(Frequency), EP(Phase), EP(Version)};
const char *memBanks[3] = {"MEM-A", "MEM-B", "CUST"};
const unsigned long memBankFreqs[2] = {10000, 500000};

// Loaded from the settings struct
Waveform curWaveform;
unsigned long curFreq;

// Whether function output is turned on
OutputStatus curOutputStat = OFF;

// Stores the current memory bank slot
unsigned char curMemBank = 0;

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

/**
 * Structure to hold EEPROM settings if EEPROM is enabled
 */
struct settings
{
  unsigned long freq[2];
  unsigned char waveform;
  bool channel;
  uint8_t checksum;
#ifdef ENABLE_EEPROM
} settings;
#else
} settings = {memBankFreqs[0], memBankFreqs[1], SIN, 0, 0};
#endif

LiquidCrystal_I2C lcd(0x27, 16, 2);
RotaryEncoder enc(ROT_B, ROT_A, RotaryEncoder::LatchMode::FOUR0);
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

  showStartScreen();

  sigGen.begin();

#ifdef ENABLE_WATCHDOG
  // Enable watchdog with 2s timeout
  Watchdog.enable(RESET_MODE, WDT_PRESCALER_256);
#endif

  Serial.begin(9600);
}

void loop()
{
  encBtn.tick();

  static int encPos = 0;
  int newPos = enc.getPosition();
  if (encPos != newPos)
  {
    encRotate();

    encPos = newPos;
  }

  if (isDispDirty)
    updateDisplay();
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
  lcd.setCursor(0, 0);
  lcd.print("F=");
  printFreq();

  printOutputStat();

  printCurMemBank();

  lcd.setCursor(13, 1);
  lcd.print(waveformStr[curWaveform]);
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
  switch (curMenuSel)
  {
  case 0:
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
  case 1:
    for (int i = 0; i < 2; i++)
    {
      lcd.setCursor(0, i);
      lcd.print(memBanks[i]);
      lcd.print(" ");
      lcd.print(memBankFreqs[i]);
      lcd.print(" ");
      lcd.print(freqUnit);
    }

    lcd.setCursor(0, 0);
    lcd.blink();

    break;
  case 3:
    printOnCenter((char *)verStr, 0);

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
  case 0:
    showHome();

    break;
  case 1:
    showMenu();

    break;
  case 2:
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
  for (int i = 2; i < 11; i++)
  {
    lcd.setCursor(i, 0);
    lcd.print(" ");
  }

  lcd.setCursor(2, 0);
  lcd.print(curFreq);
  lcd.print(freqUnit);
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
  lcd.print(outputStatStr[curOutputStat]);
}

/**
 * Print currently selected membank on main screen
 */
void printCurMemBank()
{
  for (int i = 0; i < 5; i++)
  {
    lcd.setCursor(i, 1);
    lcd.print(" ");
  }

  lcd.setCursor(0, 1);
  lcd.print(memBanks[curMemBank]);
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
  case 0:
    curOutputStat = !curOutputStat;
    printOutputStat();

    break;
  case 1:
    curDispFrame = 2;
    isDispDirty = 1;

    break;
  case 2:
    curDispFrame = 1;
    isDispDirty = 1;

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
  case 0:
    if (curMemBank < 1)
      curMemBank++;
    else
      curMemBank = 0;

    curFreq = memBankFreqs[curMemBank];

    printFreq();
    printCurMemBank();

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
  case 0:
    curDispFrame = 1;

    break;
  case 1:
    curDispFrame = 0;

    break;
  }

  isDispDirty = 1;
}

/**
 * Handles encoder rotation sequence
 */
void encRotate()
{
  int rotDir = ((int)enc.getDirection());

  if (curDispFrame == 0)
  {
    if (rotDir < 0)
    {
      long newFreq = curFreq + encFreqStep;

      if (newFreq < maxFreq)
        curFreq = newFreq;
      else
        curFreq = maxFreq;
    }
    else
    {
      long newFreq = curFreq - encFreqStep;

      if (newFreq > 0)
        curFreq = newFreq;
      else
        curFreq = 0;
    }

    if (curFreq != maxFreq || curFreq != 0)
      printFreq();

    // Update the memory bank to custom if its already not set
    if (curMemBank != 2)
    {
      curMemBank = 2;
      printCurMemBank();
    }
  }
  else if (curDispFrame == 1)
  {
    unsigned int arrLen = sizeof(menuItems) / sizeof(char *);

    if (rotDir < 0)
    {
      if (curMenuSel < arrLen - 1)
        curMenuSel++;
      else
        curMenuSel = arrLen - 1;
    }
    else
    {
      if (curMenuSel > 0)
        curMenuSel--;
      else
        curMenuSel = 0;
    }

    isDispDirty = 1;
  }
  else if (curDispFrame == 2)
  {
    switch (curMenuSel)
    {
    case 0:
      if (curWaveform == 0)
      {
        if (rotDir < 0)
        {
          lcd.setCursor(7, 0);
          lcd.blink();

          curWaveform = 1;
        }
        else
        {
          lcd.setCursor(13, 0);
          lcd.blink();

          curWaveform = 2;
        }
      }
      else if (curWaveform == 1)
      {
        if (rotDir < 0)
        {
          lcd.setCursor(13, 0);
          lcd.blink();

          curWaveform = 2;
        }
        else
        {
          lcd.setCursor(0, 0);
          lcd.blink();

          curWaveform = 0;
        }
      }
      else
      {
        if (rotDir < 0)
        {
          lcd.setCursor(0, 0);
          lcd.blink();

          curWaveform = 0;
        }
        else
        {
          lcd.setCursor(7, 0);
          lcd.blink();

          curWaveform = 1;
        }
      }

      break;
    }
  }
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
  unsigned int startPos = 16 - txtLen;

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
  unsigned int startPos = 8 - (txtLen / 2);

  lcd.setCursor(startPos, row);
  lcd.print(text);
}