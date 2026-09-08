/*
 *                                    TZXduino
 *                             Written and tested by
 *                          Andrew Beer, Duncan Edwards
 *                          www.facebook.com/Arduitape/
 *
 *              Designed for TZX files for Spectrum (and more later)
 *              Load TZX files onto an SD card, and play them directly
 *              without converting to WAV first!
 *
 *              Directory system allows multiple layers,  to return to root
 *              directory ensure a file titles ROOT (no extension) or by
 *              pressing the Menu Select Button.
 *
 *              Written using info from worldofspectrum.org
 *              and TZX2WAV code by Francisco Javier Crespo
 *
 *              ***************************************************************
 *              Menu System:
 *                TODO: add ORIC and ATARI tap support, clean up code, sleep
 *
 *              V1.0
 *                Motor Control Added.
 *                High compatibility with Spectrum TZX, and Tap files
 *                and CPC CDT and TZX files.
 *
 *                V1.32 Added direct loading support of AY files using the SpecAY loader
 *                to play Z80 coded files for the AY chip on any 128K or 48K with AY
 *                expansion without the need to convert AY to TAP using FILE2TAP.EXE.
 *                Download the AY loader from http://www.specay.co.uk/download
 *                and load the LOADER.TAP AY file loader on your spectrum first then
 *                simply select any AY file and just hit play to load it. A complete
 *                set of extracted and DEMO AY files can be downloaded from
 *                http://www.worldofspectrum.org/projectay/index.htm
 *                Happy listening!
 *
 *                V1.8.1 TSX support for MSX added by Natalia Pujol
 *
 *                V1.8.2 Percentage counter and timer added by Rafael Molina Chesserot along with a reworking of the OLED1306 library.
 *                Many memory usage improvements as well as a menu for TSX Baud Rates and a refined directory controls.
 *
 *                V1.8.3 PCD8544 library changed to use less memory. Bitmaps added and Menu system reduced to a more basic level.
 *                Bug fixes of the Percentage counter and timer when using motor control/
 *
 *                V1.11 Added unzipped UEF playback and turbo UEF to the Menu thatks to the kind work by kernal@kernalcrash.com
 *                Supports Gunzipped UEFs only.
 *
 *                v1.13 HQ.UEF support added by Rafael Molina Chesserot of Team MAXDuino
 *                v1.13.1 Removed digitalWrite in favour of a macro suggested by Ken Forster
 *                Some MSX games will now work at higher Baudrates than before.
 *                v1.13.2 Added a fix to some Gremlin Loaders which reverses the polarity of the block.
 *                New menu Item added. "Gremlin Loader" turned on will allow Samurai Trilogy and Footballer of the Year II
 *                CDTs to load properly.
 *
 *                1.14 ID15 code adapted from MAXDuino by Rafael Molina Chasserot.
 *                Not working 100% with CPC Music Loaders but will work with other ID15 files.
 *
 *                1.14.2 Added an ID15 switch to Menu as ID15 being enabled was stopping some files loading properly.
 *                1.14.3 Removed the switch in favour of an automatic system of switching ID15 routine on and off.
 *
 *                1.15 Added support for the Surenoo RGB Backlight LCD using an adapted version of the Grove RGBLCD library.
 *                     Second counter not currently working. Also some memory saving tweaks.
 *
 *                1.15.3 Adapted the MAXDuino ID19 code and TurboMode for ZX80/81
 *                       Also added UEF Chunk 117 which allows for differing baudrates in BBC UEFs.
 *                       Added a Spectrum Font for OLED 1306 users converted by Brendan Alford
 *                       Added File scrolling by holding up or down buttons. By Brendan Alford.
 *
 *                1.16 Fixed a bug that was stopping Head Over Heels (and probably others)loading on +2 Spectrum. Seems to have made
 *                     ZX80/81 playback alot more stable too.
 *
 *                1.17 Added ORIC TAP file playback from the Maxduino team.
 *
 *                1.18 Added a delay to IDCHUNKEOF before the stopFile(); as it was cutting off the last few milliseconds of playback on some UEF files.
 *                     Jungle Journey for Acorn Electron is now working.
 *
 *
  */

//#include "Timers.h"
#include "TZXDuino.h"

#include "SystemClock.h"
#include "DataFile.h"
#include <string.h>
DataFile entry;
Timer timer;

void digitalWrite(int, int) { }
void LowWrite() { }
void HighWrite() { }

#define bitSet(n, b)    n = (n | (1 << b))
#define bitClear(n, b)  n = (n & (~(1 << b)))
#define bitRead(n, b)   (n & (1 << b))

#define makeword(b1,b0) (((((uint16_t)b1) << 8) & 0xFF00) | (((uint16_t)b0) & 0x00FF))
#define lowByte(v)      ((int)(v & 255))
#define highByte(v)     ((int)((v >> 8) & 255))


void lcdTime();
void Counter1();
void Counter2();
int ReadByte(unsigned long pos);
int ReadWord(unsigned long pos);
int ReadLong(unsigned long pos);
int ReadDword(unsigned long pos);
void printtextF(const char* text, int l);
void printtext(char* text, int l);
void StandardBlock();
void PureToneBlock();
void PulseSequenceBlock();
void PureDataBlock();
void writeData4B();
void writeData();
void writeHeader();

#ifdef _ORIC
void OricBitWrite();
void OricDataBlock();
#endif

void playFile();
void stopFile();


void TZXSetup();
void TZXProcess();
void TZXStop();
void TZXPlay();
void TZXPause();
void TZXLoop();

void checkForEXT(char* filename);
bool checkForTap(char* filename);
bool checkForP(char* filename);
bool checkForO(char* filename);
bool checkForAY(char* filename);

bool checkForUEF(char* filename);
void ReadUEFHeader();
void writeUEFData();
void UEFCarrierToneBlock();

void ReadTZXHeader();
void ReadAYHeader();
void ZX81FilenameBlock();
void ZX8081DataBlock();
void ZX80ByteWrite();
void writeSampleData();
void DirectRecording();

void delay(int) {}

#define memcmp_P    memcpy
#define strstr_P    strstr
#define strcat_P    strcat
#define PSTR(p)     (p)
#define noInterrupts()    
#define interrupts()    



//SDTYPE sd;                          // Initialise SD card 
//FILETYPE entry, dir, tmpdir;        // SD card current file (=entry) and current directory (=dir) objects, plus one temporary

#define maxFilenameLength 255

char fileName[maxFilenameLength + 1];     //Current filename
int fileNameLen;
//byte subdir = 0;
unsigned long filesize;             // filesize used for dimensioning AY files

//#define scrollSpeed   250           //text scroll delay
//#define scrollWait    3000          //Delay before scrolling starts

//byte scrollPos = 0;                 //Stores scrolling text position
//unsigned long scrollTime = millis() + scrollWait;

#ifdef HAVE_MOTOR
bool motorState = true;                //Current motor control state
bool oldMotorState = true;             //Last motor control state
#endif

byte start = 0;                     //Currently playing flag
byte pauseOn = 0;                   //Pause state
uint16_t lastIndex = 0;             //Index of last file in current directory
bool isDir = false;                     //Is the current file a directory
unsigned long timeDiff = 0;         //button debounce
int browseDelay = 500;              // Delay between up/down navigation

char PlayBytes[17];


void setup() 
{
    TZXSetup();                                 //Setup TZX specific options
    printtextF(PSTR("Starting.."), 0);
    systemClock.Timers.add((UpdatableBase*)&timer);
    //    delay(500);
}


void loop(void) 
{
    if (start == 1)
    {
        //TZXLoop only runs if a file is playing, and keeps the buffer full.
        TZXLoop();
    }
    else {
        digitalWrite(outputPin, LOW);    //Keep output LOW while no file is playing.
    }

#ifdef HAVE_MOTOR
    motorState = button_motor();
#endif

    if (millis() - timeDiff > 50) {   // check switch every 50ms 
        timeDiff = millis();           // get current millisecond count

#ifdef HAVE_MOTOR
        if (start == 1 && (oldMotorState != motorState)) {
            //if file is playing and motor control is on then handle current motor state
            //Motor control works by pulling the btnMotor pin to ground to play, and NC to stop
            if (motorState && pauseOn == 0) {
                printtextF(PSTR("Paused "), 0);
                Counter1();

                pauseOn = 1;
            }
            if (!motorState && pauseOn == 1) {
                printtextF(PSTR("Playing"), 0);
                Counter1();
                pauseOn = 0;
            }
            oldMotorState = motorState;
        }
#endif
    }
}






void seekFile() {
    entry.close(); // precautionary, and seems harmelss if entry is already closed

    entry.setFilename("D:\\data\\tape.tzx");

    if (!entry.open(DataFile::O_RDONLY))
    {
    }


      entry.getName(fileName, maxFilenameLength);
      fileNameLen = strlen(fileName);
      filesize = entry.fileSize();
      ayblklen = filesize + 3;  // add 3 file header, data byte and chksum byte to file length
      entry.close();

      PlayBytes[0] = '\0';
      strcat_P(PlayBytes, PSTR("Select File.."));

      printtext(PlayBytes, 0);
}


void stopFile()
{
    TZXStop();
    if (start == 1) {
        printtextF(PSTR("Stopped"), 0);
        start = 0;
    }
}

void playFile() {

    if (fileName[0] == '\0')
    {
        printtextF(PSTR("No File Selected"), 1);
        //lcd_clearline(1);
        //lcd.print(F("No File Selected"));
    }
    else
    {
        printtextF(PSTR("Playing         "), 0);
        //scrollPos = 0;
        if (PauseAtStart == false) pauseOn = 0;
        //scrollText(fileName);
        currpct = 100;
        lcdsegs = 0;
        TZXPlay();
        start = 1;
        if (PauseAtStart == true) {
            printtextF(PSTR("Paused "), 0);
            pauseOn = 1;
            TZXPause();
        }
    }
}

