#include <SD.h>
#include <Adafruit_SH110X.h>
#include <PS2KeyAdvanced.h>

const int chipSelect = BUILTIN_SDCARD;

#define INPUT_LEN 32
char inputBuffer[INPUT_LEN] = "";
int inputLen = 0;

#define NAME_LEN 12

#define PATH_LEN 16
char currentPath[PATH_LEN] = "/";

#define DMESG_LINES 6
#define DMESG_LEN 40
typedef struct {
  unsigned long timestamp;
  char message[DMESG_LEN];
} DmesgEntry;
DmesgEntry dmesg[DMESG_LINES];
int dmesgIndex = 0;

#define MAX_ALIASES 4
#define ALIAS_NAME_LEN 6
#define ALIAS_VAL_LEN 20
typedef struct {
  char name[ALIAS_NAME_LEN];
  char value[ALIAS_VAL_LEN];
  int active;
} AliasEntry;
AliasEntry aliases[MAX_ALIASES];

#define VAR_SPACES 6
#define VAR_NAME_LEN 8
typedef struct {
  float value;
  char name[VAR_NAME_LEN];
} shVariable;
shVariable vars[VAR_SPACES];
int varIndex = 0;

#define OLED_MOSI     10
#define OLED_CLK      8
#define OLED_DC       7
#define OLED_CS       5
#define OLED_RST      9
Adafruit_SH1106G display = Adafruit_SH1106G(128, 64,OLED_MOSI, OLED_CLK, OLED_DC, OLED_RST, OLED_CS);
#define COLS 21
#define ROWS 8

#define DataPin 13
#define IRQpin 14
PS2KeyAdvanced keyboard;
#define MAX_LEN 550 //There are usually around 400,000 bytes free in RAM1 for this but don't push it
char fileBuffer[MAX_LEN] = "Alfred Nobel invented Dynamite; he made a fortune manufacturing and selling deadly weapons, canons and armaments. In 1888, his brother Ludvig died, but many newspapers mistakenly thought that he had died and published obituaries for Alfred Nobel, they weren't very flattering, one French paper declared the \"merchant of death is dead.\" Nobel read these obituaries and was so ashamed by what his legacy apparently was going to be. When he did die, he left almost all of his money to the cause of celebrating humanity, he created the Nobel Prize.";
const int linesEstimation = floor(MAX_LEN/3);
int lineStarts[linesEstimation];
int persistentCursor = 0;
char clipboard[MAX_LEN];
int clipboardLen = 0;
int apparentLine = 0;
int screenLine;
int saveState = 1;

float freeMemory() {
  Sd2Card card;
  SdVolume volume;

  if (!card.init(SPI_HALF_SPEED, chipSelect)) {
    Serial.println("initialization failed");
    return 0;
  }

  volume.init(card);

  uint32_t volumesize;

  volumesize = volume.blocksPerCluster();
  volumesize *= volume.clusterCount();
  volumesize /= 2;
  volumesize /= 1024;
  float gigs = ((float)volumesize / 1024.0);

  return gigs;
}

void(* resetFunc) (void) = 0;

// OPT from issue
void addDmesg(const __FlashStringHelper* msg) {
  if (dmesgIndex >= DMESG_LINES) dmesgIndex = 0;
  dmesg[dmesgIndex].timestamp = millis() / 1000;
  strncpy_P(dmesg[dmesgIndex].message, (PGM_P)msg, DMESG_LEN - 1);
  dmesg[dmesgIndex].message[DMESG_LEN - 1] = '\0';
  dmesgIndex++;
}

void printPrompt() {
  Serial.print(F("root@lookingglass:"));
  Serial.print(currentPath);
  Serial.print(F("# "));
}

void toLowercase(char* str) {
  int i;
  for (i = 0; str[i] != '\0'; i++) {
    if (str[i] >= 'A' && str[i] <= 'Z') str[i] = str[i] - 'A' + 'a';
  }
}

int indexOf(const char* str, const char* substr) { //Find the point in a string where a substring is
  int i, j, slen = strlen(str), sublen = strlen(substr);
  for (i = 0; i <= slen - sublen; i++) {
    int match = 1;
    for (j = 0; j < sublen; j++) {
      if (str[i + j] != substr[j]) { match = 0; break; }
    }
    if (match) return i;
  }
  return -1;
}

int safeConcatPath(char* dest, const char* add) { //Determine if result of cd will be too long for the path buffer, if not it sets the directory to that result
  int destLen = strlen(dest);
  int addLen = strlen(add);
  if (destLen + addLen + 2 >= PATH_LEN) return 0;
  strncat(dest, add, PATH_LEN - destLen - 1);
  strncat(dest, "/", PATH_LEN - strlen(dest) - 1);
  return 1;
}

float decimalCharToFloat(char* str) {
  int beforeDec = 0;
  int afterDec = 0;
  int pos = 0;
  float total = 0;
  for (int i = 0; str[i] != '\0'; i++) {
    if (str[i] == '.') {
      pos = 1;
    } else if ((str[i] - '0') >= 0 && (str[i] - '0') <= 9) {
      if (pos) {
        afterDec++;
      } else {
        beforeDec++;
      }
    } else {
      Serial.println(F("Invalid number"));
      return 0;
    }
  }
  for (int i = 0; i < beforeDec; i++) {
    total = total + (str[i] - '0')*pow(10, (beforeDec-i-1));
  }
  for (int i = 1; i <= afterDec; i++) {
    total = total + (str[beforeDec + i] - '0')*pow(10, -i);
  }
  return total;
}

