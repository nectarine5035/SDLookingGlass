#include <Arduino.h>
#include <string.h>
#include <avr/pgmspace.h>
#include <SD.h>
      
#define NAME_LEN 12
#define PATH_LEN 16
#define DMESG_LINES 6
#define DMESG_LEN 40
#define VAR_SPACES 6
#define VAR_NAME_LEN 8

const int chipSelect = BUILTIN_SDCARD;

typedef struct {
  unsigned long timestamp;
  char message[DMESG_LEN];
} DmesgEntry;

typedef struct {
  float value;
  char name[VAR_NAME_LEN];
} shVariable;

char currentPath[PATH_LEN] = "/";
char inputBuffer[32] = "";
int inputLen = 0;
DmesgEntry dmesg[DMESG_LINES];
int dmesgIndex = 0;
shVariable vars[VAR_SPACES];
int varIndex = 0;

#define MAX_ALIASES 4
#define ALIAS_NAME_LEN 6
#define ALIAS_VAL_LEN 20
typedef struct {
  char name[ALIAS_NAME_LEN];
  char value[ALIAS_VAL_LEN];
  int active;
} AliasEntry;
AliasEntry aliases[MAX_ALIASES];

char* expressionToParse;

//File root;

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
  //Serial.print("Volume size (GB):  ");
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

void addDmesgRam(const char* msg) {
  if (dmesgIndex >= DMESG_LINES) dmesgIndex = 0;
  dmesg[dmesgIndex].timestamp = millis() / 1000;
  strncpy(dmesg[dmesgIndex].message, msg, DMESG_LEN - 1);
  dmesg[dmesgIndex].message[DMESG_LEN - 1] = '\0';
  dmesgIndex++;
}

void printPrompt() {
  Serial.print(F("root@lookingglass:"));
  Serial.print(currentPath);
  Serial.print(F("# "));
}