void getMaxFile() {
    //// gets the total files in the current directory and stores the number in maxFile
    //// and also gets the file index of the last file found in this directory
}

void changeDir() {

}



void printtextF(const char* text, int) {  //Print text to screen.


#ifdef _DEBUG
    printf(text);
    printf("\n");
#endif

#ifdef SERIALSCREEN
    Serial.println(reinterpret_cast <const __FlashStringHelper*> (text));
#endif
}

void printtext(char* text, int) {  //Print text to screen.

#ifdef _DEBUG
    printf(text);
    printf("\n");
#endif

#ifdef SERIALSCREEN
    Serial.println(text);
#endif
}

void clearBuffer()
{

    for (int i = 0; i <= buffsize; i++)
    {
        wbuffer[i][0] = 0;
        wbuffer[i][1] = 0;
    }
}

word TickToUs(word ticks) {
    return (word)((((float)ticks) / 3.5) + 0.5);
}

int readfile(byte bytes, unsigned long p)
{
    int i = 0;
    if (entry.seekSet(p)) {
        i = entry.read(input, bytes);
    }
    return i;
}




void checkForEXT(char* filename) {
    if (checkForTap(filename)) {                 //Check for Tap File.  As these have no header we can skip straight to playing data
        currentTask = TZX_FILE_PROCESSID;
        currentID = TAP;
        if ((readfile(1, bytesRead)) == 1) {
            if (input[0] == 0x16) {
                currentID = ORIC;
            }
        }
        printtextF(PSTR("TAP Playing"),0);
    }
    if (checkForP(filename)) {                 //Check for P File.  As these have no header we can skip straight to playing data
        currentTask = TZX_FILE_PROCESSID;
        currentID = ZXP;
        printtextF(PSTR("ZX81 P Playing"),0);
    }
    if (checkForO(filename)) {                 //Check for O File.  As these have no header we can skip straight to playing data
        currentTask = TZX_FILE_PROCESSID;
        currentID = ZXO;
        printtextF(PSTR("ZX80 O Playing"),0);
    }
    if (checkForAY(filename)) {                 //Check for AY File.  As these have no TAP header we must create it and send AY DATA Block after
        currentTask = TZX_FILE_GETAYHEADER;
        currentID = AYO;
        AYPASS = 0;                             // Reset AY PASS flags
        hdrptr = AY_HDRSTART;                      // Start reading from position 1 -> 0x13 [0x00]
        printtextF(PSTR("AY Playing"),0);
    }

    if (checkForUEF(filename)) {                 //Check for UEF File.  As these have no TAP header we must create it and send AY DATA Block after
        currentTask = TZX_FILE_GETUEFHEADER;
        currentID = UEF;
        printtextF("UEF Playing",0);
    }
}

void TZXPlay() {
    timer.stop();                              //Stop timer interrupt

    // on entry, fileIndex is already pointing to the file entry you want to play
    // and fileName has already been set accordingly
    entry.close();
    entry.open(
        //&dir, fileIndex, 
        DataFile::O_RDONLY);

    bytesRead = 0;                                //start of file
    currentTask = TZX_FILE_GETTZXHEADER;                  //First task: search for header
    checkForEXT(fileName);
    currentBlockTask = TZX_TASK_READPARAM;               //First block task is to read in parameters
    clearBuffer();
    isStopped = false;
    pinState = LOW;                               //Always Start on a LOW output for simplicity
    count = 255;                                //End of file buffer flush
    EndOfFile = false;

    if (pinState == LOW)
    {
        LowWrite();
    }
    else
    {
        HighWrite();
    }

    timer.setPeriod(1000);                     //set 1ms wait at start of a file.
}

bool checkForTap(char* filename) {
    //Check for TAP file extensions as these have no header
    byte len = strlen(filename);
    if (strstr_P(strlwr(filename + (len - 4)), PSTR(".tap"))) {
        return true;
    }
    return false;
}

bool checkForP(char* filename) {
    //Check for TAP file extensions as these have no header
    byte len = strlen(filename);
    if (strstr_P(strlwr(filename + (len - 2)), PSTR(".p"))) {
        return true;
    }
    return false;
}

bool checkForO(char* filename) {
    //Check for TAP file extensions as these have no header
    byte len = strlen(filename);
    if (strstr_P(strlwr(filename + (len - 2)), PSTR(".o"))) {
        return true;
    }
    return false;
}

bool checkForAY(char* filename) {
    //Check for AY file extensions as these have no header
    byte len = strlen(filename);
    if (strstr_P(strlwr(filename + (len - 3)), PSTR(".ay"))) {
        return true;
    }
    return false;
}

bool checkForUEF(char* filename) {
    //Serial.println(F("checkForUEF"));
    byte len = strlen(filename);
    if (strstr_P(strlwr(filename + (len - 4)), PSTR(".uef"))) {
        return true;
    }
    return false;
}

void TZXStop() {
    timer.stop();                              //Stop timer
    isStopped = true;
    entry.close();                              //Close file
    // DEBUGGING Stuff
//lcd.setCursor(0,1);
//lcd.print(blkchksum,HEX); lcd.print("ck "); lcd.print(bytesRead); lcd.print(" "); lcd.print(ayblklen);

    bytesRead = 0;                                // reset read bytes PlayBytes
    blkchksum = 0;                              // reset block chksum byte for AY loading routine
    AYPASS = 0;                                 // reset AY flag
    ID15switch = 0;                              // ID15switch
}

void TZXPause() {
    isStopped = pauseOn;
}


void TZXLoop() 
{
    noInterrupts();                           //Pause interrupts to prevent var reads and copy values out
    copybuff = morebuff;
    morebuff = LOW;
    isStopped = pauseOn;
    interrupts();
    if (copybuff == HIGH) {
        btemppos = 0;                             //Buffer has swapped, start from the beginning of the new page
        copybuff = LOW;
    }

    if (btemppos <= buffsize)                    // Keep filling until full
    {
        TZXProcess();                           //generate the next period to add to the buffer
        if (currentPeriod > 0) {
            noInterrupts();                       //Pause interrupts while we add a period to the buffer
            wbuffer[btemppos][workingBuffer ^ 1] = currentPeriod;   //add period to the buffer
            interrupts();
            btemppos += 1;
        }
    }
    else {

        if ((pauseOn == 0) && (currpct < 100)) lcdTime();
        newpct = (100 * bytesRead) / filesize;
        if (currpct == 100) {
            currpct = 0;
            Counter2();
        }
        if ((newpct > currpct) && (newpct % 1 == 0)) {

            Counter2();

            currpct = newpct;
        }
    }
}


void TZXSetup() {
    //pinMode(outputPin, OUTPUT);               //Set output pin
    LowWrite();                               //Start output LOW
    isStopped = true;
    pinState = LOW;
    timer.initialize();
}