void variableSubstitute(char* str) { //Replaces variable name with the value of that variable
  for (int i = 0; i < varIndex; i++) { //Check for variables in entry
    int varCheck = indexOf(str, vars[i].name);
    if (varCheck != -1) {
      int j = 0; //Length of variable name
      while (vars[i].name[j] != '\0') {
        j++;
      }

      char flOut[9];
      int k; //Length of variable value
      if (fmod(vars[i].value, 1) == 0.00) {
        
        if (vars[i].value == 1) {
          k = 1;
        } else {
          k = ceil(log10(vars[i].value));
        }
        sprintf(flOut, "%f", vars[i].value);
      } else {
        k = 7;
        sprintf(flOut, "%f", vars[i].value);
      }

      int m = 0;
      char substitute[INPUT_LEN];
      for (int l = 0; l < varCheck; l++) {
        substitute[m] = str[l];
        m++;
      }
      for (int l = 0; l < k; l++) {
        substitute[m] = flOut[l];
        m++;
      }
      for (int l = (varCheck + j); l < INPUT_LEN; l++) {
        substitute[m] = str[l];
        m++;
        if (str[l] == '\0') {
          break;
        }
      }
      substitute[m] = '\0';
      strcpy(str, substitute);
    }
  }
}

void solveArithmetic(char* str) {
  char tempFloat[8];
  int termsIndex = 0;
  float firstVal = 0;
  float secondVal;
  int nextOp = 0;

  for (int i = 0; str[i] != '\0'; i++) {
    if (str[i] == '+' || str[i] == '-' || str[i] == '*' || str[i] == '/') {
      tempFloat[termsIndex] = '\0';
      termsIndex = 0;
      secondVal = decimalCharToFloat(tempFloat);
      if (nextOp == 0) {
        firstVal = firstVal + secondVal;
      } else if (nextOp == 1) {
        firstVal = firstVal - secondVal;
      } else if (nextOp == 2) {
        firstVal = firstVal * secondVal;
      } else if (nextOp == 3) {
        firstVal = firstVal / secondVal;
      }

      if (str[i] == '+') {
        nextOp = 0;
      } else if (str[i] == '-') {
        nextOp = 1;
      } else if (str[i] == '*') {
        nextOp = 2;
      } else if (str[i] == '/') {
        nextOp = 3;
      }
    } else {
      tempFloat[termsIndex] = str[i];
      termsIndex++;
    }
  }
  tempFloat[termsIndex] = '\0';
  secondVal = decimalCharToFloat(tempFloat);
  if (nextOp == 0) {
    firstVal = firstVal + secondVal;
  } else if (nextOp == 1) {
    firstVal = firstVal - secondVal;
  } else if (nextOp == 2) {
    firstVal = firstVal * secondVal;
  } else if (nextOp == 3) {
    firstVal = firstVal / secondVal;
  }

  if (fmod(firstVal, 1) == 0.00) {
    int firstValInt = firstVal;
    sprintf(str, "%d", firstValInt);
  } else {
    sprintf(str, "%f", firstVal);
  }
}

void solveParentheses(char* str) {
  int startingIndex;
  int endingIndex = 0;
  char tempString[INPUT_LEN] = "";

  while (str[endingIndex] != ')') {
    endingIndex++;
  }
  startingIndex = endingIndex;
  while (str[startingIndex] != '(') {
    startingIndex--;
  }

  int j = 0;
  for (int i = startingIndex+1; i < endingIndex; i++) {
    tempString[j] = str[i];
    j++;
  }
  tempString[j] = '\0';

  solveArithmetic(tempString);

  int m = 0;
  char substitute[INPUT_LEN];
  for (int l = 0; l < startingIndex; l++) {
    substitute[m] = str[l];
    m++;
  }
  for (int l = 0; tempString[l] != '\0'; l++) {
    substitute[m] = tempString[l];
    m++;
  }
  for (int l = endingIndex+1; str[l] != '\0'; l++) {
    substitute[m] = str[l];
    m++;
  }
  substitute[m] = '\0';
  strcpy(str, substitute);
}

int containsParentheses(char* str) {
  int parCheck1 = indexOf(str, ")");
  int parCheck2 = indexOf(str, "(");
  if (parCheck1 != -1 && parCheck2 != -1) {
    return 1;
  } else {
    return 0;
  }
}

