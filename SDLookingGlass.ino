#include <SD.h>
#include <Adafruit_SH110X.h>
#include <PS2KeyAdvanced.h>

const int internalSD = BUILTIN_SDCARD;

#define INPUT_LEN 32
char inputBuffer[INPUT_LEN] = "";
int inputLen = 0;

#define NAME_LEN 24

#define PATH_LEN 64
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

#define OLED_DC       5
#define OLED_CS       6
#define OLED_RST      4
#define OLED_MOSI     3
#define OLED_SCK      2
Adafruit_SH1106G display = Adafruit_SH1106G(128, 64, OLED_MOSI, OLED_SCK, OLED_DC, OLED_RST, OLED_CS); //On Teensy 4.1: MOSI=11, MISO=12, SCK=13
#define COLS 21
#define ROWS 8

#define DataPin 18
#define IRQpin 19
PS2KeyAdvanced keyboard;
#define MAX_LEN 100000 //There are usually around 100,000 bytes free in RAM1 for this but don't push it
char fileBuffer[MAX_LEN];
const int linesEstimation = floor(MAX_LEN/3);
int lineStarts[linesEstimation];
int persistentCursor = 0;
char clipboard[MAX_LEN];
int clipboardLen = 0;
int topLineOfScreen = 0;
int onscreenLineCount;
int saveState = 1;

#define OUTPUT_LEN 720
char outputBuffer[OUTPUT_LEN] = "SDLookingGlass v1.0\nType 'help' for commands\n";
char promptBuffer[OUTPUT_LEN + 25 + PATH_LEN];
const int outputLinesEstimation = floor(OUTPUT_LEN/3);
int outputLineStarts[outputLinesEstimation];

const int removableSD = 40;
SDClass SD1;
char mountPointPath[PATH_LEN];
int removableSDMounted = 0;
char removablePath[PATH_LEN];
int mountPointLen = 0;