void TZXProcess() {
    byte r = 0;
    currentPeriod = 0;
    if (currentTask == TZX_FILE_GETTZXHEADER) {
        //grab 7 byte string
        ReadTZXHeader();
        //set current task to GETID
        currentTask = TZX_FILE_GETID;
    }
    if (currentTask == TZX_FILE_GETAYHEADER) {
        //grab 8 byte string
        ReadAYHeader();
        //set current task to PROCESSID
        currentTask = TZX_FILE_PROCESSID;
    }
    if (currentTask == TZX_FILE_GETUEFHEADER) {
        //grab 12 byte string
        ReadUEFHeader();
        //set current task to GETCHUNKID
        currentTask = TZX_FILE_GETCHUNKID;
    }
    if (currentTask == TZX_FILE_GETCHUNKID) {

        if (r = ReadWord(bytesRead) == 2) {
            chunkID = outWord;
            if (r = ReadDword(bytesRead) == 4) {
                bytesToRead = outLong;
                parity = 0;

                if (chunkID == ID0104) {
                    //bytesRead+= 3;
                    bytesToRead += -3;
                    bytesRead += 1;
                    //grab 1 byte Parity
                    if (ReadByte(bytesRead) == 1) {
                        if (outByte == 'O') parity = wibble ? 2 : 1;
                        else if (outByte == 'E') parity = wibble ? 1 : 2;
                        else parity = 0;  // 'N'
                    }
                    bytesRead += 1;
                }

            }
            else {
                chunkID = IDCHUNKEOF;
            }
        }
        else {
            chunkID = IDCHUNKEOF;
        }
        if (!uefTurboMode) {
            zeroPulse = UEFZEROPULSE;
            onePulse = UEFONEPULSE;
        }
        else {
            zeroPulse = UEFTURBOZEROPULSE;
            onePulse = UEFTURBOONEPULSE;
        }
        lastByte = 0;

        //reset data block values
        currentBit = 0;
        pass = 0;
        //set current task to PROCESSCHUNKID
        currentTask = TZX_FILE_PROCESSCHUNKID;
        currentBlockTask = TZX_TASK_READPARAM;
        UEFPASS = 0;
    }
    if (currentTask == TZX_FILE_PROCESSCHUNKID) {
        //CHUNKID Processing

        switch (chunkID) {
        case ID0000:
            bytesRead += bytesToRead;
            currentTask = TZX_FILE_GETCHUNKID;
            break;

        case ID0100:

            //bytesRead+=bytesToRead;
            writeUEFData();
            break;

        case ID0104:
            //parity = 1; // ParityOdd i.e complete with value to get Odd number of ones
            /* stopBits = */ //stopBitPulses = 1;
            writeUEFData();
            //bytesRead+=bytesToRead;
            break;

        case ID0110:
            if (currentBlockTask == TZX_TASK_READPARAM) {
                if (r = ReadWord(bytesRead) == 2) {
                    if (!uefTurboMode) {
                        pilotPulses = UEFPILOTPULSES;
                        pilotLength = UEFPILOTLENGTH;
                    }
                    else {
                        // turbo mode    
                        pilotPulses = UEFTURBOPILOTPULSES;
                        pilotLength = UEFTURBOPILOTLENGTH;

                    }
                }
                currentBlockTask = TZX_TASK_PILOT;
            }
            else {
                UEFCarrierToneBlock();
            }
            //bytesRead+=bytesToRead;
            //currentTask = GETCHUNKID;
            break;

        case ID0111:
            if (currentBlockTask == TZX_TASK_READPARAM) {
                if (r = ReadWord(bytesRead) == 2) {
                    pilotPulses = UEFPILOTPULSES; // for TURBOBAUD1500 is outWord<<2
                    pilotLength = UEFPILOTLENGTH;
                }
                currentBlockTask = TZX_TASK_PILOT;
                UEFPASS += 1;
            }
            else if (UEFPASS == 1) {
                UEFCarrierToneBlock();
                if (pilotPulses == 0) {
                    currentTask = TZX_FILE_PROCESSCHUNKID;
                    currentByte = 0xAA;
                    lastByte = 1;
                    currentBit = 10;
                    pass = 0;
                    UEFPASS = 2;
                }
            }
            else if (UEFPASS == 2) {
                parity = 0; // NoParity
                writeUEFData();
                if (currentBit == 0) {
                    currentTask = TZX_FILE_PROCESSCHUNKID;
                    currentBlockTask = TZX_TASK_READPARAM;
                }
            }
            else if (UEFPASS == 3) {
                UEFCarrierToneBlock();
            }
            break;

        case ID0112:
            //if(currentBlockTask==READPARAM){
            if (r = ReadWord(bytesRead) == 2) {
                if (outWord > 0) {
                    //Serial.print(F("delay="));
                    //Serial.println(outWord,DEC);
                    temppause = outWord;

                    currentID = IDPAUSE;
                    currentPeriod = temppause;
                    bitSet(currentPeriod, 15);
                    currentTask = TZX_FILE_GETCHUNKID;
                }
                else {
                    currentTask = TZX_FILE_GETCHUNKID;
                }
            }
            //} 
            break;

        case ID0114:
            if (r = ReadWord(bytesRead) == 2) {
                pilotPulses = UEFPILOTPULSES;
                //pilotLength = UEFPILOTLENGTH;
                bytesRead -= 2;
            }
            UEFCarrierToneBlock();
            bytesRead += bytesToRead;
            currentTask = TZX_FILE_GETCHUNKID;
            break;

        case ID0116:
            //if(currentBlockTask==READPARAM){
            if (r = ReadDword(bytesRead) == 4) {
                byte* FloatB = (byte*)&outLong;
                outWord = (((*(FloatB + 2) & 0x80) >> 7) | (*(FloatB + 3) & 0x7f) << 1) + 10;
                outWord = *FloatB | (*(FloatB + 1)) << 8 | ((outWord & 1) << 7) << 16 | (outWord >> 1) << 24;
                outFloat = *((float*)&outWord);
                outWord = (int)outFloat;

                if (outWord > 0) {
                    //Serial.print(F("delay="));
                    //Serial.println(outWord,DEC);
                    temppause = outWord;

                    currentID = IDPAUSE;
                    currentPeriod = temppause;
                    bitSet(currentPeriod, 15);
                    currentTask = TZX_FILE_GETCHUNKID;
                }
                else {
                    currentTask = TZX_FILE_GETCHUNKID;
                }
            }
            //} 
            break;

        case ID0117:
            if (r = ReadWord(bytesRead) == 2) {
                if (outWord == 300) {
                    passforZero = 8;
                    passforOne = 16;
                    currentTask = TZX_FILE_GETCHUNKID;
                }
                else {
                    passforZero = 2;
                    passforOne = 4;
                    currentTask = TZX_FILE_GETCHUNKID;
                }
            }
            break;

        case IDCHUNKEOF:
            if (!count == 0) {
                //currentPeriod = 32767;
                currentPeriod = 10;
                bitSet(currentPeriod, 15); //bitSet(currentPeriod, 12);
                count += -1;
            }
            else {
                bytesRead += bytesToRead;
                stopFile();
                return;
            }
            break;

        default:
            //Serial.print(F("Skip id "));
            //Serial.print(chunkID);
            bytesRead += bytesToRead;
            currentTask = TZX_FILE_GETCHUNKID;
            break;

        }
    }
    if (currentTask == TZX_FILE_GETID) {
        //grab 1 byte ID
        if (ReadByte(bytesRead) == 1) {
            currentID = outByte;
        }
        else {
            currentID = EOF;
        }
        //reset data block values
        currentBit = 0;
        pass = 0;
        //set current task to PROCESSID
        currentTask = TZX_FILE_PROCESSID;
        currentBlockTask = TZX_TASK_READPARAM;
    }
    if (currentTask == TZX_FILE_PROCESSID) {
        //ID Processing
        switch (currentID) {
        case ID10:
            //Process ID10 - Standard Block
            switch (currentBlockTask) {
            case TZX_TASK_READPARAM:
                if (r = ReadWord(bytesRead) == 2) {
                    pauseLength = outWord;
                }
                if (r = ReadWord(bytesRead) == 2) {
                    bytesToRead = outWord + 1;
                }
                if (r = ReadByte(bytesRead) == 1) {
                    if (outByte == 0) {
                        pilotPulses = ZXSPECTRUM_PILOTNUMBERL;
                    }
                    else {
                        pilotPulses = ZXSPECTRUM_PILOTNUMBERH;
                    }
                    bytesRead += -1;
                }
                pilotLength = ZXSPECTRUM_PILOTLENGTH;
                sync1Length = ZXSPECTRUM_SYNCFIRST;
                sync2Length = ZXSPECTRUM_SYNCSECOND;
                zeroPulse = ZXSPECTRUM_ZEROPULSE;
                onePulse = ZXSPECTRUM_ONEPULSE;
                currentBlockTask = TZX_TASK_PILOT;
                usedBitsInLastByte = 8;
                break;

            default:
                StandardBlock();
                break;
            }

            break;

        case ID11:
            //Process ID11 - Turbo Tape Block
            switch (currentBlockTask) {
            case TZX_TASK_READPARAM:
                if (r = ReadWord(bytesRead) == 2) {
                    pilotLength = TickToUs(outWord);
                }
                if (r = ReadWord(bytesRead) == 2) {
                    sync1Length = TickToUs(outWord);
                }
                if (r = ReadWord(bytesRead) == 2) {
                    sync2Length = TickToUs(outWord);
                }
                if (r = ReadWord(bytesRead) == 2) {
                    zeroPulse = TickToUs(outWord);
                }
                if (r = ReadWord(bytesRead) == 2) {
                    onePulse = TickToUs(outWord);
                }
                if (r = ReadWord(bytesRead) == 2) {
                    pilotPulses = outWord;
                }
                if (r = ReadByte(bytesRead) == 1) {
                    usedBitsInLastByte = outByte;
                }
                if (r = ReadWord(bytesRead) == 2) {
                    pauseLength = outWord;
                }
                if (r = ReadLong(bytesRead) == 3) {
                    bytesToRead = outLong + 1;
                }
                currentBlockTask = TZX_TASK_PILOT;
                break;

            default:
                StandardBlock();
                break;
            }

            break;
        case ID12:
            //Process ID12 - Pure Tone Block
            if (currentBlockTask == TZX_TASK_READPARAM) {
                if (r = ReadWord(bytesRead) == 2) {
                    pilotLength = TickToUs(outWord);
                }
                if (r = ReadWord(bytesRead) == 2) {
                    pilotPulses = outWord;
                    //DebugBlock("Pilot Pulses", pilotPulses);
                }
                currentBlockTask = TZX_TASK_PILOT;
            }
            else {
                PureToneBlock();
            }
            break;

        case ID13:
            //Process ID13 - Sequence of Pulses          
            if (currentBlockTask == TZX_TASK_READPARAM) {
                if (r = ReadByte(bytesRead) == 1) {
                    seqPulses = outByte;
                }
                currentBlockTask = TZX_TASK_DATA;
            }
            else {
                PulseSequenceBlock();
            }
            break;

        case ID14:
            //process ID14 - Pure Data Block
            if (currentBlockTask == TZX_TASK_READPARAM) {
                if (r = ReadWord(bytesRead) == 2) {
                    zeroPulse = TickToUs(outWord);
                }
                if (r = ReadWord(bytesRead) == 2) {
                    onePulse = TickToUs(outWord);
                }
                if (r = ReadByte(bytesRead) == 1) {
                    usedBitsInLastByte = outByte;
                }
                if (r = ReadWord(bytesRead) == 2) {
                    pauseLength = outWord;
                }
                if (r = ReadLong(bytesRead) == 3) {
                    bytesToRead = outLong + 1;
                }
                currentBlockTask = TZX_TASK_DATA;
            }
            else {
                PureDataBlock();
            }
            break;

        case ID15:
            //process ID15 - Direct Recording
            if (currentBlockTask == TZX_TASK_READPARAM) {
                if (r = ReadWord(bytesRead) == 2) {
                    //Number of T-states per sample (bit of data) 79 or 158 - 22.6757uS for 44.1KHz
                    TstatesperSample = TickToUs(outWord);
                }
                if (r = ReadWord(bytesRead) == 2) {
                    //Pause after this block in milliseconds
                    pauseLength = outWord;
                }
                if (r = ReadByte(bytesRead) == 1) {
                    //Used bits in last byte (other bits should be 0)
                    usedBitsInLastByte = outByte;
                }
                if (r = ReadLong(bytesRead) == 3) {
                    // Length of samples' data
                    bytesToRead = outLong + 1;
                }
                currentBlockTask = TZX_TASK_DATA;
            }
            else {
                currentPeriod = TstatesperSample;
                bitSet(currentPeriod, 14);
                DirectRecording();
            }
            break;

        case ID19:
            //Process ID19 - Generalized data block
            switch (currentBlockTask) {
            case TZX_TASK_READPARAM:

                if (r = ReadDword(bytesRead) == 4) {
                    //bytesToRead = outLong;
                }
                if (r = ReadWord(bytesRead) == 2) {
                    //Pause after this block in milliseconds
                    pauseLength = outWord;

                }
                bytesRead += 86;  // skip until DataStream filename
                //bytesToRead += -88 ;    // pauseLength + SYMDEFs
                currentBlockTask = TZX_TASK_DATA;
                break;

            case TZX_TASK_DATA:

                ZX8081DataBlock();

                break;
            }
            break;


        case ID20:
            //process ID20 - Pause Block
            if (r = ReadWord(bytesRead) == 2) {
                if (outWord > 0) {
                    temppause = outWord;
                    currentID = IDPAUSE;
                }
                else {
                    currentTask = TZX_FILE_GETID;
                }
            }
            break;

        case ID21:
            //Process ID21 - Group Start
            if (r = ReadByte(bytesRead) == 1) {
                bytesRead += outByte;
            }
            currentTask = TZX_FILE_GETID;
            break;

        case ID22:
            //Process ID22 - Group End
            currentTask = TZX_FILE_GETID;
            break;

        case ID24:
            //Process ID24 - Loop Start
            if (r = ReadWord(bytesRead) == 2) {
                loopCount = outWord;
                loopStart = bytesRead;
            }
            currentTask = TZX_FILE_GETID;
            break;

        case ID25:
            //Process ID25 - Loop End
            loopCount += -1;
            if (loopCount != 0) {
                bytesRead = loopStart;
            }
            currentTask = TZX_FILE_GETID;
            break;

        case ID2A:
            //Skip//
            bytesRead += 4;
            currentTask = TZX_FILE_GETID;
            break;

        case ID2B:
            //Skip//
            bytesRead += 5;
            currentTask = TZX_FILE_GETID;
            break;

        case ID30:
            //Process ID30 - Text Description
            if (r = ReadByte(bytesRead) == 1) {
                //Show info on screen - removed until bigger screen used
                //byte j = outByte;
                //for(byte i=0; i<j; i++) {
                //  if(ReadByte(bytesRead)==1) {
                //    lcd.print(char(outByte));
                //  }
                //}
                bytesRead += outByte;
            }
            currentTask = TZX_FILE_GETID;
            break;

        case ID31:
            //Process ID31 - Message block
            if (r = ReadByte(bytesRead) == 1) {
                // dispayTime = outByte;
            }
            if (r = ReadByte(bytesRead) == 1) {
                bytesRead += outByte;
            }
            currentTask = TZX_FILE_GETID;
            break;

        case ID32:
            //Process ID32 - Archive Info
            //Block Skipped until larger screen used
            if (ReadWord(bytesRead) == 2) {
                bytesRead += outWord;
            }
            currentTask = TZX_FILE_GETID;
            break;

        case ID33:
            //Process ID32 - Archive Info
            //Block Skipped until larger screen used
            if (ReadByte(bytesRead) == 1) {
                bytesRead += (long(outByte) * 3);
            }
            currentTask = TZX_FILE_GETID;
            break;

        case ID35:
            //Process ID35 - Custom Info Block
            //Block Skipped
            bytesRead += 0x10;
            if (r = ReadDword(bytesRead) == 4) {
                bytesRead += outLong;
            }
            currentTask = TZX_FILE_GETID;
            break;

        case ID4B:
            //Process ID4B - Kansas City Block (MSX specific implementation only)
            switch (currentBlockTask) {
            case TZX_TASK_READPARAM:
                if (r = ReadDword(bytesRead) == 4) {  // Data size to read
                    bytesToRead = outLong - 12;
                }
                if (r = ReadWord(bytesRead) == 2) {  // Pause after block in ms
                    pauseLength = outWord;
                }
                if (TSXspeedup == 0) {
                    if (r = ReadWord(bytesRead) == 2) {  // T-states each pilot pulse
                        pilotLength = TickToUs(outWord);
                    }
                    if (r = ReadWord(bytesRead) == 2) {  // Number of pilot pulses
                        pilotPulses = outWord;
                    }
                    if (r = ReadWord(bytesRead) == 2) {  // T-states 0 bit pulse
                        zeroPulse = TickToUs(outWord);
                    }
                    if (r = ReadWord(bytesRead) == 2) {  // T-states 1 bit pulse
                        onePulse = TickToUs(outWord);
                    }
                    ReadWord(bytesRead);
                }
                else {
                    //Fixed speedup baudrate, reduced pilot duration
                    pilotPulses = 10000;
                    bytesRead += 10;
                    switch (BAUDRATE) {

                    case 1200:
                        pilotLength = onePulse = TickToUs(729);
                        zeroPulse = TickToUs(1458);
                        break;

                    case 2400:
                        pilotLength = onePulse = TickToUs(365);
                        zeroPulse = TickToUs(730);
                        break;

                    case 3600:
                        pilotLength = onePulse = TickToUs(243);
                        zeroPulse = TickToUs(486);
                        break;

                    case 3760:
                        pilotLength = onePulse = TickToUs(233);
                        zeroPulse = TickToUs(466);
                        break;
                    }

                } //TSX_SPEEDUP


                currentBlockTask = TZX_TASK_PILOT;
                break;

            case TZX_TASK_PILOT:
                //Start with Pilot Pulses
                if (!pilotPulses--) {
                    currentBlockTask = TZX_TASK_DATA;
                }
                else {
                    currentPeriod = pilotLength;
                }
                break;

            case TZX_TASK_DATA:
                //Data playback
                writeData4B();
                break;

            case TZX_TASK_PAUSE:
                //Close block with a pause
                temppause = pauseLength;
                currentID = IDPAUSE;
                break;
            }
            break;

        case TAP:
            //Pure Tap file block
            switch (currentBlockTask) {
            case TZX_TASK_READPARAM:
                pauseLength = ZXSPECTRUM_PAUSELENGTH;
                if (r = ReadWord(bytesRead) == 2) {
                    bytesToRead = outWord + 1;
                }
                if (r = ReadByte(bytesRead) == 1) {
                    if (outByte == 0) {
                        pilotPulses = ZXSPECTRUM_PILOTNUMBERL + 1;
                    }
                    else {
                        pilotPulses = ZXSPECTRUM_PILOTNUMBERH + 1;
                    }
                    bytesRead += -1;
                }
                pilotLength = ZXSPECTRUM_PILOTLENGTH;
                sync1Length = ZXSPECTRUM_SYNCFIRST;
                sync2Length = ZXSPECTRUM_SYNCSECOND;
                zeroPulse = ZXSPECTRUM_ZEROPULSE;
                onePulse = ZXSPECTRUM_ONEPULSE;
                currentBlockTask = TZX_TASK_PILOT;
                usedBitsInLastByte = 8;
                break;

            default:
                StandardBlock();
                break;
            }
            break;

        case ZXP:
            switch (currentBlockTask) {
            case TZX_TASK_READPARAM:
                pauseLength = ZXSPECTRUM_PAUSELENGTH * 5;
                currentChar = 0;
                currentBlockTask = TZX_TASK_PILOT;
                break;

            case TZX_TASK_PILOT:
                ZX81FilenameBlock();
                break;

            case TZX_TASK_DATA:
                ZX8081DataBlock();
                break;
            }
            break;

        case ZXO:
            switch (currentBlockTask) {
            case TZX_TASK_READPARAM:
                pauseLength = ZXSPECTRUM_PAUSELENGTH * 5;
                currentBlockTask = TZX_TASK_DATA;
                break;

            case TZX_TASK_DATA:
                ZX8081DataBlock();
                break;

            }
            break;

        case AYO:                           //AY File - Pure AY file block - no header, must emulate it
            switch (currentBlockTask) {
            case TZX_TASK_READPARAM:
                pauseLength = ZXSPECTRUM_PAUSELENGTH;  // Standard 1 sec pause
                // here we must generate the TAP header which in pure AY files is missing.
                // This was done with a DOS utility called FILE2TAP which does not work under recent 32bit OSs (only using DOSBOX).
                // TAPed AY files begin with a standard 0x13 0x00 header (0x13 bytes to follow) and contain the 
                // name of the AY file (max 10 bytes) which we will display as "ZXAYFile " followed by the 
                // length of the block (word), checksum plus 0xFF to indicate next block is DATA.
                // 13 00[00 03(5A 58 41 59 46 49 4C 45 2E 49)1A 0B 00 C0 00 80]21<->[1C 0B FF<AYFILE>CHK]
//if(hdrptr==1) {
//bytesToRead = 0x13-2; // 0x13 0x0 - TAP Header minus 2 (FLAG and CHKSUM bytes) 17 bytes total 
//}
                if (hdrptr == AY_HDRSTART) {
                    //if (!AYPASS) {
                    pilotPulses = ZXSPECTRUM_PILOTNUMBERL + 1;
                }
                else {
                    pilotPulses = ZXSPECTRUM_PILOTNUMBERH + 1;
                }
                pilotLength = ZXSPECTRUM_PILOTLENGTH;
                sync1Length = ZXSPECTRUM_SYNCFIRST;
                sync2Length = ZXSPECTRUM_SYNCSECOND;
                zeroPulse = ZXSPECTRUM_ZEROPULSE;
                onePulse = ZXSPECTRUM_ONEPULSE;
                currentBlockTask = TZX_TASK_PILOT;    // now send pilot, SYNC1, SYNC2 and DATA (writeheader() from String Vector on 1st pass then writeData() on second)
                if (hdrptr == AY_HDRSTART) AYPASS = 1;     // Set AY TAP data read flag only if first run
                if (AYPASS == 2) {           // If we have already sent TAP header
                    blkchksum = 0;
                    bytesRead = 0;
                    bytesToRead = ayblklen + 2;   // set length of file to be read plus data byte and CHKSUM (and 2 block LEN bytes)
                    AYPASS = 5;                 // reset flag to read from file and output header 0xFF byte and end chksum
                }
                usedBitsInLastByte = 8;
                break;

            default:
                StandardBlock();
                break;
            }
            break;
#ifdef _ORIC
        case ORIC:
            //ReadByte(bytesRead);  
            //OricByteWrite();  
            switch (currentBlockTask) {
            case READPARAM: // currentBit = 0 y count = 255 
            case SYNC1:
                if (currentBit > 0) OricBitWrite();
                else {
                    //if (count >0) {      
                    ReadByte(bytesRead); currentByte = outByte; currentBit = 11; bitChecksum = 0; lastByte = 0;
                    if (currentByte == 0x16) count--;
                    else { currentBit = 0; currentBlockTask = SYNC2; } //0x24 
                    //}  
                    //else currentBlockTask=SYNC2; 
                }
                break;
            case SYNC2:
                if (currentBit > 0) OricBitWrite();
                else {
                    if (count > 0) { currentByte = 0x16; currentBit = 11; bitChecksum = 0; lastByte = 0; count--; }
                    else { count = 1; currentBlockTask = SYNCLAST; } //0x24   
                }
                break;

            case SYNCLAST:
                if (currentBit > 0) OricBitWrite();
                else {
                    if (count > 0) { currentByte = 0x24; currentBit = 11; bitChecksum = 0; lastByte = 0; count--; }
                    else { count = 9; lastByte = 0; currentBlockTask = HEADER; }
                }
                break;

            case HEADER:
                if (currentBit > 0) OricBitWrite();
                else {
                    if (count > 0) {
                        ReadByte(bytesRead); currentByte = outByte; currentBit = 11; bitChecksum = 0; lastByte = 0;
                        if (count == 5) bytesToRead = 256 * outByte;
                        else if (count == 4) bytesToRead += (outByte + 1);
                        else if (count == 3) bytesToRead -= (256 * outByte);
                        else if (count == 2) bytesToRead -= outByte;
                        count--;
                    }
                    else currentBlockTask = NAME;
                }
                break;

            case NAME:
                if (currentBit > 0) OricBitWrite();
                else {
                    ReadByte(bytesRead); currentByte = outByte; currentBit = 11; bitChecksum = 0; lastByte = 0;
                    if (currentByte == 0x00) { count = 1; currentBit = 0; currentBlockTask = NAMELAST; }
                }
                break;

            case NAMELAST:
                if (currentBit > 0) OricBitWrite();
                else {
                    if (count > 0) { currentByte = 0x00; currentBit = 11; bitChecksum = 0; lastByte = 0; count--; }
                    else { count = 100; lastByte = 0; currentBlockTask = GAP; }
                }
                break;

            case GAP:
                if (count > 0) {
                    currentPeriod = ORICONEPULSE;
                    count--;
                }
                else {
                    currentBlockTask = DATA;
                }
                break;

            case DATA:
                OricDataBlock();
                break;

            case PAUSE:
                //currentPeriod = 100; // 100ms pause 
                //bitSet(currentPeriod, 15);  
                if (!count == 0) {
                    currentPeriod = 32769;
                    count += -1;
                }
                else {
                    count = 255;
                    currentBlockTask = SYNC1;
                }
                break;
            }
            break;
#endif


        case IDPAUSE:

            if (temppause > 0) {
                if (temppause > 8300) {
                    //Serial.println(temppause, DEC);
                    currentPeriod = 8300;
                    temppause += -8300;
                }
                else {
                    currentPeriod = temppause;
                    temppause = 0;
                }
                bitSet(currentPeriod, 15);
            }
            else {
                currentTask = TZX_FILE_GETID;
                if (EndOfFile == true) currentID = EOF;
            }
            break;

        case EOF:
            //Handle end of file
            if (!count == 0) {
                currentPeriod = 32767;
                //currentPeriod = 2000;
                //bitSet(currentPeriod, 15); bitSet(currentPeriod, 12);
                count += -1;
            }
            else {
                stopFile();
                return;
            }
            break;

        default:
            //stopFile();
            //ID Not Recognised - Fall back if non TZX file or unrecognised ID occurs

#ifdef LCDSCREEN16x2
            lcd.clear();
            lcd.setCursor(0, 0);
            lcd.print("ID? ");
            lcd.setCursor(4, 0);
            lcd.print(String(currentID, HEX));
            lcd.setCursor(0, 1);
            lcd.print(String(bytesRead, HEX) + " - L: " + String(loopCount, DEC));
#endif

#ifdef RGBLCD
            lcd.clear();
            lcd.setCursor(0, 0);
            lcd.print("ID? ");
            lcd.setCursor(4, 0);
            lcd.print(String(currentID, HEX));
            lcd.setCursor(0, 1);
            lcd.print(String(bytesRead, HEX) + " - L: " + String(loopCount, DEC));
#endif

#ifdef OLED1306
            printtextF(PSTR("ID? "), 0);
            itoa(currentID, PlayBytes, 16); sendStrXY(PlayBytes, 4, 0);
            itoa(bytesRead, PlayBytes, 16); strcat_P(PlayBytes, PSTR(" - L: ")); printtext(PlayBytes, 1);
            itoa(loopCount, PlayBytes, 10); sendStrXY(PlayBytes, 10, 1);

#endif 

            delay(5000);
            stopFile();
            break;
        }
    }
}