void dollarSignDoublePar(char* str) { //Replaces expression in $(( )) with the solution to that expression
  int expCheck = indexOf(str, "$(("); //Check for expression in parentheses
  if (expCheck != -1) {
    int expCheck2 = indexOf(str, "))");
    if (expCheck2 != -1) {
      int j = 0;
      char cmdOld[INPUT_LEN] = "";
      strcpy(cmdOld, str);
      char exp[INPUT_LEN] = "";
      exp[INPUT_LEN-1] = '\0';

      for (int i = expCheck + 2; i <= expCheck2; i++) { //Copy text in parentheses to exp
        exp[j] = str[i];
        j++;
      }
      exp[j] = '\0';

      variableSubstitute(exp);

      while (containsParentheses(exp) == 1) {
        solveParentheses(exp);
      }
      int alphanumeric = 1;
      for (int i = 0; exp[i] != '\0'; i++) {
        if ((exp[i] < '0' || exp[i] > '9') && exp[i] != '.') {
          alphanumeric = 0;
        }
      }
      if (alphanumeric) {
        int expLength; //Replace the expression in str with exp
        for (expLength = 0; exp[expLength] != '\0'; expLength++) {}
        int j = expCheck;
        for (int i = 0; i < expLength; i++) {
          str[j] = exp[i];
          j++;
        }
        for (int i = expCheck2+2; cmdOld[i] != '\0'; i++) {
          str[j] = cmdOld[i];
          j++;
        }
        str[j] = '\0';
      } else {
        Serial.println("Invalid expression");
      }
    } else {
      Serial.println(F("Error: expected '))'"));
      return;
    }
  }
}

void wrapText(char* str) {
  int count = 0;
  int prevSpace = 0;
  int lineCount = 1;
  lineStarts[0] = 0;
  int i;

  for (i = 0; str[i] != '\0'; i++) {
    if (str[i] == ' ') {
      prevSpace = i;
    }
    if (count >= COLS && count - prevSpace < COLS && prevSpace != 0) {
      str[prevSpace] = 0x0D; //A special character is used for newlines in wrapText, so that newlines typed in the text by the user can be preserved
      lineStarts[lineCount] = prevSpace + 1;
      lineCount++;
      i = prevSpace;
      count = -1;
    }
    if (str[i] == '\n') {
      lineStarts[lineCount] = i + 1;
      lineCount++;
      count = -1;
    }
    count++;
  }
  str[i] = '\0';
  lineStarts[lineCount] = i+1;
  lineStarts[lineCount+1] = -1;
  if (lineCount < ROWS) {
    screenLine = lineCount;
  } else {
    screenLine = ROWS;
  }
}

void unwrapText(char* str) {
  int i;

  for (i = 0; str[i] != '\0'; i++) {
    if (str[i] == 0x0D) {
      str[i] = ' ';
    }
  }
  str[i] = '\0';
}

void printSection(char* str, int start, int end) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(0, 0);
  for (int i = start; i < end; i++) {
    if (str[i] == 0x0D) { //A special character is used for newlines in wrapText, so that newlines typed in the text by the user can be preserved
      display.println("");
    } else {
      display.print(str[i]);
    }
  }
  display.display();
}