void setup() {
  Serial.begin(115200);

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
        memset(inputBuffer, 0, 32);
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
    else if (inputLen < 31) {
      Serial.print(c);
      inputBuffer[inputLen] = c;
      inputLen++;
    }
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

int atoi_safe(const char* str) { //Convert multi digit number in a string to an integer
  int num = 0;
  while (*str >= '0' && *str <= '9') {
    num = num * 10 + (*str - '0');
    str++;
  }
  return num;
}

void toLowercase(char* str) {
  int i;
  for (i = 0; str[i] != '\0'; i++) {
    if (str[i] >= 'A' && str[i] <= 'Z') str[i] = str[i] - 'A' + 'a';
  }
}

void toUppercase(char* str) {
  int i;
  for (i = 0; str[i] != '\0'; i++) {
    if (str[i] >= 'a' && str[i] <= 'z') str[i] = str[i] - 'a' + 'A';
  }
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
  for (int i = 0; i < 31; i++) {
    if (str[i] == '.') {
      pos = 1;
    } else if ((str[i] - '0') >= 0 && (str[i] - '0') <= 9) {
      if (pos) {
        afterDec++;
      } else {
        beforeDec++;
      }
    } else if (str[i] == '\0') {
      break;
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

char peek() {
  return *expressionToParse;
}

char get() {
  return *expressionToParse++;
}

float number() {
  float result = get() - '0';
  while (peek() >= '0' && peek() <= '9') {
    result = 10 * result + get() - '0';
  }
  return result;
}

float factor() {
  if (peek() >= '0' && peek() <= '9')
    return number();
  else if (peek() == '(') {
    get();  // '('
    float result = expression();
    get();  // ')'
    return result;
  } else if (peek() == '-') {
    get();
    return -factor();
  }
  return 0;  // error
}

float term() {
  float result = factor();
  while (peek() == '*' || peek() == '/')
    if (get() == '*')
      result *= factor();
    else
      result /= factor();
  return result;
}

float expression() {
  float result = term();
  while (peek() == '+' || peek() == '-')
    if (get() == '+')
      result += term();
    else
      result -= term();
  return result;
}

void variableSubstitute(char* str) { //Replaces variable name with the value of that variable
  for (int i = 0; i < varIndex; i++) { //Check for variables in entry
    int varCheck = indexOf(str, vars[i].name);
    if (varCheck != -1) {
      int j = 0; //Length of variable name
      while (vars[i].name[j] != '\0') {
        j++;
      }

      char flOut[8];
      int k; //Length of variable value
      if (fmod(vars[i].value, 1) == 0.00) {
        k = ceil(log10(vars[i].value));
        snprintf(flOut, k+1, "%f", vars[i].value);
      } else {
        k = 7;
        snprintf(flOut, 8, "%f", vars[i].value);
      }

      int m = 0;
      char substitute[32];
      for (int l = 0; l < varCheck; l++) {
        substitute[m] = str[l];
        m++;
      }
      for (int l = 0; l < k; l++) {
        substitute[m] = flOut[l];
        m++;
      }
      for (int l = (varCheck + j); l < 32; l++) {
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

void dollarSignDoublePar(char* str) { //Replaces expression in $(( )) with the solution to that expression
  int expCheck = indexOf(str, "$(("); //Check for expression in parentheses
  if (expCheck != -1) {
    int expCheck2 = indexOf(str, "))");
    if (expCheck2 != -1) {
      variableSubstitute(str);
      int j = 0;
      char cmdOld[32] = "";
      strncpy(cmdOld, str, 31);
      char exp[32] = "";
      exp[31] = '\0';

      for (int i = expCheck + 3; i < expCheck2; i++) { //Copy text in parentheses to exp
        exp[j] = str[i];
        j++;
      }
      exp[j] = '\0';

      expressionToParse = exp; //Parse expression in exp. Calc functions from https://github.com/Enjoy-Mechatronics/Arduino-Calculator
      float result = expression();
      if(isinf(result) == 0 && isnan(result)==0){
        if (fmod(result, 1) == 0.00) {
          int intResult = floor(result);
          sprintf(exp, "%d", intResult);
          j = ceil(log10(intResult));
        } else {
          sprintf(exp, "%f", result);
          j = 8;
        }
      }else{
        Serial.println("Invalid calculation");
        return;
      }

      for (int i = 0; i <= j; i++) { //Replace parentheses section with exp
        str[expCheck + i] = exp[i];
      }
      for (int i = expCheck2 + 2; i < 31; i++) {
        str[expCheck + j] = cmdOld[i];
        j++;
        if (cmdOld[i] == '\0') {
          break;
        }
      }
    } else {
      Serial.println(F("Error: expected '))'"));
      return;
    }
  }
}

void runScript(const char* content);

void executeCommand(char* line) {
  char cmd[32] = "";
  char args[32] = "";
  int space1 = -1;
  int i, sp, pin, count;
  char buf[40];

  strncpy(cmd, line, 31);
  cmd[31] = '\0';

  dollarSignDoublePar(cmd);

  for (i = 0; cmd[i] != '\0'; i++) { //Break up the input into command (everything before first space) and arguments
    if (cmd[i] == ' ') {
      space1 = i;
      strncpy(args, cmd + i + 1, 31);
      args[31] = '\0';
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
    /*strcpy(dirPath, currentPath);
    strcat(dirPath, "/");
    strcat(dirPath, args);*/

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
      strncpy(currentPath, "/", PATH_LEN - 1);
      currentPath[PATH_LEN - 1] = '\0';
    } else {
      int found = 0;
      //toUppercase(args);

      File dir = SD.open(currentPath);

      //Serial.print("Looking for: ");
      //Serial.println(args);

      while (true) {
        File entry =  dir.openNextFile();
        if (! entry) {
          // no more files
          Serial.println("");
          break;
        }

        //Serial.print("Checking: ");
        //Serial.println(entry.name());

        if (entry.isDirectory() && strcmp(args, entry.name()) == 0) {
          found = 1;
        }
        entry.close();
      }

      if (found) {
        if (!safeConcatPath(currentPath, args)) {
          strncpy(currentPath, "/", PATH_LEN - 1);
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
      strncpy(filename, args + arrow + 3, NAME_LEN - 1);
      filename[NAME_LEN - 1] = '\0';
      char newfilepath[PATH_LEN] = "";
      strncpy(newfilepath, currentPath, PATH_LEN - 1);

      //Serial.print(F("newfilepath before concat: "));
      //Serial.println(newfilepath);

      //Serial.print(F("filename before concat: "));
      //Serial.println(filename);

      strcat(newfilepath, filename);

      //Serial.print(F("newfilepath after concat: "));
      //Serial.println(newfilepath);

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
    //toUppercase(args);

    char newfilepath[PATH_LEN] = "";
    strncpy(newfilepath, currentPath, PATH_LEN - 1);
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
    //toUppercase(args);
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
    //toUppercase(args);
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

    char script[32];

    //toUppercase(args);
    File myFile = SD.open(args);
    if (myFile) {
      addDmesg(F("sh: running script"));
      int j = 0;
      while (myFile.available()) {
        script[j] = myFile.read();
        j++;
      }
      myFile.close();
      script[31] = '\0';
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
        strncpy(aliases[slot].name, aname, ALIAS_NAME_LEN - 1);
        aliases[slot].name[ALIAS_NAME_LEN - 1] = '\0';
        strncpy(aliases[slot].value, aval, ALIAS_VAL_LEN - 1);
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
    Serial.println(F(" "));
  }
  else {
    // check alias
    int j, resolved = 0;
    for (j = 0; j < MAX_ALIASES; j++) {
      if (aliases[j].active && strcmp(aliases[j].name, cmd) == 0) {
        char aliasLine[32] = "";
        strncpy(aliasLine, aliases[j].value, 31);
        aliasLine[31] = '\0';
        if (args[0] != '\0') {
          int al = strlen(aliasLine);
          if (al < 30) { aliasLine[al] = ' '; aliasLine[al+1] = '\0'; }
          strncat(aliasLine, args, 31 - strlen(aliasLine));
        }
        executeCommand(aliasLine);
        resolved = 1;
        break;
      }
    }
    int eqCheck = indexOf(cmd, "="); //Check for expression in parentheses
    if (eqCheck != -1) {
      resolved = 1;
      char varName[32] = "";
      strncpy(varName, cmd, eqCheck);

      char varVal[32] = "";
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
          Serial.println(F("No space available for vars"));
        }
      }

      Serial.println(F("All current vars:"));
      for (int i = 0; i < varIndex; i++) {
        Serial.print(vars[i].name);
        Serial.print(": ");
        Serial.println(vars[i].value);
      }
    }
    if (!resolved) Serial.println(F("Unknown command."));
  }
}

// Interpreter sh
void runScript(const char* content) {
  char line[32];
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
      if (li < 31) line[li++] = c;
    }
  }
  addDmesg(F("sh: script done"));
  Serial.println(F("[sh] done."));
}