void StandardBlock() 
{
    //Standard Block Playback
    switch (currentBlockTask) {
        case TZX_TASK_PILOT:
            //Start with Pilot Pulses
            currentPeriod = pilotLength;
            pilotPulses += -1;
            if (pilotPulses == 0) {
                currentBlockTask = TZX_TASK_SYNC1;
            }
            break;

        case TZX_TASK_SYNC1:
            //First Sync Pulse
            currentPeriod = sync1Length;
            currentBlockTask = TZX_TASK_SYNC2;
            break;

        case TZX_TASK_SYNC2:
            //Second Sync Pulse
            currentPeriod = sync2Length;
            currentBlockTask = TZX_TASK_DATA;
            break;

        case TZX_TASK_DATA:
            //Data Playback    
            if ((AYPASS == 0) || (AYPASS == 4) || (AYPASS == 5)) writeData();   // Check if we are playing from file or Vector String and we need to send first 0xFF byte or checksum byte at EOF
            else {
                writeHeader();            // write TAP Header data from String Vector (AYPASS=1)
            }
            break;

        case TZX_TASK_PAUSE:
            //Close block with a pause
              // DEBUG
              //lcd.setCursor(0,1);
              //lcd.print(blkchksum,HEX); lcd.print("ck ptr:"); lcd.print(hdrptr);


            if ((currentID != TAP) && (currentID != AYO)) {                  // Check if we have !=AYO too
                temppause = pauseLength;
                currentID = IDPAUSE;
            }
            else {
                currentPeriod = pauseLength;
                bitSet(currentPeriod, 15);
                currentBlockTask = TZX_TASK_READPARAM;

            }
            if (EndOfFile == true) currentID = EOF;
            break;
    }
}