void keyboardEdit(char* str, char* saveFilePath) {
  int printing;
  uint16_t ps2;
  char c;
  int highlighting = 0;
  int highlightEnd;

  int currentLen;
  for (currentLen = 0; str[currentLen] != '\0'; currentLen++) {}
  int cursor;
  if (persistentCursor > currentLen) {
    cursor = 0;
  } else {
    cursor = persistentCursor;
  }

  insInString(str,  '|', cursor);
  wrapText(str);
  printSection(str, lineStarts[0], lineStarts[0+screenLine]-1);
  unwrapText(str);
  backspaceChar(str, cursor);

  while (true) {
    if (keyboard.available()) {
      // read the next key
      ps2 = keyboard.read();
      c = codesToAscii(ps2);
      printing = 1;
    
      if (c != 0) {
        if (highlighting == 1) {
          saveState = 0;
          deleteSeg(str, highlightEnd, cursor, c);
          currentLen = currentLen + 1 - abs(highlightEnd - cursor);
          if (cursor > highlightEnd) {
            cursor = highlightEnd;
          }
          cursor++;
          highlighting = 0;
        } else {
          if (currentLen+1 <= MAX_LEN-1) {
            saveState = 0;
            insInString(str, c, cursor);
            currentLen++;
            cursor++;
          } else {
            Serial.println("Buffer full");
            printing = 0;
          }
        }
      } else if (ps2 == 0x115) { //Left arrow
        if (highlighting == 1) {
          if (highlightEnd <= cursor) {
            cursor = highlightEnd;
          }
          highlighting = 0;
        } else if (cursor > 0) {
          cursor--;
        }
      } else if (ps2 == 0x116) { //Right arrow
        if (highlighting == 1) {
          if (highlightEnd > cursor) {
            cursor = highlightEnd;
          }
          highlighting = 0;
        } else if (cursor < currentLen) {
          cursor++;
        }
      } else if (ps2 == 0x117) { //Up arrow
        if (absoluteLine(cursor) > 0) {
          int spacing = lineStarts[absoluteLine(cursor)-1] + cursor - lineStarts[absoluteLine(cursor)];
          cursor = spacing;
        }
      } else if (ps2 == 0x118) { //Down arrow
        int spacing = lineStarts[absoluteLine(cursor)+1] + cursor - lineStarts[absoluteLine(cursor)];
        if (spacing <= currentLen) {
          cursor = spacing-1;
        }
      } else if (ps2 == 0x11C) { //Backspace
        saveState = 0;
        if (highlighting == 1) {
          deleteSeg(str, highlightEnd, cursor, 0);
          currentLen = currentLen - abs(highlightEnd - cursor);
          if (cursor > highlightEnd) {
            cursor = highlightEnd;
          }
          highlighting = 0;
        } else if (cursor > 0) {
          backspaceChar(str, cursor-1);
          cursor--;
          currentLen--;
        }
      } else if (ps2 == 0x2115) { //Ctrl + left arrow
        if (highlighting == 0) {
          highlightEnd = cursor;
        }
        if (cursor > 0) {
          cursor--;
        }
        highlighting = 1;
      } else if (ps2 == 0x2116) { //Ctrl + right arrow
        if (highlighting == 0) {
          highlightEnd = cursor;
        }
        if (cursor < currentLen) {
          cursor++;
        }
        highlighting = 1;
      } else if (ps2 == 0x2041) { //Ctrl + a
        highlightEnd = 0;
        cursor = currentLen;
        highlighting = 1;
      } else if ((ps2 == 0x2043) && highlighting) { //Ctrl + c
        copySeg(clipboard, highlightEnd, cursor, str);
        clipboardLen = abs(highlightEnd - cursor);
        printing = 0;
      } else if ((ps2 == 0x2058) && highlighting) { //Ctrl + x
        saveState = 0;
        copySeg(clipboard, highlightEnd, cursor, str);
        clipboardLen = abs(highlightEnd - cursor);
        deleteSeg(str, highlightEnd, cursor, 0);
        currentLen = currentLen - abs(highlightEnd - cursor);
        if (cursor > highlightEnd) {
          cursor = highlightEnd;
        }
        highlighting = 0;
      } else if (ps2 == 0x2056) { //Ctrl + v
        if (highlighting == 1) {
          deleteSeg(str, highlightEnd, cursor, 0);
          currentLen = currentLen - abs(highlightEnd - cursor);
          if (cursor > highlightEnd) {
            cursor = highlightEnd;
          }
          highlighting = 0;
        }

        if (currentLen+clipboardLen <= MAX_LEN-1) {
          saveState = 0;
          for (int i = 0; i < clipboardLen; i++) {
            insInString(str, clipboard[i], cursor);
            currentLen++;
            cursor++;
          }
        } else {
          Serial.println("Buffer full");
          printing = 0;
        }
      } else if (ps2 == 0x2053) { //Ctrl + s
        if (saveState == 0) {
          saveState = 1;
          SD.remove(saveFilePath);
          File myFile = SD.open(saveFilePath, FILE_WRITE);
          if (myFile) {
            myFile.println(str);
            myFile.close();
          } else {
            Serial.println("error opening file");
          }
        }
      } else if (ps2 == 0x011E) { //Enter
        if (highlighting == 1) {
          deleteSeg(str, highlightEnd, cursor, '\n');
          currentLen = currentLen + 1 - abs(highlightEnd - cursor);
          if (cursor > highlightEnd) {
            cursor = highlightEnd;
          }
          cursor++;
          highlighting = 0;
        } else {
          if (currentLen+1 <= MAX_LEN-1) {
            saveState = 0;
            insInString(str, '\n', cursor);
            currentLen++;
            cursor++;
          } else {
            Serial.println("Buffer full");
            printing = 0;
          }
        }
      } else if (ps2 == 0x0964) { //Alt+F4
        if (saveState == 0) {
          Serial.println("unsaved");
        }
        persistentCursor = cursor;
        return;
      } else {
        printing = 0;
      }

      if (printing) {
        if (highlighting) {
          if (cursor > highlightEnd) { 
            highlightString(str, highlightEnd, cursor+1);
            wrapText(str);
            if (absoluteLine(cursor)-ROWS+1 > apparentLine) {
              apparentLine++;
            } else if (absoluteLine(cursor) < apparentLine) {
              apparentLine--;
            }
            printSection(str, lineStarts[apparentLine], lineStarts[apparentLine+screenLine]-1);
            unwrapText(str);
            backspaceChar(str, cursor+1);
            backspaceChar(str, highlightEnd);
          } else {
            highlightString(str, highlightEnd+1, cursor);
            wrapText(str);
            if (absoluteLine(highlightEnd)-ROWS+1 > apparentLine) {
              apparentLine++;
            } else if (absoluteLine(highlightEnd) < apparentLine) {
              apparentLine--;
            }
            printSection(str, lineStarts[apparentLine], lineStarts[apparentLine+screenLine]-1);
            unwrapText(str);
            backspaceChar(str, highlightEnd+1);
            backspaceChar(str, cursor);
          }
        } else {
          insInString(str, '|', cursor);
          wrapText(str);
          if (absoluteLine(cursor)-ROWS+1 > apparentLine) {
            apparentLine++;
          } else if (absoluteLine(cursor) < apparentLine) {
            apparentLine--;
          }
          printSection(str, lineStarts[apparentLine], lineStarts[apparentLine+screenLine]-1);
          unwrapText(str);
          backspaceChar(str, cursor);
        }
      }
    }
  }
}

void insInString(char* str, char charIn, int pos) {
  char nextChar = charIn;
  char overwrittenChar;
  int i;

  for (i = pos; str[i] != '\0'; i++) {
    overwrittenChar = str[i];
    str[i] = nextChar;
    nextChar = overwrittenChar;
  }
  str[i] = nextChar;
  str[i+1] = '\0';
}