float freeMemory(int SDPin) {
  Sd2Card card;
  SdVolume volume;

  if (!card.init(SPI_HALF_SPEED, SDPin)) {
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

void toLowercase(char* str) {
  int i;
  for (i = 0; str[i] != '\0'; i++) {
    if (str[i] >= 'A' && str[i] <= 'Z') str[i] = str[i] - 'A' + 'a';
  }
}

int indexOf(const char* str, const char* substr, int direction) { //Find the point in a string where a substring is
  int i, j, slen = strlen(str), sublen = strlen(substr);
  if (direction == 0) { //Scan the string from the beginning
    for (i = 0; i <= slen - sublen; i++) {
      int match = 1;
      for (j = 0; j < sublen; j++) {
        if (str[i + j] != substr[j]) { match = 0; break; }
      }
      if (match) return i;
    }
  } else if (direction == 1) { //Scan the string from the end
    for (i = slen - sublen; i >= 0; i--) {
      int match = 1;
      for (j = 0; j < sublen; j++) {
        if (str[i + j] != substr[j]) { match = 0; break; }
      }
      if (match) return i;
    }
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
  int negativeSign = 0;
  if (str[0] == '-') {
    negativeSign = 1;
  }
  for (int i = negativeSign; str[i] != '\0'; i++) {
    if (str[i] == '.') {
      pos = 1;
    } else if ((str[i] - '0') >= 0 && (str[i] - '0') <= 9) {
      if (pos) {
        afterDec++;
      } else {
        beforeDec++;
      }
    } else if (str[i] == '-' && negativeSign) {

    } else {
      strcat(outputBuffer, "Invalid number\n");
      return 0;
    }
  }
  beforeDec = beforeDec + negativeSign;
  for (int i = negativeSign; i < beforeDec; i++) {
    total = total + (str[i] - '0')*pow(10, (beforeDec-i-1));
  }
  for (int i = 1; i <= afterDec; i++) {
    total = total + (str[beforeDec + i] - '0')*pow(10, -i);
  }
  if (negativeSign) {
    total = -total;
  }
  return total;
}

void variableSubstitute(char* str) { //Replaces variable name with the value of that variable
  for (int i = 0; i < varIndex; i++) { //Check for variables in entry
    int varCheck = indexOf(str, vars[i].name, 0);
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

void solveArithmetic(char* str, int outerFunction) {
  char tempFloat[8];
  int termsIndex = 0;
  float result = 0;

  float flTerms[16];
  int flOps[16];
  int flTermsIndex = 0;
  int flOpsIndex = 0;
  int startNumber = 1;

  for (int i = 0; str[i] != '\0'; i++) {
    if (str[i] == '-' && startNumber) {
      tempFloat[termsIndex] = str[i];
      termsIndex++;
      startNumber = 0;
    } else if (str[i] == '+' || str[i] == '-' || str[i] == '*' || str[i] == '/' || str[i] == '^') {
      tempFloat[termsIndex] = '\0';
      termsIndex = 0;
      flTerms[flTermsIndex] = decimalCharToFloat(tempFloat);
      flTermsIndex++;

      if (str[i] == '+') {
        flOps[flOpsIndex] = 1;
      } else if (str[i] == '-') {
        flOps[flOpsIndex] = 2;
      } else if (str[i] == '*') {
        flOps[flOpsIndex] = 3;
      } else if (str[i] == '/') {
        flOps[flOpsIndex] = 4;
      } else if (str[i] == '^') {
        flOps[flOpsIndex] = 5;
      }
      flOpsIndex++;
      startNumber = 1;
    } else {
      tempFloat[termsIndex] = str[i];
      termsIndex++;
      startNumber = 0;
    }
  }
  tempFloat[termsIndex] = '\0';
  flTerms[flTermsIndex] = decimalCharToFloat(tempFloat);

  //flTermsIndex++;
  //for (int q = 0; q < flTermsIndex; q++) {
  //  Serial.print(flTerms[q]);
  //  Serial.print(" ");
  //  Serial.print(flOps[q]);
  //  Serial.print(" ");
  //}
  //Serial.println(flTerms[flTermsIndex]);

  for (int q = 0; q < flTermsIndex; q++) { //Exponents
    if (flOps[q] == 5 || flOps[q] == 0) {
      int offset = 0;
      while (flOps[q+offset] == 5 || flOps[q+offset] == 0) {
        if (flOps[q+offset] == 5) {
          flTerms[q] = pow(flTerms[q], flTerms[q+offset+1]);
          flOps[q+offset] = 0;
        }
        if (q+offset+1 == flTermsIndex) {
          break;
        }
        offset++;
      }
      q = q + offset;
    }
  }

  for (int q = 0; q < flTermsIndex; q++) { //Multiplication
    if (flOps[q] == 3 || flOps[q] == 0) {
      int offset = 0;
      while (flOps[q+offset] == 3 || flOps[q+offset] == 0) {
        if (flOps[q+offset] == 3) {
          flTerms[q] = flTerms[q] * flTerms[q+offset+1];
          flOps[q+offset] = 0;
        }
        if (q+offset+1 == flTermsIndex) {
          break;
        }
        offset++;
      }
      q = q + offset;
    }
  }

  for (int q = 0; q < flTermsIndex; q++) { //Division
    if (flOps[q] == 4 || flOps[q] == 0) {
      int offset = 0;
      while (flOps[q+offset] == 4 || flOps[q+offset] == 0) {
        if (flOps[q+offset] == 4) {
          flTerms[q] = flTerms[q] / flTerms[q+offset+1];
          flOps[q+offset] = 0;
        }
        if (q+offset+1 == flTermsIndex) {
          break;
        }
        offset++;
      }
      q = q + offset;
    }
  }

  for (int q = 0; q < flTermsIndex; q++) { //Addition
    if (flOps[q] == 1 || flOps[q] == 0) {
      int offset = 0;
      while (flOps[q+offset] == 1 || flOps[q+offset] == 0) {
        if (flOps[q+offset] == 1) {
          flTerms[q] = flTerms[q] + flTerms[q+offset+1];
          flOps[q+offset] = 0;
        }
        if (q+offset+1 == flTermsIndex) {
          break;
        }
        offset++;
      }
      q = q + offset;
    }
  }

  for (int q = 0; q < flTermsIndex; q++) { //Subtraction
    if (flOps[q] == 2 || flOps[q] == 0) {
      int offset = 0;
      while (flOps[q+offset] == 2 || flOps[q+offset] == 0) {
        if (flOps[q+offset] == 2) {
          flTerms[q] = flTerms[q] - flTerms[q+offset+1];
          flOps[q+offset] = 0;
        }
        if (q+offset+1 == flTermsIndex) {
          break;
        }
        offset++;
      }
      q = q + offset;
    }
  }

  //for (int q = 0; q < flTermsIndex; q++) {
  //  Serial.print(flTerms[q]);
  //  Serial.print(" ");
  //  Serial.print(flOps[q]);
  //  Serial.print(" ");
  //}
  //Serial.println(flTerms[flTermsIndex]);

  result = flTerms[0];

  if (outerFunction == 1) {
    result = log(result);
  } else if (outerFunction == 2) {
    result = abs(result);
  } else if (outerFunction == 3) {
    result = sin(result);
  } else if (outerFunction == 4) {
    result = cos(result);
  } else if (outerFunction == 5) {
    result = tan(result);
  } else if (outerFunction == 6) {
    result = log10(result);
  } else if (outerFunction == 7) {
    result = exp(result);
  } else if (outerFunction == 8) {
    result = sqrt(result);
  } else if (outerFunction == 9) {
    result = cbrt(result);
  } else if (outerFunction == 10) {
    result = asin(result);
  } else if (outerFunction == 11) {
    result = acos(result);
  } else if (outerFunction == 12) {
    result = atan(result);
  } else if (outerFunction == 13) {
    result = ceil(result);
  } else if (outerFunction == 14) {
    result = tgamma(result+1);
  } else if (outerFunction == 15) {
    result = round(result);
  } else if (outerFunction == 16) {
    result = floor(result);
  }

  if (fmod(result, 1) == 0.00) {
    int resultInt = result;
    sprintf(str, "%d", resultInt);
  } else {
    sprintf(str, "%f", result);
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
  //Serial.println(str);
  //Serial.println(startingIndex);
  //Serial.println(tempString);
  //Serial.println("");

  int outerFunction = 0;
  if (startingIndex > 5) {
    if (str[startingIndex-5] == 'r' && str[startingIndex-4] == 'o' && str[startingIndex-3] == 'u' && str[startingIndex-2] == 'n' && str[startingIndex-1] == 'd') {
      outerFunction = 15;
      startingIndex = startingIndex - 5;
    } else if (str[startingIndex-5] == 'f' && str[startingIndex-4] == 'l' && str[startingIndex-3] == 'o' && str[startingIndex-2] == 'o' && str[startingIndex-1] == 'r') {
      outerFunction = 16;
      startingIndex = startingIndex - 5;
    }
  } else if (startingIndex > 4) {
    if (str[startingIndex-4] == 's' && str[startingIndex-3] == 'q' && str[startingIndex-2] == 'r' && str[startingIndex-1] == 't') {
      outerFunction = 8;
      startingIndex = startingIndex - 4;
    } else if (str[startingIndex-4] == 'c' && str[startingIndex-3] == 'b' && str[startingIndex-2] == 'r' && str[startingIndex-1] == 't') {
      outerFunction = 9;
      startingIndex = startingIndex - 4;
    } else if (str[startingIndex-4] == 'a' && str[startingIndex-3] == 's' && str[startingIndex-2] == 'i' && str[startingIndex-1] == 'n') {
      outerFunction = 10;
      startingIndex = startingIndex - 4;
    } else if (str[startingIndex-4] == 'a' && str[startingIndex-3] == 'c' && str[startingIndex-2] == 'o' && str[startingIndex-1] == 's') {
      outerFunction = 11;
      startingIndex = startingIndex - 4;
    } else if (str[startingIndex-4] == 'a' && str[startingIndex-3] == 't' && str[startingIndex-2] == 'a' && str[startingIndex-1] == 'n') {
      outerFunction = 12;
      startingIndex = startingIndex - 4;
    } else if (str[startingIndex-4] == 'c' && str[startingIndex-3] == 'e' && str[startingIndex-2] == 'i' && str[startingIndex-1] == 'l') {
      outerFunction = 13;
      startingIndex = startingIndex - 4;
    } else if (str[startingIndex-4] == 'f' && str[startingIndex-3] == 'a' && str[startingIndex-2] == 'c' && str[startingIndex-1] == 't') {
      outerFunction = 14;
      startingIndex = startingIndex - 4;
    }
  } else if (startingIndex > 3) {
    if (str[startingIndex-3] == 'a' && str[startingIndex-2] == 'b' && str[startingIndex-1] == 's') {
      outerFunction = 2;
      startingIndex = startingIndex - 3;
    } else if (str[startingIndex-3] == 's' && str[startingIndex-2] == 'i' && str[startingIndex-1] == 'n') {
      outerFunction = 3;
      startingIndex = startingIndex - 3;
    } else if (str[startingIndex-3] == 'c' && str[startingIndex-2] == 'o' && str[startingIndex-1] == 's') {
      outerFunction = 4;
      startingIndex = startingIndex - 3;
    } else if (str[startingIndex-3] == 't' && str[startingIndex-2] == 'a' && str[startingIndex-1] == 'n') {
      outerFunction = 5;
      startingIndex = startingIndex - 3;
    } else if (str[startingIndex-3] == 'l' && str[startingIndex-2] == 'o' && str[startingIndex-1] == 'g') {
      outerFunction = 6;
      startingIndex = startingIndex - 3;
    } else if (str[startingIndex-3] == 'e' && str[startingIndex-2] == 'x' && str[startingIndex-1] == 'p') {
      outerFunction = 7;
      startingIndex = startingIndex - 3;
    }
  } else if (startingIndex > 2) {
    if (str[startingIndex-2] == 'l' && str[startingIndex-1] == 'n') {
      outerFunction = 1;
      startingIndex = startingIndex - 2;
    }
  }

  solveArithmetic(tempString, outerFunction);

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
  int parCheck1 = indexOf(str, ")", 0);
  int parCheck2 = indexOf(str, "(", 0);
  if (parCheck1 != -1 && parCheck2 != -1) {
    return 1;
  } else {
    return 0;
  }
}

void dollarSignDoublePar(char* str) { //Replaces expression in $(( )) with the solution to that expression
  int expCheck = indexOf(str, "$((", 0); //Check for expression in parentheses
  if (expCheck != -1) {
    int expCheck2 = indexOf(str, "))", 1);
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
      if ((exp[0] < '0' || exp[0] > '9') && exp[0] != '.' && exp[0] != '-') {
        alphanumeric = 0;
      }
      for (int i = 1; exp[i] != '\0'; i++) {
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
        strcat(outputBuffer, "Invalid expression\n");
      }
    } else {
      strcat(outputBuffer, "Error: expected '))'\n");
      return;
    }
  }
}

void wrapText(char* str, int* globalLineStarts) {
  int count = 0;
  int prevSpace = 0;
  int lineCount = 1;
  globalLineStarts[0] = 0;
  int i;

  for (i = 0; str[i] != '\0'; i++) {
    //Serial.print(str[i]);
    if (str[i] == ' ') {
      prevSpace = i;
    }
    if (count >= COLS && prevSpace != 0) {
      if (str[prevSpace] == 0x0D) {
        globalLineStarts[lineCount] = i;
        lineCount++;
        //Serial.println(" (if statement A)");
        //Serial.print(lineCount);
        //Serial.print(": ");
        count = -1;
      } else {
        str[prevSpace] = 0x0D; //A special character is used for newlines in wrapText, so that newlines typed in the text by the user can be preserved
        globalLineStarts[lineCount] = prevSpace + 1;
        lineCount++;
        //Serial.println(" (if statement B)");
        //Serial.print(lineCount);
        //Serial.print(": ");
        i = prevSpace;
        count = -1;
      }
    }
    if (str[i] == '\n') {
      globalLineStarts[lineCount] = i + 1;
      lineCount++;
        //Serial.println(" (if statement C)");
        //Serial.print(lineCount);
        //Serial.print(": ");
      count = -1;
      prevSpace = 0;
    }
    count++;
  }
  str[i] = '\0';
  globalLineStarts[lineCount] = i+1;
  globalLineStarts[lineCount+1] = -1;
  if (lineCount < ROWS) {
    onscreenLineCount = lineCount;
  } else {
    onscreenLineCount = ROWS;
  }
  //Serial.println("");
  //Serial.println("");
  //Serial.println("");
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
    //Serial.print(str[i], HEX);
    //Serial.print(" ");
    if (str[i] == 0x0D) { //A special character is used for newlines in wrapText, so that newlines typed in the text by the user can be preserved
      display.println("");
    } else {
      display.print(str[i]);
    }
  }
  //Serial.println("");
  display.display();
}

void keyboardEdit(char* str, char* saveFilePath, int card) {
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
  wrapText(str, lineStarts);
  printSection(str, lineStarts[0], lineStarts[0+onscreenLineCount]-1);
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
        if (absoluteLine(cursor, lineStarts) > 0) {
          int aboveLineLength = lineStarts[absoluteLine(cursor, lineStarts)] - lineStarts[absoluteLine(cursor, lineStarts)-1];
          int cursorSpace = cursor - lineStarts[absoluteLine(cursor, lineStarts)];
          if (aboveLineLength > cursorSpace) {
            cursor = lineStarts[absoluteLine(cursor, lineStarts)-1] + cursorSpace;
          } else {
            cursor = lineStarts[absoluteLine(cursor, lineStarts)];
          }
        }
      } else if (ps2 == 0x118) { //Down arrow
        if (absoluteLine(cursor, lineStarts) < absoluteLine(currentLen, lineStarts)) {
          int belowLineLength = lineStarts[absoluteLine(cursor, lineStarts)+2] - lineStarts[absoluteLine(cursor, lineStarts)+1];
          int cursorSpace = cursor - lineStarts[absoluteLine(cursor, lineStarts)];
          if (belowLineLength > cursorSpace) {
            cursor = lineStarts[absoluteLine(cursor, lineStarts)+1] + cursorSpace - 1;
          } else {
            if (absoluteLine(cursor, lineStarts) + 1 == absoluteLine(currentLen, lineStarts)) {
              cursor = currentLen;
            } else {
              cursor = lineStarts[absoluteLine(cursor, lineStarts)+2] - 1;
            }
          }
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
          File myFile;
          if (card) {
            inMountedPath(saveFilePath);
            SD1.remove(removablePath);
            myFile = SD1.open(removablePath, FILE_WRITE);
          } else {
            SD.remove(saveFilePath);
            myFile = SD.open(saveFilePath, FILE_WRITE);
          }
          if (myFile) {
            myFile.print(str);
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
          printSection("Exit without saving?\n[S] - Save + exit\n[X] - Exit\n[R] - Return", 0, 62);
          delay(1000);
          while (true) {
            if (keyboard.available()) {

              ps2 = keyboard.read();
              c = codesToAscii(ps2);
            
              if (c == 's') {
                File myFile;
                if (card) {
                  inMountedPath(saveFilePath);
                  SD1.remove(removablePath);
                  myFile = SD1.open(removablePath, FILE_WRITE);
                } else {
                  SD.remove(saveFilePath);
                  myFile = SD.open(saveFilePath, FILE_WRITE);
                }
                if (myFile) {
                  myFile.print(str);
                  myFile.close();
                } else {
                  Serial.println("error opening file");
                }
                persistentCursor = cursor;
                return;
              } else if (c == 'x') {
                persistentCursor = cursor;
                return;
              } else if (c == 'r') {
                break;
              }
            }
          }
        } else {
          return;
        }
      } else {
        printing = 0;
      }

      if (printing) {
        if (topLineOfScreen + onscreenLineCount + -1 > absoluteLine(currentLen, lineStarts)) {
          while (topLineOfScreen + onscreenLineCount + -1 > absoluteLine(currentLen, lineStarts)) {
            topLineOfScreen--;
          }
        }

        if (highlighting) {
          if (cursor > highlightEnd) { 
            highlightString(str, highlightEnd, cursor+1);
            wrapText(str, lineStarts);
            if (absoluteLine(cursor, lineStarts)-ROWS+1 > topLineOfScreen) {
              topLineOfScreen++;
            } else if (absoluteLine(cursor, lineStarts) < topLineOfScreen) {
              topLineOfScreen--;
            }
            printSection(str, lineStarts[topLineOfScreen], lineStarts[topLineOfScreen+onscreenLineCount]-1);
            unwrapText(str);
            backspaceChar(str, cursor+1);
            backspaceChar(str, highlightEnd);
          } else {
            highlightString(str, highlightEnd+1, cursor);
            wrapText(str, lineStarts);
            if (absoluteLine(highlightEnd, lineStarts)-ROWS+1 > topLineOfScreen) {
              topLineOfScreen++;
            } else if (absoluteLine(highlightEnd, lineStarts) < topLineOfScreen) {
              topLineOfScreen--;
            }
            printSection(str, lineStarts[topLineOfScreen], lineStarts[topLineOfScreen+onscreenLineCount]-1);
            unwrapText(str);
            backspaceChar(str, highlightEnd+1);
            backspaceChar(str, cursor);
          }
        } else {
          insInString(str, '|', cursor);
          wrapText(str, lineStarts);
          if (absoluteLine(cursor, lineStarts)-ROWS+1 > topLineOfScreen) {
            topLineOfScreen++;
          } else if (absoluteLine(cursor, lineStarts) < topLineOfScreen) {
            topLineOfScreen--;
          }
          if (lineStarts[topLineOfScreen+onscreenLineCount] < 0) {
            topLineOfScreen--;
          }
          printSection(str, lineStarts[topLineOfScreen], lineStarts[topLineOfScreen+onscreenLineCount]-1);
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
  } else if (in == 0x403B) {
    return '<';
  } else if (in == 0x403D) {
    return '>';
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

int absoluteLine(int strPoint, int* globalLineStarts) {
  int i;
  for (i = 0; globalLineStarts[i] <= strPoint; i++) {}
  return i-1;
}

void terminalKeyboardEdit(char* strIn, char* strOut) {
  int printing;
  uint16_t ps2;
  char c;
  int highlighting = 0;
  int highlightEnd;

  int staticLen;
  for (staticLen = 0; strIn[staticLen] != '\0'; staticLen++) {}

  char str[OUTPUT_LEN + 2*INPUT_LEN];
  strcpy(str, strIn);
  strcat(str, strOut);

  int currentLen;
  for (currentLen = 0; str[currentLen] != '\0'; currentLen++) {}
  int cursor = currentLen;

  insInString(str,  '|', cursor);
  wrapText(str, outputLineStarts);
  topLineOfScreen = absoluteLine(currentLen, outputLineStarts) - onscreenLineCount + 1;
  printSection(str, outputLineStarts[topLineOfScreen], outputLineStarts[topLineOfScreen+onscreenLineCount]-1);
  unwrapText(str);
  backspaceChar(str, cursor);

  while (true) {
    if (keyboard.available()) {
      // read the next key
      ps2 = keyboard.read();
      c = codesToAscii(ps2);
      printing = 1;
    
      if ((c != 0) && cursor >= staticLen) {
        if (highlighting == 1) {
          deleteSeg(str, highlightEnd, cursor, c);
          currentLen = currentLen + 1 - abs(highlightEnd - cursor);
          if (cursor > highlightEnd) {
            cursor = highlightEnd;
          }
          cursor++;
          highlighting = 0;
        } else {
          //Serial.print("cursor: ");
          //Serial.println(cursor);
          //Serial.print("currentLen: ");
          //Serial.println(currentLen);
          //Serial.print("staticLen: ");
          //Serial.println(staticLen);
          if (currentLen+1-staticLen < INPUT_LEN) {
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
        if (absoluteLine(cursor, outputLineStarts) > 0) {
          int aboveLineLength = outputLineStarts[absoluteLine(cursor, outputLineStarts)] - outputLineStarts[absoluteLine(cursor, outputLineStarts)-1];
          int cursorSpace = cursor - outputLineStarts[absoluteLine(cursor, outputLineStarts)];
          if (aboveLineLength > cursorSpace) {
            cursor = outputLineStarts[absoluteLine(cursor, outputLineStarts)-1] + cursorSpace;
          } else {
            cursor = outputLineStarts[absoluteLine(cursor, outputLineStarts)];
          }
        }
      } else if (ps2 == 0x118) { //Down arrow
        if (absoluteLine(cursor, outputLineStarts) < absoluteLine(currentLen, outputLineStarts)) {
          int belowLineLength = outputLineStarts[absoluteLine(cursor, outputLineStarts)+2] - outputLineStarts[absoluteLine(cursor, outputLineStarts)+1];
          int cursorSpace = cursor - outputLineStarts[absoluteLine(cursor, outputLineStarts)];
          if (belowLineLength > cursorSpace) {
            cursor = outputLineStarts[absoluteLine(cursor, outputLineStarts)+1] + cursorSpace - 1;
          } else {
            if (absoluteLine(cursor, outputLineStarts) + 1 == absoluteLine(currentLen, outputLineStarts)) {
              cursor = currentLen;
            } else {
              cursor = outputLineStarts[absoluteLine(cursor, outputLineStarts)+2] - 1;
            }
          }
        }
      } else if (ps2 == 0x11C) { //Backspace
        if (highlighting == 1 && cursor > staticLen && highlightEnd > staticLen) {
          deleteSeg(str, highlightEnd, cursor, 0);
          currentLen = currentLen - abs(highlightEnd - cursor);
          if (cursor > highlightEnd) {
            cursor = highlightEnd;
          }
          highlighting = 0;
        } else if (cursor > staticLen) {
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
      } else if ((ps2 == 0x2058) && highlighting && cursor >= staticLen && highlightEnd >= staticLen) { //Ctrl + x
        copySeg(clipboard, highlightEnd, cursor, str);
        clipboardLen = abs(highlightEnd - cursor);
        deleteSeg(str, highlightEnd, cursor, 0);
        currentLen = currentLen - abs(highlightEnd - cursor);
        if (cursor > highlightEnd) {
          cursor = highlightEnd;
        }
        highlighting = 0;
      } else if (ps2 == 0x2056 && cursor >= staticLen) { //Ctrl + v
        if (highlighting == 1 && highlightEnd >= staticLen) {
          deleteSeg(str, highlightEnd, cursor, 0);
          currentLen = currentLen - abs(highlightEnd - cursor);
          if (cursor > highlightEnd) {
            cursor = highlightEnd;
          }
          highlighting = 0;
        }

        if (currentLen+clipboardLen-staticLen < INPUT_LEN) { 
          for (int i = 0; i < clipboardLen; i++) {
            insInString(str, clipboard[i], cursor);
            currentLen++;
            cursor++;
          }
        } else {
          Serial.println("Buffer full");
          printing = 0;
        }
      } else if (ps2 == 0x011E) { //Enter
        int j = 0;
        for (int i = staticLen; str[i] != '\0'; i++) {
          strOut[j] = str[i];
          j++;
        }
        strOut[j] = '\0';
        return;
      } else {
        printing = 0;
      }

      if (printing) {
        if (topLineOfScreen + onscreenLineCount + -1 > absoluteLine(currentLen, outputLineStarts)) {
          while (topLineOfScreen + onscreenLineCount + -1 > absoluteLine(currentLen, outputLineStarts) && topLineOfScreen > 0) {
            topLineOfScreen--;
          }
        }

        if (highlighting) {
          if (cursor > highlightEnd) { 
            highlightString(str, highlightEnd, cursor+1);
            wrapText(str, outputLineStarts);
            if (absoluteLine(cursor, outputLineStarts)-ROWS+1 > topLineOfScreen) {
              topLineOfScreen++;
            } else if (absoluteLine(cursor, outputLineStarts) < topLineOfScreen) {
              topLineOfScreen--;
            }
            printSection(str, outputLineStarts[topLineOfScreen], outputLineStarts[topLineOfScreen+onscreenLineCount]-1);
            unwrapText(str);
            backspaceChar(str, cursor+1);
            backspaceChar(str, highlightEnd);
          } else {
            highlightString(str, highlightEnd+1, cursor);
            wrapText(str, outputLineStarts);
            if (absoluteLine(highlightEnd, outputLineStarts)-ROWS+1 > topLineOfScreen) {
              topLineOfScreen++;
            } else if (absoluteLine(highlightEnd, outputLineStarts) < topLineOfScreen) {
              topLineOfScreen--;
            }
            printSection(str, outputLineStarts[topLineOfScreen], outputLineStarts[topLineOfScreen+onscreenLineCount]-1);
            unwrapText(str);
            backspaceChar(str, highlightEnd+1);
            backspaceChar(str, cursor);
          }
        } else {
          insInString(str, '|', cursor);
          wrapText(str, outputLineStarts);
          if (absoluteLine(cursor, outputLineStarts)-ROWS+1 > topLineOfScreen) {
            topLineOfScreen++;
          } else if (absoluteLine(cursor, outputLineStarts) < topLineOfScreen) {
            topLineOfScreen--;
          }
          if (outputLineStarts[topLineOfScreen+onscreenLineCount] < 0) {
            topLineOfScreen--;
          }
          printSection(str, outputLineStarts[topLineOfScreen], outputLineStarts[topLineOfScreen+onscreenLineCount]-1);
          unwrapText(str);
          backspaceChar(str, cursor);
        }
      }
    }
  }
}

void argsSeparate(char* strStart, char* strEnd) {
  for (int i = 0; strStart[i] != '\0'; i++) { //Break up the input into command (everything before first space) and arguments
    if (strStart[i] == ' ') {
      strcpy(strEnd, strStart + i + 1);
      strEnd[INPUT_LEN-1] = '\0';
      strStart[i] = '\0';
      break;
    }
  }
}

int inMountedPath(char* str) { //Determine whether a path is inside the mounted drive. If it is, set removablePath to the truncated form of that path that can access the mounted drive file system
  if ((indexOf(str, mountPointPath, 0) == 0) && removableSDMounted) {
    int j = 0;
    for (int i = mountPointLen; str[i] != '\0'; i++) {
      removablePath[j] = str[i];
      j++;
    }
    removablePath[j] = '\0';
    return 1;
  } else {
    return 0;
  }
}

void setup() {
  Serial.begin(115200);
  display.begin(0, true);
  display.display();
  delay(2000);
  display.clearDisplay();
  display.display();
  keyboard.begin(DataPin, IRQpin);

  if (!SD.begin(internalSD)) {
    printSection("SD card\ninitialization failed", 0, 29);
    while (true);
  }
}

void loop() {
  strcpy(promptBuffer, "root@lookingglass:");
  strcat(promptBuffer, currentPath);
  strcat(promptBuffer, "# ");
  strcat(outputBuffer, promptBuffer);
  //Serial.print("outputBuffer: ");
  //Serial.println(outputBuffer);
  //Serial.print("inputBuffer: ");
  //Serial.println(inputBuffer);
  terminalKeyboardEdit(outputBuffer, inputBuffer);
  strcpy(outputBuffer, promptBuffer);
  strcat(outputBuffer, inputBuffer);
  strcat(outputBuffer, "\n");
  executeCommand(inputBuffer);
  inputBuffer[0] = '\0';
}

void runScript(const char* content);

void executeCommand(char* line) {
  char cmd[INPUT_LEN] = "";
  char args[INPUT_LEN] = "";

  strcpy(cmd, line);

  dollarSignDoublePar(cmd);
  argsSeparate(cmd, args);
  toLowercase(cmd);

  if (strcmp_P(cmd, PSTR("ls")) == 0) {
    int empty = 1;

    File lsDir;
    if (inMountedPath(currentPath)) {
      lsDir = SD1.open(removablePath);
    } else {
      lsDir = SD.open(currentPath);
    }
    while (true) {
      File lsEntry =  lsDir.openNextFile();
      if (! lsEntry) {
        // no more files
        strcat(outputBuffer, "\n");
        break;
      }
      empty = 0;

      strcat(outputBuffer, lsEntry.name());

      if (lsEntry.isDirectory()) {
        strcat(outputBuffer, "/   ");
      } else {
        strcat(outputBuffer, "   ");
      }
      lsEntry.close();
    }

    if (empty) strcat(outputBuffer, "(empty)\n");
    lsDir.close();
  }
  else if (strcmp_P(cmd, PSTR("mkdir")) == 0) {
    char dirPath[NAME_LEN + PATH_LEN];

    if (inMountedPath(currentPath)) {
      if (strcmp_P(removablePath, PSTR("/")) == 0) {
        strcpy(dirPath, args);
      } else {
        strcpy(dirPath, removablePath);
        strcat(dirPath, "/");
        strcat(dirPath, args);
      }

      SD1.mkdir(dirPath);
    } else {
      if (strcmp_P(currentPath, PSTR("/")) == 0) {
        strcpy(dirPath, args);
      } else {
        strcpy(dirPath, currentPath);
        strcat(dirPath, "/");
        strcat(dirPath, args);
      }

      SD.mkdir(dirPath);
    }
  }
  else if (strcmp_P(cmd, PSTR("touch")) == 0) {
    char filePath[NAME_LEN + PATH_LEN];
    File myFile;
    if (inMountedPath(currentPath)) {
      strcpy(filePath, removablePath);
      strcat(filePath, "/");
      strcat(filePath, args);
      
      myFile = SD1.open(filePath, FILE_WRITE);
    } else {
      strcpy(filePath, currentPath);
      strcat(filePath, "/");
      strcat(filePath, args);
      
      myFile = SD.open(filePath, FILE_WRITE);
    }
    myFile.close();
  }
  else if (strcmp_P(cmd, PSTR("cd")) == 0) {
    if (strcmp_P(args, PSTR("..")) == 0 || strcmp_P(args, PSTR("/")) == 0) { //Move back to top directory
      strcpy(currentPath, "/");
      currentPath[PATH_LEN - 1] = '\0';
    } else {
      int found = 0;

      File dir;
      if (inMountedPath(currentPath)) {
        dir = SD1.open(removablePath);
      } else {
        dir = SD.open(currentPath);
      }

      while (true) {
        File entry =  dir.openNextFile();
        if (! entry) {
          // no more files
          //Serial.println("");
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
          strcat(outputBuffer, "Path too long.");
          return;
        }
      } else {
        strcat(outputBuffer, "No dir.");
      }
    }
  }
  else if (strcmp_P(cmd, PSTR("pwd")) == 0) {
    strcat(outputBuffer, currentPath);
    strcat(outputBuffer, "\n");
  }
  else if (strcmp_P(cmd, PSTR("echo")) == 0) {
    variableSubstitute(args);

    int arrow = indexOf(args, " > ", 0);
    if (arrow != -1) { //Option to use > to send string to file
      char text[40] = "";
      strncpy(text, args, arrow);
      text[arrow] = '\0';
      char filename[12] = "";
      strcpy(filename, args + arrow + 3);
      filename[NAME_LEN - 1] = '\0';
      char newfilepath[PATH_LEN] = "";
      if (inMountedPath(currentPath)) {
        strcpy(newfilepath, removablePath);
      } else {
        strcpy(newfilepath, currentPath);
      }

      strcat(newfilepath, filename);

      File dataFile;
      if (inMountedPath(currentPath)) {
        dataFile = SD1.open(newfilepath, FILE_WRITE);
      } else {
        dataFile = SD.open(newfilepath, FILE_WRITE);
      }

      if (dataFile) {
        dataFile.print(text);
        dataFile.close();
        strcat(outputBuffer, text);
      } else {
        strcat(outputBuffer, "error opening file");
      }
    } else {
      strcat(outputBuffer, args);
    }
    strcat(outputBuffer, "\n");
  }
  else if (strcmp_P(cmd, PSTR("cat")) == 0) {
    char str[OUTPUT_LEN - (26 + NAME_LEN + PATH_LEN)];
    char newfilepath[PATH_LEN] = "";
    if (inMountedPath(currentPath)) {
      strcpy(newfilepath, removablePath);
    } else {
      strcpy(newfilepath, currentPath);
    }
    strcat(newfilepath, args);

    File myFile;
    if (inMountedPath(currentPath)) {
      myFile = SD1.open(newfilepath);
    } else {
      myFile = SD.open(newfilepath);
    }

    if (myFile) {
      int i = 0;
      while (myFile.available() && i < MAX_LEN) {
        str[i] = myFile.read();
        if (i > OUTPUT_LEN - (26 + NAME_LEN + PATH_LEN)) {
          break;
        }
        i++;
      }
      str[i] = '\0';
      strcat(outputBuffer, str);
      myFile.close();
    } else {
      strcat(outputBuffer, "error opening file");
    }
    strcat(outputBuffer, "\n");
  }
  else if (strcmp_P(cmd, PSTR("info")) == 0) {
    File myFile;
    char newfilepath[PATH_LEN] = "";
    char str[10];
    if (inMountedPath(currentPath)) {
      strcpy(newfilepath, removablePath);
      strcat(newfilepath, args);

      myFile = SD1.open(newfilepath);
    } else {
      strcpy(newfilepath, currentPath);
      strcat(newfilepath, args);

      myFile = SD.open(newfilepath);
    }
    if (myFile) {
      strcat(outputBuffer, "Name: ");
      strcat(outputBuffer, myFile.name());
      strcat(outputBuffer, "\nType: ");
      strcat(outputBuffer, (myFile.isDirectory() ? "Directory" : "File"));
      strcat(outputBuffer, "\nSize: ");
      sprintf(str, "%d", myFile.size());
      strcat(outputBuffer, str);
      strcat(outputBuffer, " bytes\n");
    } else {
      strcat(outputBuffer, "Not found.\n");
    }
    myFile.close();
  }
  else if (strcmp_P(cmd, PSTR("rm")) == 0) {
    char filePath[NAME_LEN + PATH_LEN];
    if (inMountedPath(currentPath)) {
      if (strcmp_P(removablePath, PSTR("/")) == 0) {
        strcpy(filePath, args);
      } else {
        strcpy(filePath, removablePath);
        strcat(filePath, "/");
        strcat(filePath, args);
      }
      if (SD1.exists(filePath)) {
        SD1.remove(filePath);
        SD1.rmdir(filePath);
        strcat(outputBuffer, "Removed\n");
      } else {
        strcat(outputBuffer, "Not found\n");
      }
    } else {
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
        strcat(outputBuffer, "Removed\n");
      } else {
        strcat(outputBuffer, "Not found\n");
      }
    }
  }
  else if (strcmp_P(cmd, PSTR("dmesg")) == 0) {
    strcat(outputBuffer, "=== KERNEL MESSAGES ===\n");
    char str[10];
    int j;
    for (j = 0; j < DMESG_LINES; j++) {
      if (dmesg[j].message[0] != '\0') {
        strcat(outputBuffer, "[");
        sprintf(str, "%d", dmesg[j].timestamp);
        strcat(outputBuffer, str);
        strcat(outputBuffer, "] ");
        strcat(outputBuffer, dmesg[j].message);
        strcat(outputBuffer, "\n");
      }
    }
  }
  else if (strcmp_P(cmd, PSTR("uptime")) == 0) {
    unsigned long s = millis() / 1000;
    unsigned long h = s / 3600;
    unsigned long m = (s % 3600) / 60;
    unsigned long sec = s % 60;

    char str[10];
    strcat(outputBuffer, "up ");
    sprintf(str, "%d", h);
    strcat(outputBuffer, str);
    strcat(outputBuffer, "h ");
    sprintf(str, "%d", m);
    strcat(outputBuffer, str);
    strcat(outputBuffer, "m ");
    sprintf(str, "%d", sec);
    strcat(outputBuffer, str);
    strcat(outputBuffer, "s\n");
    addDmesg(F("uptime command"));
  }
  else if (strcmp_P(cmd, PSTR("df")) == 0 || strcmp_P(cmd, PSTR("free")) == 0) {
    char str[10];
    sprintf(str, "%f", freeMemory(internalSD));
    strcat(outputBuffer, "Free Disk Space: ");
    strcat(outputBuffer, str);
    strcat(outputBuffer, " GB\n");
  }
  else if (strcmp_P(cmd, PSTR("whoami")) == 0) {
    strcat(outputBuffer, "root\n");
  }
  else if (strcmp_P(cmd, PSTR("uname")) == 0) {
    char str[10];
    sprintf(str, "%.2f", freeMemory(internalSD));
    strcat(outputBuffer, "SDLookingGlass v1.0\n"
    "Kernel: Arduino "
    "AVR\n"
    "Hardware: Teensy 4.1\n"
    "Disk Space: ");
    strcat(outputBuffer, str);
    strcat(outputBuffer, " GB free\n");
  }
  else if (strcmp_P(cmd, PSTR("reboot")) == 0) {
    printSection("Rebooting...", 0, 12);
    addDmesg(F("System reboot"));
    delay(500);
    resetFunc();
  }
  else if (strcmp_P(cmd, PSTR("clear")) == 0) {
    inputBuffer[0] = '\0';
    outputBuffer[0] = '\0';
  }
  else if (strcmp_P(cmd, PSTR("sh")) == 0) {
    if (args[0] == '\0') {
      strcat(outputBuffer, "Usage: sh [script]");
      return;
    }

    char script[INPUT_LEN];

    File myFile;
    if (inMountedPath(currentPath)) {
      myFile = SD1.open(args);
    } else {
      myFile = SD.open(args);
    }
    if (myFile) {
      addDmesg(F("sh: running script"));
      int j = 0;
      while (myFile.available()) {
        script[j] = myFile.read();
        j++;
      }
      myFile.close();
      script[j] = '\0';
      runScript(script);
    } else {
      strcat(outputBuffer, "Script not found.");
    }
    strcat(outputBuffer, "\n");
  }
  else if (strcmp_P(cmd, PSTR("alias")) == 0) {
    if (args[0] == '\0') {
      int j, any = 0;
      for (j = 0; j < MAX_ALIASES; j++) {
        if (aliases[j].active) {
          //Serial.print(aliases[j].name);
          //Serial.print(F("='"));
          //Serial.print(aliases[j].value);
          //Serial.println(F("'"));
          strcat(outputBuffer, aliases[j].name);
          strcat(outputBuffer, "='");
          strcat(outputBuffer, aliases[j].value);
          strcat(outputBuffer, "'");
          any = 1;
        }
      }
      if (!any) strcat(outputBuffer, "No aliases.");
    } else {
      int eq = indexOf(args, "=", 0);
      if (eq == -1) {
        // show single alias
        int j, found = 0;
        for (j = 0; j < MAX_ALIASES; j++) {
          if (aliases[j].active && strcmp(aliases[j].name, args) == 0) {
            //Serial.print(args); Serial.print(F("='")); Serial.print(aliases[j].value); Serial.println(F("'"));
            strcat(outputBuffer, args);
            strcat(outputBuffer, "='");
            strcat(outputBuffer, aliases[j].value);
            strcat(outputBuffer, "'");
            found = 1; break;
          }
        }
        if (!found) strcat(outputBuffer, "No such alias.");
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
        if (slot == -1) { strcat(outputBuffer, "Alias table full."); return; }
        strncpy(aliases[slot].name, aname, ALIAS_NAME_LEN);
        aliases[slot].name[ALIAS_NAME_LEN - 1] = '\0';
        strncpy(aliases[slot].value, aval, ALIAS_VAL_LEN);
        aliases[slot].value[ALIAS_VAL_LEN - 1] = '\0';
        aliases[slot].active = 1;
        strcat(outputBuffer, "Alias set.");
      }
    }
    strcat(outputBuffer, "\n");
  }
  else if (strcmp_P(cmd, PSTR("help")) == 0) {
    strcat(outputBuffer, "ls - List files\n\n"
    "cd - Change directory\n\n"
    "pwd - Print working directory\n\n"
    "mkdir - Make directory\n\n"
    "touch - Create file\n\n"
    "cat - Read file text\n\n"
    "echo - Echo text to terminal\n\n"
    "echo [text] > [file]  -- Add text to file\n\n"
    "rm - Remove file or directory\n\n"
    "info - File info\n\n"
    "sh - Run shell script\n\n"
    "sh [file]  -- run script (use ; as line separator)\n\n"
    "uptime - Show uptime\n\n"
    "uname - Show device info\n\n"
    "dmesg - Show kernel messages\n\n"
    "df, free - Show free disk space\n\n"
    "whoami - Show username\n\n"
    "clear - Clear terminal\n\n"
    "reboot - Reboot device\n\n"
    "alias - Create command alias\n\n"
    "$(([expression]))  -- Solve a math expression\n\n"
    "[variable]=[number]  -- Set a variable\n\n"
    "unset - Unset a variable\n\n"
    "variables - List values of all variables\n");
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
      strcat(outputBuffer, "Variable not found\n");
    }
  }
  else if (strcmp_P(cmd, PSTR("notepad")) == 0) {
    int editRemovablePath = 0;
    char newfilepath[PATH_LEN] = "";
    strcpy(newfilepath, currentPath);
    strcat(newfilepath, args);

    File myFile;
    if (inMountedPath(newfilepath)) {
      myFile = SD1.open(removablePath);
      editRemovablePath = 1;
    } else {
      myFile = SD.open(newfilepath);
    }
    int i = 0;
    if (myFile) {
      while (myFile.available() && i < MAX_LEN) {
        fileBuffer[i] = myFile.read();
        if (i >= 3) {
          if (fileBuffer[i-3] == 0x20 && fileBuffer[i-2] == 0x0A && fileBuffer[i-1] == 0x0D && fileBuffer[i] == 0x0A) { //I get this series of chars at the end of files saved in FeatherPad. They add extra spaces to the end of the text, and if you try to delete them it makes the screen go blank, so this replaces them when the file is loaded
            i = i - 3;
            break;
          }
        }
        i++;
      }
      myFile.close();
      fileBuffer[i] = '\0';
      keyboardEdit(fileBuffer, newfilepath, editRemovablePath);
      wrapText(fileBuffer, lineStarts);
      printSection(fileBuffer, lineStarts[0], lineStarts[0+onscreenLineCount]-1);
      unwrapText(fileBuffer);
      display.clearDisplay();
      display.display();
    } else {
      strcat(outputBuffer, "error opening file");
    }
  }
  else if (strcmp_P(cmd, PSTR("variables")) == 0) {
    char str[10];
    strcat(outputBuffer, "All current vars:\n");
    for (int i = 0; i < varIndex; i++) {
      strcat(outputBuffer, vars[i].name);
      strcat(outputBuffer, ": ");
      strcpy(str, vars[i].name);
      variableSubstitute(str);
      strcat(outputBuffer, str);
      strcat(outputBuffer, "\n");
      str[0] = '\0';
    }
  }
  else if (strcmp_P(cmd, PSTR("lsblk")) == 0) {
    char str[10];
    sprintf(str, "%.2f", freeMemory(internalSD));
    strcat(outputBuffer, "NAME SIZE    MTPTS\n");
    strcat(outputBuffer, "sda  ");
    strcat(outputBuffer, str);
    strcat(outputBuffer, "GB /\n");
    if (SD1.begin(removableSD)) {
      //sprintf(str, "%.2f", freeMemory(removableSD));
      strcat(outputBuffer, "mmc  ");
      strcat(outputBuffer, str);
      strcat(outputBuffer, "GB ");
      strcat(outputBuffer, mountPointPath);
      strcat(outputBuffer, "\n");
    }
    //delay(700);
    //display.begin(0, true);
    //display.display();
  }
  else if (strcmp_P(cmd, PSTR("cp")) == 0) {
    char originalFile[INPUT_LEN];
    char copyFile[INPUT_LEN];
    strcpy(originalFile, args);
    argsSeparate(originalFile, copyFile);

    char openFilePath[PATH_LEN] = "";
    strcpy(openFilePath, currentPath);
    strcat(openFilePath, originalFile);
    File openOriginalFile;
    if (inMountedPath(openFilePath)) {
      openOriginalFile = SD1.open(removablePath);
    } else {
      openOriginalFile = SD.open(openFilePath);
    }
    int i = 0;
    if (openOriginalFile) {
      while (openOriginalFile.available() && i < MAX_LEN) {
        fileBuffer[i] = openOriginalFile.read();
        if (i >= 3) {
          if (fileBuffer[i-3] == 0x20 && fileBuffer[i-2] == 0x0A && fileBuffer[i-1] == 0x0D && fileBuffer[i] == 0x0A) { //I get this series of chars at the end of files saved in FeatherPad. They add extra spaces to the end of the text, and if you try to delete them it makes the screen go blank, so this replaces them when the file is loaded
            i = i - 3;
            break;
          }
        }
        i++;
      }
      openOriginalFile.close();
      fileBuffer[i] = '\0';

      char copiedFilePath[12] = "";
      strcpy(copiedFilePath, currentPath);
      strcat(copiedFilePath, copyFile);
      File dataFile;
      if (inMountedPath(copiedFilePath)) {
        dataFile = SD1.open(removablePath, FILE_WRITE);
      } else {
        dataFile = SD.open(copiedFilePath, FILE_WRITE);
      }
      if (dataFile) {
        dataFile.print(fileBuffer);
        dataFile.close();
      }
    } else {
      strcat(outputBuffer, "error opening file");
    }
  }
  else if (strcmp_P(cmd, PSTR("mount")) == 0) {
    char folder[INPUT_LEN];
    char mountDevice[INPUT_LEN];
    strcpy(folder, args);
    argsSeparate(folder, mountDevice);

    int found = 0;
    File dir = SD.open(currentPath);
    while (true) {
      File entry =  dir.openNextFile();
      if (! entry) {
        // no more files
        //Serial.println("");
        break;
      }

      if (entry.isDirectory() && strcmp(folder, entry.name()) == 0) {
        found = 1;
      }
      entry.close();
    }
    if (found) {
      strcat(mountPointPath, currentPath);
      strcat(mountPointPath, folder);
      for (mountPointLen = 0; mountPointPath[mountPointLen] != '\0'; mountPointLen++) {

      }
      strcat(mountPointPath, "/");
    } else {
      strcat(outputBuffer, "No dir\n");
      return;
    }

    if ((strcmp_P(mountDevice, PSTR("mmc")) == 0) && SD1.begin(removableSD)) {
      removableSDMounted = 1;
    } else {
      strcpy(mountPointPath, "");
      strcat(outputBuffer, "Device ");
      strcat(outputBuffer, mountDevice);
      strcat(outputBuffer, " not found\n");
      removableSDMounted = 0;
    }
    //delay(700);
    //display.begin(0, true);
    //display.display();
  }
  else if (strcmp_P(cmd, PSTR("umount")) == 0) {
    if (strcmp_P(mountPointPath, args) == 0) {
      strcpy(mountPointPath, "");
      removableSDMounted = 0;
      mountPointLen = 0;
    } else {
      strcat(outputBuffer, "Mountpoint ");
      strcat(outputBuffer, args);
      strcat(outputBuffer, " not found\n");
    }
  }
  else if (strcmp_P(cmd, PSTR("debug")) == 0) {
    //Serial.println(inMountedPath(currentPath));
    //Serial.println(removablePath);
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

    int eqCheck = indexOf(cmd, "=", 0); //Check for variables being set
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
          strcat(outputBuffer, "No space available for variables");
        }
      }

      /*Serial.println(F("All current vars:"));
      for (int i = 0; i < varIndex; i++) {
        Serial.print(vars[i].name);
        Serial.print(": ");
        Serial.println(vars[i].value);
      }*/
    }
    if (!resolved) strcat(outputBuffer, "\nUnknown command.");
  }
}

// Interpreter sh
void runScript(const char* content) {
  char line[INPUT_LEN];
  int ci = 0, li = 0, lineNum = 0;
  int len = strlen(content);
  char num[10];

  Serial.print("content: ");
  Serial.println(content);

  while (ci <= len) {
    char c = (ci < len) ? content[ci] : ';';
    ci++;
    if (c == ';' || c == '\n' || c == '\r') {
      if (li > 0) {
        line[li] = '\0';
        lineNum++;
        strcat(outputBuffer, "[sh:");
        sprintf(num, "%d", lineNum);
        strcat(outputBuffer, num);
        strcat(outputBuffer, "] ");
        strcat(outputBuffer, line);
        strcat(outputBuffer, "\n");
        executeCommand(line);
        strcat(outputBuffer, "\n");
        li = 0;
      }
    } else {
      if (li < INPUT_LEN-1) line[li++] = c;
    }
  }
  addDmesg(F("sh: script done"));
  strcat(outputBuffer, "[sh] done.");
}