void PureToneBlock() {
    //Pure Tone Block - Long string of pulses with the same length
    currentPeriod = pilotLength;
    pilotPulses += -1;
    if (pilotPulses == 0) {
        currentTask = TZX_FILE_GETID;
    }
}


void PulseSequenceBlock() {
    //Pulse Sequence Block - String of pulses each with a different length
    //Mainly used in speedload blocks
    byte r = 0;
    if (r = ReadWord(bytesRead) == 2) {
        currentPeriod = TickToUs(outWord);
    }
    seqPulses += -1;
    if (seqPulses == 0) {
        currentTask = TZX_FILE_GETID;
    }
}

void PureDataBlock() {
    //Pure Data Block - Data & pause only, no header, sync
    switch (currentBlockTask) {
    case TZX_TASK_DATA:
        writeData();
        break;

    case TZX_TASK_PAUSE:
        temppause = pauseLength;
        currentID = IDPAUSE;
        break;
    }
}



void writeData4B() {
    //Convert byte (4B Block) from file into string of pulses.  One pulse per pass
    byte r;
    byte dataBit;

    //Continue with current byte
    if (currentBit > 0) {

        //Start bit (0)
        if (currentBit == 11) {
            currentPeriod = zeroPulse;
            pass += 1;
            if (pass == 2) {
                currentBit += -1;
                pass = 0;
            }
        }
        else
            //Stop bits (1)
            if (currentBit <= 2) {
                currentPeriod = onePulse;
                pass += 1;
                if (pass == 4) {
                    currentBit += -1;
                    pass = 0;
                }
            }
            else
                //Data bits
            {
                dataBit = currentByte & 1;
                currentPeriod = dataBit == 1 ? onePulse : zeroPulse;
                pass += 1;
                if ((dataBit == 1 && pass == 4) || (dataBit == 0 && pass == 2)) {
                    currentByte >>= 1;
                    currentBit += -1;
                    pass = 0;
                }
            }
    }
    else
        if (currentBit == 0 && bytesToRead != 0) {
            //Read new byte
            if (r = ReadByte(bytesRead) == 1) {
                bytesToRead += -1;
                currentByte = outByte;
                currentBit = 11;
                pass = 0;
            }
            else if (r == 0) {
                //End of file
                currentID = EOF;
                return;
            }
        }

    //End of block?
    if (bytesToRead == 0 && currentBit == 0) {
        temppause = pauseLength;
        currentBlockTask = TZX_TASK_PAUSE;
    }

}