char codesToAscii(uint16_t in) {
  char out;

  uint16_t status = in >> 8;
  out = in & 0xFF;

  if ((status & 0x80) == 0x80) { //Prevents from returning break key signals
    return 0;
  }

  //Serial.println(" ");
  //Serial.print("Status Bits: ");
  //Serial.println(status, HEX);
  //Serial.print("Code: ");
  //Serial.println(out, HEX);

  if ((status & 0x20) == 0x20) { //Ctrl key
    return 0;
  }

  if ((status & 0x08) == 0x08) { //Alt key
    return 0;
  }

  if (in == 0x11F) {
    return ' ';
  } else if (in == 0x3B) {
    return ',';
  } else if (in == 0x3D) {
    return '.';
  } else if (in == 0x4031) {
    return '!';
  } else if (in == 0x4032) {
    return '@';
  } else if (in == 0x4033) {
    return '#';
  } else if (in == 0x4034) {
    return '$';
  } else if (in == 0x4035) {
    return '%';
  } else if (in == 0x4036) {
    return '^';
  } else if (in == 0x4037) {
    return '&';
  } else if (in == 0x4038) {
    return '*';
  } else if (in == 0x4039) {
    return '(';
  } else if (in == 0x4030) {
    return ')';
  } else if (in == 0x3E) {
    return '/';
  } else if (in == 0x403E) {
    return '?';
  } else if (in == 0x5B) {
    return ';';
  } else if (in == 0x405B) {
    return ':';
  } else if (in == 0x3A) {
    return 0x27;
  } else if (in == 0x403A) {
    return 0x22;
  } else if (in == 0x405D) {
    return '{';
  } else if (in == 0x405E) {
    return '}';
  } else if (in == 0x3C) {
    return '-';
  } else if (in == 0x403C) {
    return '_';
  } else if (in == 0x5F) {
    return '=';
  } else if (in == 0x405F) {
    return '+';
  } else if (in == 0x5C) {
    return 0x5C;
  }

  if (!(out > 31 && out < 128)) { //Except for cases above, only returns alphanumeric characters
    return 0;
  }

  if (((status & 0x40) != 0x40) && (out >= 65 && out <= 90)) {
    out = out + ('a' - 'A');
  }

  return out;
}

void deleteSeg(char* str, int startPos, int endPos, char in) {
  int i;
  int greater;
  int width = abs(startPos - endPos);

  if (startPos > endPos) {
    greater = startPos;
  } else {
    greater = endPos;
  }

  if (in != 0) {
    str[greater-width] = in;
    width--;
  }

  for (i = greater; str[i] != '\0'; i++) {
    str[i-width] = str[i];
  }
  str[i-width] = '\0';
}

void backspaceChar(char* str, int pos) {
  char nextChar;

  for (int i = pos; str[i] != '\0'; i++) {
    nextChar = str[i+1];
    str[i] = nextChar;
  }
}

void copySeg(char* strOut, int startPos, int endPos, char* strIn) {
  int j = 0;
  int leftPos;
  int rightPos;

  if (startPos > endPos) {
    leftPos = endPos;
    rightPos = startPos;
  } else {
    rightPos = endPos;
    leftPos = startPos;
  }

  for (int i = leftPos; i < rightPos; i++) {
    strOut[j] = strIn[i];
    j++;
  }
  strOut[j] = '\0';
}

void highlightString(char* str, int startPos, int endPos) {
  int leftPos;
  int rightPos;

  if (startPos > endPos) {
    leftPos = endPos;
    rightPos = startPos;
  } else {
    rightPos = endPos;
    leftPos = startPos;
  }

  insInString(str, '[', leftPos);
  insInString(str, ']', rightPos);
}

int absoluteLine(int strPoint) {
  int i;
  for (i = 0; lineStarts[i] <= strPoint; i++) {}
  return i-1;
}

void setup() {
  Serial.begin(115200);
  display.begin(0, true);
  display.display();
  delay(2000);
  display.clearDisplay();
  display.display();
  keyboard.begin(DataPin, IRQpin);

  if (!SD.begin(chipSelect)) {
    Serial.println("SD card initialization failed");
    while (true);
  }

  Serial.println(F("\n--- SDLookingGlass v1.0 ---"));
  Serial.println(F("Type 'help' for commands"));
  printPrompt();
}

void loop() {
  if (Serial.available() > 0) {
    char c = Serial.read();
    if (c == '\r' || c == '\n') { //Enter
      if (inputLen > 0) {
        inputBuffer[inputLen] = '\0';
        Serial.println();
        executeCommand(inputBuffer);
        inputLen = 0;
        memset(inputBuffer, 0, INPUT_LEN);
        printPrompt();
      } else {
        
        Serial.println();
        printPrompt();
      }
    }
    else if (c == 8 || c == 127) { //Backspace
      if (inputLen > 0) {
        inputLen--;
        inputBuffer[inputLen] = '\0';
        Serial.print(F("\b \b"));
      }
    }
    else if (inputLen < INPUT_LEN-1) {
      Serial.print(c);
      inputBuffer[inputLen] = c;
      inputLen++;
    }
  }
}

void runScript(const char* content);