void DirectRecording() {
    //Direct Recording - Output bits based on specified sample rate (Ticks per clock) either 44.1KHz or 22.05
    switch (currentBlockTask) {
    case TZX_TASK_DATA:
        writeSampleData();
        break;

    case TZX_TASK_PAUSE:
        temppause = pauseLength;
        currentID = IDPAUSE;
        break;
    }
}



void ZX81FilenameBlock() {
    //output ZX81 filename data  byte r;
    if (currentBit == 0) {                         //Check for byte end/first byte
        currentByte=ZX81Filename[currentChar];
        //currentByte = pgm_read_byte(ZX81Filename + currentChar);
        currentChar += 1;
        if (currentChar == 10) {
            currentBlockTask = TZX_TASK_DATA;
            return;
        }
        currentBit = 9;
        pass = 0;
    }
    /*currentPeriod = ZX80PULSE;
    if(pass==1) {
      currentPeriod=ZX80BITGAP;
    }
    if(pass==0) {
      if(currentByte&0x80) {                       //Set next period depending on value of bit 0
        pass=19;
      } else {
        pass=9;
      }
      currentByte <<= 1;                        //Shift along to the next bit
      currentBit += -1;
      currentPeriod=0;
    }
    pass+=-1;*/
    ZX80ByteWrite();
}


void ZX8081DataBlock() {
    byte r;
    if (currentBit == 0) {                         //Check for byte end/first byte
        if (r = ReadByte(bytesRead) == 1) {            //Read in a byte
            currentByte = outByte;
            bytesToRead += -1;


        }
        else if (r == 0) {
            //EndOfFile=true;
            //temppause = 3000;
            temppause = pauseLength;
            currentID = IDPAUSE;
            //return;
        }
        currentBit = 9;
        pass = 0;
    }

    /*currentPeriod = ZX80PULSE;
    if(pass==1) {
      currentPeriod=ZX80BITGAP;
    }
    if(pass==0) {
      if(currentByte&0x80) {                       //Set next period depending on value of bit 0
        pass=19;
      } else {
        pass=9;
      }
      currentByte <<= 1;                        //Shift along to the next bit
      currentBit += -1;
      currentPeriod=0;
    }
    pass+=-1;*/
    ZX80ByteWrite();
}


void ZX80ByteWrite() {
    if (uefTurboMode) {
        currentPeriod = ZX80TURBOPULSE;
        if (pass == 1) {
            currentPeriod = ZX80TURBOBITGAP;
        }
    }
    else {
        currentPeriod = ZX80PULSE;
        if (pass == 1) {
            currentPeriod = ZX80BITGAP;
        }
    }
    if (pass == 0) {
        if (currentByte & 0x80) {                       //Set next period depending on value of bit 0
            pass = 19;
        }
        else {
            pass = 9;
        }
        currentByte <<= 1;                        //Shift along to the next bit
        currentBit += -1;
        currentPeriod = 0;
    }
    pass += -1;

}




void writeData() {
    //Convert byte from file into string of pulses.  One pulse per pass
    byte r;
    if (currentBit == 0) {                         //Check for byte end/first byte
        if (r = ReadByte(bytesRead) == 1) {            //Read in a byte
            currentByte = outByte;
            if (AYPASS == 5) {
                currentByte = 0xFF;                 // Only insert first DATA byte if sending AY TAP DATA Block and don't decrement counter
                AYPASS = 4;                         // set Checksum flag to be sent when EOF reached
                bytesRead += -1;                    // rollback ptr and compensate for dummy read byte
                bytesToRead += 2;                   // add 2 bytes to read as we send 0xFF (data flag header byte) and chksum at the end of the block
            }
            else {
                bytesToRead += -1;
            }
            blkchksum = blkchksum ^ currentByte;    // keep calculating checksum
            if (bytesToRead == 0) {                  //Check for end of data block
                bytesRead += -1;                      //rewind a byte if we've reached the end
                if (pauseLength == 0) {                  //Search for next ID if there is no pause
                    currentTask = TZX_FILE_GETID;
                }
                else {
                    currentBlockTask = TZX_TASK_PAUSE;           //Otherwise start the pause
                }
                return;                               // exit
            }
        }
        else if (r == 0) {                         // If we reached the EOF
            if (AYPASS != 4) {                   // Check if need to send checksum
                EndOfFile = true;
                if (pauseLength == 0) {
                    currentTask = TZX_FILE_GETID;
                }
                else {
                    currentBlockTask = TZX_TASK_PAUSE;
                }
                return;                           // return here if normal TAP or TZX
            }
            else {
                currentByte = blkchksum;            // else send calculated chksum
                bytesToRead += 1;                   // add one byte to read
                AYPASS = 0;                         // Reset flag to end block
            }
            //return;
        }
        if (bytesToRead != 1) {                      //If we're not reading the last byte play all 8 bits
            currentBit = 8;
        }
        else {
            currentBit = usedBitsInLastByte;          //Otherwise only play back the bits needed
        }
        pass = 0;
    }
    if (currentByte & 0x80) {                       //Set next period depending on value of bit 0
        currentPeriod = onePulse;
    }
    else {
        currentPeriod = zeroPulse;
    }
    pass += 1;                                    //Data is played as 2 x pulses
    if (pass == 2) {
        currentByte <<= 1;                        //Shift along to the next bit
        currentBit += -1;
        pass = 0;
    }
}


void writeHeader() {
    //Convert byte from HDR Vector String into string of pulses and calculate checksum. One pulse per pass
    if (currentBit == 0) {                         //Check for byte end/new byte                         
        if (hdrptr == 19) {              // If we've reached end of header block send checksum byte
            currentByte = blkchksum;
            AYPASS = 2;                 // set flag to Stop playing from header in RAM 
            currentBlockTask = TZX_TASK_PAUSE;   // we've finished outputting the TAP header so now PAUSE and send DATA block normally from file
            return;
        }
        hdrptr += 1;                   // increase header string vector pointer
        if (hdrptr < 20) {                     //Read a byte until we reach end of tap header
            currentByte = TAPHdr[hdrptr];
            //currentByte = pgm_read_byte(TAPHdr + hdrptr);
            if (hdrptr == 13) {                           // insert calculated block length minus LEN bytes
                currentByte = lowByte(ayblklen - 3);
            }
            else if (hdrptr == 14) {
                currentByte = highByte(ayblklen);
            }
            blkchksum = blkchksum ^ currentByte;    // Keep track of Chksum
            //}    
            //if(hdrptr<20) {               //If we're not reading the last byte play all 8 bits
            //if(bytesToRead!=1) {                      //If we're not reading the last byte play all 8 bits
            currentBit = 8;
        }
        else {
            currentBit = usedBitsInLastByte;          //Otherwise only play back the bits needed
        }
        pass = 0;
    } //End if currentBit == 0
    if (currentByte & 0x80) {                       //Set next period depending on value of bit 0
        currentPeriod = onePulse;
    }
    else {
        currentPeriod = zeroPulse;
    }
    pass += 1;                                    //Data is played as 2 x pulses
    if (pass == 2) {
        currentByte <<= 1;                        //Shift along to the next bit
        currentBit += -1;
        pass = 0;
    }
}  // End writeHeader()



void wave() 
{
    //ISR Output routine
    //unsigned long fudgeTime = micros();         //fudgeTime is used to reduce length of the next period by the time taken to process the ISR
    word workingPeriod = wbuffer[pos][workingBuffer];
    byte pauseFlipBit = false;
    unsigned long newTime = 1;
    intError = false;
    if (isStopped == 0 && workingPeriod >= 1)
    {
        if (bitRead(workingPeriod, 15))
        {
            //If bit 15 of the current period is set we're about to run a pause
            //Pauses start with a 1.5ms where the output is untouched after which the output is set LOW
            //Pause block periods are stored in milliseconds not microseconds
            isPauseBlock = true;
            bitClear(workingPeriod, 15);         //Clear pause block flag
            pinState = !pinState;
            pauseFlipBit = true;
            wasPauseBlock = true;
        }
        else {
            if (workingPeriod >= 1 && wasPauseBlock == false) {
                pinState = !pinState;
            }
            else if (wasPauseBlock == true && isPauseBlock == false) {
                wasPauseBlock = false;
            }
            //if (wasPauseBlock==true && isPauseBlock==false) wasPauseBlock=false; 
        }

        if (ID15switch == 1) {
            if (bitRead(workingPeriod, 14) == 0)
            {
                //pinState = !pinState;
                if (pinState == LOW)
                {
                    LowWrite();
                }
                else
                {
                    HighWrite();
                }
            }
            else
            {
                if (bitRead(workingPeriod, 13) == 0)
                {
                    LowWrite();
                }
                else
                {
                    HighWrite();
                    bitClear(workingPeriod, 13);
                }
                bitClear(workingPeriod, 14);         //Clear ID15 flag
                workingPeriod = TstatesperSample;
            }
        }
        else {
            //pinState = !pinState;
            if (pinState == LOW)
            {
                LowWrite();
            }
            else
            {
                HighWrite();
            }
        }

        if (pauseFlipBit == true) {
            newTime = 1500;                     //Set 1.5ms initial pause block
            //pinState = LOW;                     //Set next pinstate LOW
            if (!FlipPolarity) {
                pinState = LOW;
            }
            else {
                pinState = HIGH;
            }
            wbuffer[pos][workingBuffer] = workingPeriod - 1;  //reduce pause by 1ms as we've already pause for 1.5ms
            pauseFlipBit = false;
        }
        else {
            if (isPauseBlock == true) {
                newTime = long(workingPeriod) * 1000; //Set pause length in microseconds
                isPauseBlock = false;
            }
            else {
                newTime = workingPeriod;          //After all that, if it's not a pause block set the pulse period 
            }
            pos += 1;
            if (pos > buffsize)                  //Swap buffer pages if we've reached the end
            {
                pos = 0;
                workingBuffer ^= 1;
                morebuff = HIGH;                  //Request more data to fill inactive page
            }
        }
    }
    else if (workingPeriod <= 1 && isStopped == 0) {
        newTime = 1000;                         //Just in case we have a 0 in the buffer
        pos += 1;
        if (pos > buffsize) {
            pos = 0;
            workingBuffer ^= 1;
            morebuff = HIGH;
        }
    }
    else {
        newTime = 1000000;                         //Just in case we have a 0 in the buffer
    }
    //newTime += 12;
    //fudgeTime = micros() - fudgeTime;         //Compensate for stupidly long ISR
    //Timer.setPeriod(newTime - fudgeTime);    //Finally set the next pulse length

//    timer.setPeriod(newTime + 4);    //Finally set the next pulse length
    timer.setPeriod(newTime);    //Finally set the next pulse length
}


int ReadByte(unsigned long pos) 
{
    //Read a byte from the file, and move file position on one if successful
    byte out[1];
    int i = 0;
    if (entry.seekSet(pos)) {
        i = entry.read(out, 1);
        if (i == 1) bytesRead += 1;
    }
    outByte = out[0];
    //blkchksum = blkchksum ^ out[0];
    return i;
}

int ReadWord(unsigned long pos) {
    //Read 2 bytes from the file, and move file position on two if successful
    byte out[2] = { 0, 0 };
    int i = 0;
    if (entry.seekSet(pos)) {
        i = entry.read(out, 2);
        if (i == 2) bytesRead += 2;
    }
    outWord = makeword(out[1], out[0]);
    //blkchksum = blkchksum ^ out[0] ^ out[1];
    return i;
}

int ReadLong(unsigned long pos) {
    //Read 3 bytes from the file, and move file position on three if successful
    byte out[3] = { 0, 0, 0 };
    int i = 0;
    if (entry.seekSet(pos)) {
        i = entry.read(out, 3);
        if (i == 3) bytesRead += 3;
    }
    outLong = ((unsigned long)makeword(out[2], out[1]) << 8) | out[0];
    //outLong = (word(out[2],out[1]) << 8) | out[0];
    //blkchksum = blkchksum ^ out[0] ^ out[1] ^ out[2];
    return i;
}

int ReadDword(unsigned long pos) {
    //Read 4 bytes from the file, and move file position on four if successful  
    byte out[4] = { 0, 0, 0, 0 };
    int i = 0;
    if (entry.seekSet(pos)) {
        i = entry.read(out, 4);
        if (i == 4) bytesRead += 4;
    }
    outLong = ((unsigned long)makeword(out[3], out[2]) << 16) | makeword(out[1], out[0]);
    //outLong = (word(out[3],out[2]) << 16) | word(out[1],out[0]);
    //blkchksum = blkchksum ^ out[0] ^ out[1] ^ out[2] ^ out[3];
    return i;
}



void ReadTZXHeader() {
    //Read and check first 10 bytes for a TZX header
    char tzxHeader[11];
    int i = 0;

    if (entry.seekSet(0)) {
        i = entry.read(tzxHeader, 10);
        if (memcmp_P(tzxHeader, TZXTape, 7) != 0) {
            printtextF(PSTR("Not TZXTape"), 1);
            //lcd_clearline(1);
            //lcd.print(F("Not TZXTape"));     
            TZXStop();
        }
    }
    else {
        printtextF(PSTR("Error Reading File"), 0);
        //lcd_clearline(0);
        //lcd.print(F("Error Reading File"));        
    }
    bytesRead = 10;
}


void ReadAYHeader() {
    //Read and check first 8 bytes for a TZX header
    char ayHeader[9];
    int i = 0;

    if (entry.seekSet(0)) {
        i = entry.read(ayHeader, 8);
        if (memcmp_P(ayHeader, AYFile, 8) != 0) {
            printtextF(PSTR("Not AY File"), 1);
            //lcd_clearline(0);
            //lcd.print(F("Not AY File"));    
            TZXStop();
        }
    }
    else {
        printtextF(PSTR("Error Reading File"), 0);
        //lcd_clearline(0);
        //lcd.print(F("Error Reading File"));    
    }
    bytesRead = 0;
}


void writeSampleData() {
    //Convert byte from file into string of pulses.  One pulse per pass
    byte r;
    ID15switch = 1;
    if (currentBit == 0) {                         //Check for byte end/first byte
        if (r = ReadByte(bytesRead) == 1) {            //Read in a byte
            currentByte = outByte;
            bytesToRead += -1;
            if (bytesToRead == 0) {                  //Check for end of data block
                bytesRead += -1;                      //rewind a byte if we've reached the end
                if (pauseLength == 0) {                  //Search for next ID if there is no pause
                    currentTask = TZX_FILE_GETID;
                }
                else {
                    currentBlockTask = TZX_TASK_PAUSE;           //Otherwise start the pause
                }
                return;
            }
        }
        else if (r == 0) {
            EndOfFile = true;
            if (pauseLength == 0) {
                //ID15switch = 0;
                currentTask = TZX_FILE_GETID;
            }
            else {
                currentBlockTask = TZX_TASK_PAUSE;
            }
            return;
        }
        if (bytesToRead != 1) {                      //If we're not reading the last byte play all 8 bits
            currentBit = 8;
        }
        else {
            currentBit = usedBitsInLastByte;          //Otherwise only play back the bits needed
        }
        pass = 0;
    }
    if (bitRead(currentPeriod, 14)) {
        //bitWrite(currentPeriod,13,currentByte&0x80);
        if (currentByte & 0x80) bitSet(currentPeriod, 13);
        pass += 2;
    }
    else {
        if (currentByte & 0x80) {                       //Set next period depending on value of bit 0
            currentPeriod = onePulse;
        }
        else {
            currentPeriod = zeroPulse;
        }
        pass += 1;
    }
    if (pass == 2) {
        currentByte <<= 1;                        //Shift along to the next bit
        currentBit += -1;
        pass = 0;
    }
}



void ReadUEFHeader() {
    //Read and check first 12 bytes for a UEF header
    char uefHeader[9];
    int i = 0;

    if (entry.seekSet(0)) {
        i = entry.read(uefHeader, 9);
        if (memcmp_P(uefHeader, UEFFile, 9) != 0) {
            printtextF(PSTR("Not UEF File"), 1);
            TZXStop();
        }
    }
    else {
        printtextF(PSTR("Error Reading File"), 0);
    }
    bytesRead = 12;
}

void UEFCarrierToneBlock() {
    //Pure Tone Block - Long string of pulses with the same length
    currentPeriod = pilotLength;
    pilotPulses += -1;
    if (pilotPulses == 0) {
        currentTask = TZX_FILE_GETCHUNKID;
    }
}

void writeUEFData() {

    //Convert byte from file into string of pulses.  One pulse per pass
    byte r;
    if (currentBit == 0) {                         //Check for byte end/first byte


        if (r = ReadByte(bytesRead) == 1) {            //Read in a byte
            currentByte = outByte;
            //itoa(currentByte,PlayBytes,16); printtext(PlayBytes,lineaxy);
            bytesToRead += -1;
            bitChecksum = 0;

            //blkchksum = blkchksum ^ currentByte;    // keep calculating checksum
            if (bytesToRead == 0) {                  //Check for end of data block
                lastByte = 1;
                //Serial.println(F("  Rewind bytesRead"));
                //bytesRead += -1;                      //rewind a byte if we've reached the end
                if (pauseLength == 0) {                  //Search for next ID if there is no pause
                    currentTask = TZX_FILE_PROCESSCHUNKID;
                }
                else {
                    currentBlockTask = TZX_TASK_PAUSE;           //Otherwise start the pause
                }
                //return;                               // exit
            }
        }
        else if (r == 0) {                         // If we reached the EOF
            currentTask = TZX_FILE_GETCHUNKID;
        }

        currentBit = 11;
        pass = 0;
    }
    if ((currentBit == 2) && (parity == 0))currentBit = 1; // parity N
    if (currentBit == 11) {
        currentPeriod = zeroPulse;
    }
    else if (currentBit == 2) {
        //itoa(bitChecksum,PlayBytes,16);printtext(PlayBytes,lineaxy);
        currentPeriod = (bitChecksum ^ (parity & 0x01)) ? onePulse : zeroPulse;
        //currentPeriod =  bitChecksum ? onePulse : zeroPulse;
    }
    else if (currentBit == 1) {
        currentPeriod = onePulse;
    }
    else {
        if (currentByte & 0x01) {                       //Set next period depending on value of bit 0
            currentPeriod = onePulse;
        }
        else {
            currentPeriod = zeroPulse;
        }

    }
    pass += 1;      //Data is played as 2 x pulses for a zero, and 4 pulses for a one when speed is 1200

    if (currentPeriod == zeroPulse) {
        if (pass == passforZero) {
            if ((currentBit > 1) && (currentBit < 11)) {
                currentByte >>= 1;                        //Shift along to the next bit
            }
            currentBit += -1;
            pass = 0;
            if ((lastByte) && (currentBit == 0)) {
                currentTask = TZX_FILE_GETCHUNKID;
            }
        }
    }
    else {
        // must be a one pulse
        if (pass == passforOne) {
            if ((currentBit > 1) && (currentBit < 11)) {
                bitChecksum ^= 1;
                currentByte >>= 1;                        //Shift along to the next bit
            }

            currentBit += -1;
            pass = 0;
            if ((lastByte) && (currentBit == 0)) {
                currentTask = TZX_FILE_GETCHUNKID;
            }
        }
    }
}