void executeCommand(char* line) {
  char cmd[INPUT_LEN] = "";
  char args[INPUT_LEN] = "";

  strcpy(cmd, line);

  dollarSignDoublePar(cmd);

  for (int i = 0; cmd[i] != '\0'; i++) { //Break up the input into command (everything before first space) and arguments
    if (cmd[i] == ' ') {
      strcpy(args, cmd + i + 1);
      args[INPUT_LEN-1] = '\0';
      cmd[i] = '\0';
      break;
    }
  }

  toLowercase(cmd);

  if (strcmp_P(cmd, PSTR("ls")) == 0) {
    int empty = 1;

    File lsDir = SD.open(currentPath);

    while (true) {
      File lsEntry =  lsDir.openNextFile();
      if (! lsEntry) {
        // no more files
        Serial.println(" ");
        break;
      }
      empty = 0;

      Serial.print(lsEntry.name());

      if (lsEntry.isDirectory()) {
        Serial.print("/   ");
      } else {
        Serial.print("   ");
      }
      lsEntry.close();
    }

    if (empty) Serial.println(F("(empty)"));
    lsDir.close();
  }
  else if (strcmp_P(cmd, PSTR("mkdir")) == 0) {
    char dirPath[NAME_LEN + PATH_LEN];

    if (strcmp_P(currentPath, PSTR("/")) == 0) {
      strcpy(dirPath, args);
    } else {
      strcpy(dirPath, currentPath);
      strcat(dirPath, "/");
      strcat(dirPath, args);
    }

    SD.mkdir(dirPath);
  }
  else if (strcmp_P(cmd, PSTR("touch")) == 0) {
    char filePath[NAME_LEN + PATH_LEN];
    strcpy(filePath, currentPath);
    strcat(filePath, "/");
    strcat(filePath, args);
    
    File myFile = SD.open(filePath, FILE_WRITE);
    myFile.close();
  }
  else if (strcmp_P(cmd, PSTR("cd")) == 0) {
    if (strcmp_P(args, PSTR("..")) == 0 || strcmp_P(args, PSTR("/")) == 0) { //Move back to top directory
      strcpy(currentPath, "/");
      currentPath[PATH_LEN - 1] = '\0';
    } else {
      int found = 0;

      File dir = SD.open(currentPath);

      while (true) {
        File entry =  dir.openNextFile();
        if (! entry) {
          // no more files
          Serial.println("");
          break;
        }

        if (entry.isDirectory() && strcmp(args, entry.name()) == 0) {
          found = 1;
        }
        entry.close();
      }

      if (found) {
        if (!safeConcatPath(currentPath, args)) {
          strcpy(currentPath, "/");
          currentPath[PATH_LEN - 1] = '\0';
          Serial.println(F("Path too long."));
          return;
        }
      } else {
        Serial.println(F("No dir."));
      }
    }
  }
  else if (strcmp_P(cmd, PSTR("pwd")) == 0) {
    Serial.println(currentPath);
  }
  else if (strcmp_P(cmd, PSTR("echo")) == 0) {
    variableSubstitute(args);

    int arrow = indexOf(args, " > ");
    if (arrow != -1) { //Option to use > to send string to file
      char text[40] = "";
      strncpy(text, args, arrow);
      text[arrow] = '\0';
      char filename[12] = "";
      strcpy(filename, args + arrow + 3);
      filename[NAME_LEN - 1] = '\0';
      char newfilepath[PATH_LEN] = "";
      strcpy(newfilepath, currentPath);

      strcat(newfilepath, filename);

      File dataFile = SD.open(newfilepath, FILE_WRITE);

      if (dataFile) {
        dataFile.println(text);
        dataFile.close();
        Serial.println(text);
      } else {
        Serial.println(F("error opening file"));
      }
    } else {
      Serial.println(args);
    }
  }
  else if (strcmp_P(cmd, PSTR("cat")) == 0) {
    char newfilepath[PATH_LEN] = "";
    strcpy(newfilepath, currentPath);
    strcat(newfilepath, args);

    File myFile = SD.open(newfilepath);
    if (myFile) {
      while (myFile.available()) {
        Serial.write(myFile.read());
      }
      myFile.close();
    } else {
      Serial.println("error opening file");
    }
  }
  else if (strcmp_P(cmd, PSTR("info")) == 0) {
    File myFile = SD.open(args);
    if (myFile) {
      Serial.print(F("Name: ")); Serial.println(myFile.name());
      Serial.print(F("Type: ")); Serial.println(myFile.isDirectory() ? F("Directory") : F("File"));
      Serial.print(F("Size: ")); Serial.print(myFile.size()); Serial.println(F(" bytes"));
    } else {
      Serial.println(F("Not found."));
    }
    myFile.close();
  }
  else if (strcmp_P(cmd, PSTR("rm")) == 0) {
    char filePath[NAME_LEN + PATH_LEN];
    if (strcmp_P(currentPath, PSTR("/")) == 0) {
      strcpy(filePath, args);
    } else {
      strcpy(filePath, currentPath);
      strcat(filePath, "/");
      strcat(filePath, args);
    }
    if (SD.exists(filePath)) {
      SD.remove(filePath);
      SD.rmdir(filePath);
      Serial.println(F("Removed."));
    } else {
      Serial.println(F("Not found."));
    }
  }
  else if (strcmp_P(cmd, PSTR("dmesg")) == 0) {
    Serial.println(F("=== KERNEL MESSAGES ==="));
    int j;
    for (j = 0; j < DMESG_LINES; j++) {
      if (dmesg[j].message[0] != '\0') {
        Serial.print(F("["));
        Serial.print(dmesg[j].timestamp);
        Serial.print(F("] "));
        Serial.println(dmesg[j].message);
      }
    }
  }
  else if (strcmp_P(cmd, PSTR("uptime")) == 0) {
    unsigned long s = millis() / 1000;
    unsigned long h = s / 3600;
    unsigned long m = (s % 3600) / 60;
    unsigned long sec = s % 60;
    Serial.print(F("up "));
    Serial.print(h); Serial.print(F("h "));
    Serial.print(m); Serial.print(F("m "));
    Serial.print(sec); Serial.println(F("s"));
    addDmesg(F("uptime command"));
  }
  else if (strcmp_P(cmd, PSTR("df")) == 0 || strcmp_P(cmd, PSTR("free")) == 0) {
    Serial.print(F("Free Disk Space: "));
    Serial.print(freeMemory());
    Serial.println(F(" GB"));
  }
  else if (strcmp_P(cmd, PSTR("whoami")) == 0) {
    Serial.println(F("root"));
  }
  else if (strcmp_P(cmd, PSTR("uname")) == 0) {
    Serial.println(F("SDLookingGlass v1.0"));
    Serial.print(F("Kernel: Arduino "));
    Serial.println(F("AVR"));
    Serial.print(F("Hardware: "));
    Serial.println(F("Teensy 4.1"));
    Serial.print(F("Disk Space: "));
    Serial.print(freeMemory());
    Serial.println(F(" GB free"));
  }
  else if (strcmp_P(cmd, PSTR("reboot")) == 0) {
    Serial.println(F("Rebooting..."));
    addDmesg(F("System reboot"));
    delay(500);
    resetFunc();
  }
  else if (strcmp_P(cmd, PSTR("clear")) == 0) {
    int j;
    for (j = 0; j < 30; j++) Serial.println();
  }
  else if (strcmp_P(cmd, PSTR("sh")) == 0) {
    if (args[0] == '\0') {
      Serial.println(F("Usage: sh [script]"));
      return;
    }

    char script[INPUT_LEN];

    File myFile = SD.open(args);
    if (myFile) {
      addDmesg(F("sh: running script"));
      int j = 0;
      while (myFile.available()) {
        script[j] = myFile.read();
        j++;
      }
      myFile.close();
      script[INPUT_LEN-1] = '\0';
      runScript(script);
    } else {
      Serial.println(F("Script not found."));
    }
  }
  else if (strcmp_P(cmd, PSTR("alias")) == 0) {
    if (args[0] == '\0') {
      int j, any = 0;
      for (j = 0; j < MAX_ALIASES; j++) {
        if (aliases[j].active) {
          Serial.print(aliases[j].name);
          Serial.print(F("='"));
          Serial.print(aliases[j].value);
          Serial.println(F("'"));
          any = 1;
        }
      }
      if (!any) Serial.println(F("No aliases."));
    } else {
      int eq = indexOf(args, "=");
      if (eq == -1) {
        // show single alias
        int j, found = 0;
        for (j = 0; j < MAX_ALIASES; j++) {
          if (aliases[j].active && strcmp(aliases[j].name, args) == 0) {
            Serial.print(args); Serial.print(F("='")); Serial.print(aliases[j].value); Serial.println(F("'"));
            found = 1; break;
          }
        }
        if (!found) Serial.println(F("No such alias."));
      } else {
        char aname[ALIAS_NAME_LEN] = "";
        char aval[ALIAS_VAL_LEN] = "";
        strncpy(aname, args, eq < ALIAS_NAME_LEN ? eq : ALIAS_NAME_LEN - 1); //Set aname to the one given in the argument, before the equals sign
        aname[ALIAS_NAME_LEN - 1] = '\0';
        strncpy(aval, args + eq + 1, ALIAS_VAL_LEN - 1); //Set aval to the one given in the argument, after the equals sign
        aval[ALIAS_VAL_LEN - 1] = '\0';
        int j, slot = -1;
        for (j = 0; j < MAX_ALIASES; j++) {
          if (aliases[j].active && strcmp(aliases[j].name, aname) == 0) { slot = j; break; }
        }
        if (slot == -1) {
          for (j = 0; j < MAX_ALIASES; j++) {
            if (!aliases[j].active) { slot = j; break; }
          }
        }
        if (slot == -1) { Serial.println(F("Alias table full.")); return; }
        strncpy(aliases[slot].name, aname, ALIAS_NAME_LEN);
        aliases[slot].name[ALIAS_NAME_LEN - 1] = '\0';
        strncpy(aliases[slot].value, aval, ALIAS_VAL_LEN);
        aliases[slot].value[ALIAS_VAL_LEN - 1] = '\0';
        aliases[slot].active = 1;
        Serial.println(F("Alias set."));
      }
    }
  }
  else if (strcmp_P(cmd, PSTR("help")) == 0) {
    Serial.println(F(" "));
    Serial.println(F("ls - List files"));
    Serial.println(F("cd - Change directory"));
    Serial.println(F("pwd - Print working directory"));
    Serial.println(F("mkdir - Make directory"));
    Serial.println(F("touch - Create file"));
    Serial.println(F("cat - Read file text"));
    Serial.println(F("echo - Echo text to terminal"));
    Serial.println(F("echo [text] > [file]  -- Add text to file"));
    Serial.println(F("rm - Remove file or directory"));
    Serial.println(F("info - File info"));
    Serial.println(F("sh - Run shell script"));
    Serial.println(F("sh [file]  -- run script (use ; as line separator)"));
    Serial.println(F("uptime - Show uptime"));
    Serial.println(F("uname - Show device info"));
    Serial.println(F("dmesg - Show kernel messages"));
    Serial.println(F("df, free - Show free disk space"));
    Serial.println(F("whoami - Show username"));
    Serial.println(F("clear - Clear terminal"));
    Serial.println(F("reboot - Reboot device"));
    Serial.println(F("alias - Create command alias"));
    Serial.println(F("$(([expression]))  -- Solve a math expression"));
    Serial.println(F("[variable]=[number]  -- Set a variable"));
    Serial.println(F("unset - Unset a variable"));
    Serial.println(F(" "));
  }
  else if (strcmp_P(cmd, PSTR("unset")) == 0) {
    int found = 0;

    for (int i = 0; i < varIndex; i++) {
      if (strcmp_P(vars[i].name, args) == 0) {
        found = 1;
      }
      if (found) {
        strcpy(vars[i].name, vars[i+1].name);
        vars[i].value = vars[i+1].value;
      }
    }
    if (found) {
      varIndex--;
    } else {
      Serial.println(F("Variable not found"));
    }
  }
  else if (strcmp_P(cmd, PSTR("notepad")) == 0) {
    char newfilepath[PATH_LEN] = "";
    strcpy(newfilepath, currentPath);
    strcat(newfilepath, args);

    File myFile = SD.open(newfilepath);
    int i = 0;
    if (myFile) {
      while (myFile.available() && i < MAX_LEN) {
        fileBuffer[i] = myFile.read();
        i++;
      }
      myFile.close();
      fileBuffer[i] = '\0';
      keyboardEdit(fileBuffer, newfilepath);
      wrapText(fileBuffer);
      printSection(fileBuffer, lineStarts[0], lineStarts[0+screenLine]-1);
      unwrapText(fileBuffer);
      display.clearDisplay();
      display.display();
    } else {
      Serial.println("error opening file");
    }
  } else {
    int j, resolved = 0; //Check alias
    for (j = 0; j < MAX_ALIASES; j++) {
      if (aliases[j].active && strcmp(aliases[j].name, cmd) == 0) {
        char aliasLine[INPUT_LEN] = "";
        strcpy(aliasLine, aliases[j].value);
        aliasLine[INPUT_LEN-1] = '\0';
        if (args[0] != '\0') {
          int al = strlen(aliasLine);
          if (al < (INPUT_LEN-2)) { aliasLine[al] = ' '; aliasLine[al+1] = '\0'; }
          strncat(aliasLine, args, (INPUT_LEN-1) - strlen(aliasLine));
        }
        executeCommand(aliasLine);
        resolved = 1;
        break;
      }
    }

    int eqCheck = indexOf(cmd, "="); //Check for variables being set
    if (eqCheck != -1) {
      resolved = 1;
      char varName[INPUT_LEN] = "";
      strncpy(varName, cmd, eqCheck);

      char varVal[INPUT_LEN] = "";
      int j = 0;
      for (int i = eqCheck + 1; i < 31; i++) {
        varVal[j] = cmd[i];
        j++;
        if (cmd[i] == '\0') {
          break;
        }
      }
      float varTotal = decimalCharToFloat(varVal);

      int existing = 0;
      for (int i = 0; i < varIndex; i++) {
        if (strcmp(vars[i].name, varName) == 0) {
          vars[i].value = varTotal;
          existing = 1;
        }
      }
      if (!existing) {
        if (varIndex < VAR_SPACES) {
          strcpy(vars[varIndex].name, varName);
          vars[varIndex].value = varTotal;
          varIndex++;
        } else {
          Serial.println(F("No space available for variables"));
        }
      }

      /*Serial.println(F("All current vars:"));
      for (int i = 0; i < varIndex; i++) {
        Serial.print(vars[i].name);
        Serial.print(": ");
        Serial.println(vars[i].value);
      }*/
    }
    if (!resolved) Serial.println(F("Unknown command."));
  }
}

// Interpreter sh
void runScript(const char* content) {
  char line[INPUT_LEN];
  int ci = 0, li = 0, lineNum = 0;
  int len = strlen(content);

  while (ci <= len) {
    char c = (ci < len) ? content[ci] : ';';
    ci++;
    if (c == ';' || c == '\n' || c == '\r') {
      if (li > 0) {
        line[li] = '\0';
        lineNum++;
        Serial.print(F("[sh:")); Serial.print(lineNum); Serial.print(F("] "));
        Serial.println(line);
        executeCommand(line);
        li = 0;
      }
    } else {
      if (li < INPUT_LEN-1) line[li++] = c;
    }
  }
  addDmesg(F("sh: script done"));
  Serial.println(F("[sh] done."));
}