void Counter1() {
#ifdef LCDSCREEN16x2            

    itoa(newpct, PlayBytes, 10);
    strcat_P(PlayBytes, PSTR("%"));
    lcd.setCursor(8, 0);
    lcd.print(PlayBytes);
    //sprintf(PlayBytes,"%03d",lcdsegs%1000);lcd.setCursor(13,0);lcd.print(PlayBytes);
    strcpy(PlayBytes, "000");
    if ((lcdsegs % 1000) < 10) itoa(lcdsegs % 10, PlayBytes + 2, 10);
    else
        if ((lcdsegs % 1000) < 100)itoa(lcdsegs % 1000, PlayBytes + 1, 10);
        else
            itoa(lcdsegs % 1000, PlayBytes, 10);

    lcd.setCursor(13, 0);
    lcd.print(PlayBytes);

#endif

#ifdef RGBLCD            


    itoa(newpct, PlayBytes, 10);
    strcat_P(PlayBytes, PSTR("%"));
    lcd.setCursor(8, 0);
    lcd.print(PlayBytes);
    /*
     //sprintf(PlayBytes,"%03d",lcdsegs%1000);lcd.setCursor(13,0);lcd.print(PlayBytes);
    strcpy(PlayBytes,"000");
    if ((lcdsegs %1000) <10) itoa(lcdsegs%10,PlayBytes+2,10);
    else
       if ((lcdsegs %1000) <100)itoa(lcdsegs%1000,PlayBytes+1,10);
       else
          itoa(lcdsegs%1000,PlayBytes,10);

    lcd.setCursor(13,0);
    lcd.print(PlayBytes);  */

#endif

#ifdef OLED1306

    itoa(newpct, PlayBytes, 10); strcat_P(PlayBytes, PSTR("%")); sendStrXY(PlayBytes, 8, 0);

    strcpy(PlayBytes, "000");
    if ((lcdsegs % 1000) < 10) itoa(lcdsegs % 10, PlayBytes + 2, 10);
    else
        if ((lcdsegs % 1000) < 100)itoa(lcdsegs % 1000, PlayBytes + 1, 10);
        else
            itoa(lcdsegs % 1000, PlayBytes, 10);

    sendStrXY(PlayBytes, 13, 0);
#endif


}


void Counter2() {

#ifdef LCDSCREEN16x2  
    lcd.setCursor(8, 0);
    lcd.print(newpct);
    lcd.print("%");

#endif

#ifdef RGBLCD  
    lcd.setCursor(8, 0);
    lcd.print(newpct);
    lcd.print("%");

#endif

#ifdef OLED1306
    if (newpct < 10) { setXY(8, 0); sendChar(48 + newpct % 10); }
    else
        if (newpct < 100) { setXY(8, 0); sendChar(48 + newpct / 10); sendChar(48 + newpct % 10); }
        else { setXY(8, 0); sendChar('1'); sendChar('0'); sendChar('0'); }

    sendChar('%');
#endif

}



//int readfile(byte bytes, unsigned long p)
//{
//
//    int i = 0;
//    int t = 0;
//    if (entry.seekSet(p)) {
//        i = entry.read(input, bytes);
//    }
//    return i;
//}

#ifdef _ORIC

void OricDataBlock() {
    //Convert byte from file into string of pulses.  One pulse per pass 
    byte r;
    if (currentBit == 0) {                         //Check for byte end/first byte 

        if (r = ReadByte(bytesRead) == 1) {            //Read in a byte  
            currentByte = outByte;
            bytesToRead += -1;
            bitChecksum = 0;
            if (bytesToRead == 0) {                  //Check for end of data block 
                lastByte = 1;
                //if(pauseLength==0) {                  //Search for next ID if there is no pause 
                  //currentTask = IDEOF;  
                //} else {  
                  //currentBlockTask = PAUSE;           //Otherwise start the pause 
                //} 
                //return;                               // exit 
            }
        }
        else if (r == 0) {                         // If we reached the EOF  
            EndOfFile = true;
            temppause = 0;
            forcePause0 = 1;
            count = 255;
            currentID = IDPAUSE;
            //currentBlockTask = GAP; 
            //currentTask = IDEOF;  

            return;
        }

        currentBit = 11;
        pass = 0;
    }
    OricBitWrite();

}



void OricBitWrite() {
    if (currentBit == 11) { //Start Bit 
        //currentPeriod = ORICZEROPULSE;  
        if (pass == 0) currentPeriod = ORICZEROLOWPULSE;
        if (pass == 1) currentPeriod = ORICZEROHIGHPULSE;
    }
    else if (currentBit == 2) { // Paridad inversa i.e. Impar 
        //currentPeriod =  bitChecksum ? ORICONEPULSE : ORICZEROPULSE;  
        if (pass == 0)  currentPeriod = bitChecksum ? ORICZEROLOWPULSE : ORICONEPULSE;
        if (pass == 1)  currentPeriod = bitChecksum ? ORICZEROHIGHPULSE : ORICONEPULSE;
    }
    else if (currentBit == 1) {
        currentPeriod = ORICONEPULSE;
    }
    else {
        if (currentByte & 0x01) {                       //Set next period depending on value of bit 0  
            currentPeriod = ORICONEPULSE;
        }
        else {
            //currentPeriod = ORICZEROPULSE;  
            if (pass == 0)  currentPeriod = ORICZEROLOWPULSE;
            if (pass == 1)  currentPeriod = ORICZEROHIGHPULSE;
        }
    }

    pass += 1;      //Data is played as 2 x pulses for a zero, and 2 pulses for a one 

    if (currentPeriod == ORICONEPULSE) {
        // must be a one pulse  

    /*    if(pass==2) {

          if ((currentBit>2) && (currentBit<11)) {
            bitChecksum ^= 1;
            currentByte >>= 1;                        //Shift along to the next bit
          }

          currentBit += -1;
          pass=0;
          if ((lastByte) && (currentBit==0)) {
            //currentTask = GETCHUNKID;
            currentBlockTask = PAUSE;
          }
        } */
        if ((currentBit > 2) && (currentBit < 11) && (pass == 2)) {
            bitChecksum ^= 1;
            currentByte >>= 1;                        //Shift along to the next bit 
            currentBit += -1;
            pass = 0;
        }
        if ((currentBit == 1) && (pass == 6)) {
            currentBit += -1;
            pass = 0;
        }
        if (((currentBit == 2) || (currentBit == 11)) && (pass == 2)) {
            currentBit += -1;
            pass = 0;
        }
        if ((currentBit == 0) && (lastByte)) {
            //currentTask = GETCHUNKID; 
            count = 255;
            currentBlockTask = PAUSE;
        }
    }
    else {
        // must be a zero pulse 
        if (pass == 2) {
            if ((currentBit > 2) && (currentBit < 11)) {
                currentByte >>= 1;                        //Shift along to the next bit 
            }
            currentBit += -1;
            pass = 0;
            if ((currentBit == 0) && (lastByte)) {
                //currentTask = GETCHUNKID;  
                count = 255;
                currentBlockTask = PAUSE;
            }
        }

    }

}
#endif

void lcdTime() {
    if (millis() - timeDiff2 > 1000) {   // check switch every second 
        timeDiff2 = millis();           // get current millisecond count

#ifdef LCDSCREEN16x2

        if (lcdsegs % 10 != 0) { itoa(lcdsegs % 10, PlayBytes, 10); lcd.setCursor(15, 0); lcd.print(PlayBytes); } // ultima cifra 1,2,3,4,5,6,7,8,9
        else
            if (lcdsegs % 100 != 0) { itoa(lcdsegs % 100, PlayBytes, 10); lcd.setCursor(14, 0); lcd.print(PlayBytes); } // es 10,20,30,40,50,60,70,80,90,110,120,..
            else
                if (lcdsegs % 1000 != 0) { itoa(lcdsegs % 1000, PlayBytes, 10); lcd.setCursor(13, 0); lcd.print(PlayBytes); } // es 100,200,300,400,500,600,700,800,900,1100,..
                else {
                    lcd.setCursor(13, 0);
                    lcd.print("000");
                } // es 000,1000,2000,...
        lcdsegs++;
#endif

#ifdef OLED1306

        if (lcdsegs % 10 != 0) { setXY(15, 0); sendChar(48 + lcdsegs % 10); } // ultima cifra 1,2,3,4,5,6,7,8,9
        else
            if (lcdsegs % 100 != 0) { setXY(14, 0); sendChar(48 + (lcdsegs % 100) / 10); sendChar('0'); } // es 10,20,30,40,50,60,70,80,90,110,120,..
            else
                if (lcdsegs % 1000 != 0) { setXY(13, 0); sendChar(48 + (lcdsegs % 1000) / 100); sendChar('0'); sendChar('0'); } // es 100,200,300,400,500,600,700,800,900,1100,..
                else { setXY(13, 0); sendChar('0'); sendChar('0'); sendChar('0'); } // es 000,1000,2000,...

        lcdsegs++;

#endif


    }
}