#define LGFX_USE_V1

#include <LovyanGFX.hpp>
#include <Wire.h>
#include <Preferences.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <Arduino.h>
struct MemoryPhoto { const uint8_t* data; size_t size; };
// Public distribution omits personal photographs from the original sketch.
// PHOTO_COUNT keeps every card on the existing text-only fallback path.
constexpr int PHOTO_COUNT = 0;
const MemoryPhoto memoryPhotos[1] = {{nullptr, 0}};

// ============================================================
// NAANI CRUSH: DESI EDITION
// Landscape Match-3 + Levels + Memory Card Collection
// ESP32-2432S022C
// ============================================================


// ============================================================
// DISPLAY
// ============================================================

class LGFX : public lgfx::LGFX_Device
{
  lgfx::Panel_ST7789 _panel;
  lgfx::Bus_Parallel8 _bus;

public:
  LGFX()
  {
    {
      auto cfg = _bus.config();

      cfg.freq_write = 25000000;

      cfg.pin_wr = 4;
      cfg.pin_rd = 2;
      cfg.pin_rs = 16;

      cfg.pin_d0 = 15;
      cfg.pin_d1 = 13;
      cfg.pin_d2 = 12;
      cfg.pin_d3 = 14;
      cfg.pin_d4 = 27;
      cfg.pin_d5 = 25;
      cfg.pin_d6 = 33;
      cfg.pin_d7 = 32;

      _bus.config(cfg);
      _panel.setBus(&_bus);
    }

    {
      auto cfg = _panel.config();

      cfg.pin_cs = 17;
      cfg.pin_rst = -1;
      cfg.pin_busy = -1;

      cfg.panel_width = 240;
      cfg.panel_height = 320;

      cfg.offset_x = 0;
      cfg.offset_y = 0;

      cfg.readable = false;
      cfg.invert = false;
      cfg.rgb_order = false;
      cfg.dlen_16bit = false;
      cfg.bus_shared = true;

      _panel.config(cfg);
    }

    setPanel(&_panel);
  }
};

LGFX lcd;


// ============================================================
// HARDWARE
// ============================================================

#define TFT_BL 0

#define TOUCH_SDA 21
#define TOUCH_SCL 22
#define CST816_ADDR 0x15


// ============================================================
// GAME CONSTANTS
// ============================================================

#define COLS 8
#define ROWS 6
#define CELL 30

#define BOARD_X 72
#define BOARD_Y 42

#define PIECE_COUNT 6

#define TOTAL_LEVELS 24
#define CARDS_PER_PAGE 8


// ============================================================
// SCREEN STATE
// ============================================================

enum ScreenState
{
  SCREEN_HOME,
  SCREEN_LEVELS,
  SCREEN_GAME,
  SCREEN_WIN_CARD,
  SCREEN_LOSE,
  SCREEN_COLLECTION,
  SCREEN_CARD_DETAIL
};

ScreenState screenState = SCREEN_HOME;


// ============================================================
// BOARD DATA
// ============================================================

uint8_t board[ROWS][COLS];

long score = 0;

int currentLevel = 1;

int movesUsed = 0;
int maxMoves = 12;

long targetScore = 150;


// ============================================================
// SAVED PROGRESS
// ============================================================


// Quiet synthesized effects through the board's GPIO26 DAC and speaker amplifier.
// Timer task plays notes independently of display drawing and match animations.
constexpr uint8_t AUDIO_PIN = 26;
struct SoundNote { uint16_t frequency; uint16_t duration; };
QueueHandle_t soundQueue = nullptr;
esp_timer_handle_t soundTimer = nullptr;
volatile bool soundEnabled = true;
const int8_t soundWave[32] = {0,3,6,9,11,13,15,16,16,16,15,13,11,9,6,3,0,-3,-6,-9,-11,-13,-15,-16,-16,-16,-15,-13,-11,-9,-6,-3};

void audioTick(void*) {
  static uint32_t left=0,total=0,phase=0,step=0;
  static bool idle=true;
  if (!soundEnabled) {
    left=0;
    SoundNote discarded;
    while(xQueueReceive(soundQueue,&discarded,0)==pdTRUE) {}
    if(!idle) { dacWrite(AUDIO_PIN,128); idle=true; }
    return;
  }
  if(!left) {
    SoundNote note;
    if(xQueueReceive(soundQueue,&note,0)!=pdTRUE) {
      if(!idle) { dacWrite(AUDIO_PIN,128); idle=true; }
      return;
    }
    left=total=(uint32_t)note.duration*8;
    phase=0; step=((uint32_t)note.frequency<<16)/8000;
    idle=false;
  }
  // Short fade at both ends avoids sharp clicks.
  uint32_t age=total-left;
  int envelope=(int)min((uint32_t)40,min(age,left));
  int sample=soundWave[(phase>>11)&31]*envelope/40;
  dacWrite(AUDIO_PIN,128+sample);
  phase=(phase+step)&0xffff; --left;
}

void queueTone(uint16_t frequency,uint16_t duration) {
  if(!soundEnabled || !soundQueue) return;
  SoundNote note={frequency,duration}; xQueueSend(soundQueue,&note,0);
}

// 0 tap, 1 swap, 2 invalid/locked, 3 match, 4 win, 5 loss, 6 flip, 7 level start.
void playSound(int effect) {
  switch(effect) {
    case 0: queueTone(1000,22); break;
    case 1: queueTone(440,35); queueTone(660,35); break;
    case 2: queueTone(220,70); break;
    case 3: queueTone(784,45); queueTone(1047,60); break;
    case 4: queueTone(523,100); queueTone(659,100); queueTone(784,100); queueTone(1047,200); break;
    case 5: queueTone(392,100); queueTone(330,100); queueTone(262,180); break;
    case 6: queueTone(880,35); queueTone(1175,45); break;
    case 7: queueTone(523,55); queueTone(784,75); break;
  }
}

void initAudio() {
  soundQueue=xQueueCreate(32,sizeof(SoundNote));
  if(!soundQueue) { Serial.println("Audio queue allocation failed"); return; }
  dacWrite(AUDIO_PIN,128);
  esp_timer_create_args_t args={};
  args.callback=audioTick; args.dispatch_method=ESP_TIMER_TASK; args.name="naaniAudio";
  args.skip_unhandled_events=true;
  if(esp_timer_create(&args,&soundTimer)!=ESP_OK) { Serial.println("Audio timer failed"); return; }
  if(esp_timer_start_periodic(soundTimer,125)!=ESP_OK) Serial.println("Audio start failed");
}

Preferences prefs;

uint32_t unlockedCards = 0;

int highestUnlockedLevel = 1;


// ============================================================
// TOUCH
// ============================================================

bool wasTouching = false;
bool touchHandled = false;

// Larger than the visible button, fully separate from the play board.
bool gameHomeHit(int x,int y) { return x>=0 && x<=68 && y>=82 && y<=166; }

uint16_t touchStartX = 0;
uint16_t touchStartY = 0;

uint16_t lastTouchX = 0;
uint16_t lastTouchY = 0;


// ============================================================
// COLLECTION
// ============================================================

int collectionPage = 0;

int selectedCard = 0;
bool cardBack = false;


// ============================================================
// COLORS
// ============================================================

uint16_t BG;
uint16_t BOARD_BG;

uint16_t CREAM;
uint16_t GOLD;
uint16_t PINK;
uint16_t ORANGE;

uint16_t GREEN;
uint16_t CYAN;
uint16_t PURPLE;
uint16_t WHITE;

uint16_t DARK_PURPLE;
uint16_t LOCKED;


// ============================================================
// MEMORY CARDS
// ============================================================

const char* cardTitles[TOTAL_LEVELS] =
{
  "Memory 01",
  "Memory 02",
  "Memory 03",
  "Memory 04",
  "Memory 05",
  "Memory 06",
  "Memory 07",
  "Memory 08",

  "Memory 09",
  "Memory 10",
  "Memory 11",
  "Memory 12",
  "Memory 13",
  "Memory 14",
  "Memory 15",
  "Memory 16",

  "Memory 17",
  "Memory 18",
  "Memory 19",
  "Memory 20",
  "Memory 21",
  "Memory 22",
  "Memory 23",
  "Memory 24"
};


const char* cardMessages[TOTAL_LEVELS] = {
"Naani, every moment with you is a little treasure.",
"Your smile makes every day feel warmer.",
"Thank you for filling our home with love.",
"Your stories are my favorite kind of magic.",
"Together is always my favorite place to be.",
"A little photo, a whole lot of love for you.",
"You make ordinary moments feel special.",
"Your hugs feel like coming home.",
"For all the laughter we have shared.",
"This moment is sweeter because of you.",
"Sending you a pocket full of happy memories.",
"Thank you for always cheering me on.",
"Your kindness stays with me wherever I go.",
"You are the heart of so many happy days.",
"May this little card bring you a big smile.",
"Every memory with you is worth keeping.",
"You make our family feel like home.",
"Here is a little reminder that you are loved.",
"For the smiles still waiting to happen.",
"Your love is the sweetest gift.",
"So grateful for you, today and always.",
"More laughter, more chai, more time together.",
"One more little treasure, just for Naani.",
"All 24 levels, and so much love for you!"
};


// ============================================================
// TOUCH READING
// ============================================================

bool readTouch(uint16_t &x, uint16_t &y)
{
  Wire.beginTransmission(CST816_ADDR);
  Wire.write(0x02);

  if (Wire.endTransmission(false) != 0)
    return false;

  Wire.requestFrom(CST816_ADDR, 5);

  if (Wire.available() < 5)
    return false;

  uint8_t fingers = Wire.read();

  uint8_t xHigh = Wire.read();
  uint8_t xLow = Wire.read();

  uint8_t yHigh = Wire.read();
  uint8_t yLow = Wire.read();

  if ((fingers & 0x0F) == 0)
    return false;

  uint16_t rawX =
    ((xHigh & 0x0F) << 8) | xLow;

  uint16_t rawY =
    ((yHigh & 0x0F) << 8) | yLow;

  x = rawY;
  y = 239 - rawX;

  if (x > 319)
    x = 319;

  if (y > 239)
    y = 239;

  return true;
}


// ============================================================
// PIECES
// ============================================================


// BARFI

void drawBarfi(int x, int y)
{
  lcd.fillRect(
    x - 9,
    y - 9,
    18,
    18,
    CREAM
  );

  lcd.drawRect(
    x - 9,
    y - 9,
    18,
    18,
    GOLD
  );

  lcd.drawLine(
    x - 6,
    y + 6,
    x + 6,
    y - 6,
    GOLD
  );

  lcd.fillCircle(
    x,
    y,
    2,
    PINK
  );
}


// LADOO

void drawLadoo(int x, int y)
{
  lcd.fillCircle(
    x,
    y,
    10,
    GOLD
  );

  lcd.fillCircle(
    x - 3,
    y - 3,
    2,
    CREAM
  );

  lcd.fillCircle(
    x + 4,
    y + 2,
    2,
    CREAM
  );

  lcd.fillCircle(
    x - 2,
    y + 5,
    1,
    CREAM
  );
}


// JALEBI

void drawJalebi(int x, int y)
{
  lcd.drawCircle(
    x,
    y,
    10,
    ORANGE
  );

  lcd.drawCircle(
    x,
    y,
    7,
    ORANGE
  );

  lcd.drawCircle(
    x,
    y,
    4,
    ORANGE
  );

  lcd.drawLine(
    x + 7,
    y,
    x + 11,
    y + 4,
    ORANGE
  );
}


// CHAI

void drawChai(int x, int y)
{
  lcd.fillRoundRect(
    x - 9,
    y - 6,
    16,
    14,
    3,
    CYAN
  );

  lcd.drawCircle(
    x + 8,
    y,
    5,
    CYAN
  );

  lcd.drawLine(
    x - 5,
    y - 9,
    x - 5,
    y - 13,
    CREAM
  );

  lcd.drawLine(
    x,
    y - 9,
    x,
    y - 14,
    CREAM
  );

  lcd.drawLine(
    x + 5,
    y - 9,
    x + 5,
    y - 13,
    CREAM
  );
}


// ELEPHANT

void drawElephant(int x, int y)
{
  uint16_t elephantColor =
    lcd.color565(170, 140, 210);

  lcd.fillEllipse(
    x,
    y + 2,
    10,
    8,
    elephantColor
  );

  lcd.fillCircle(
    x + 7,
    y - 4,
    6,
    elephantColor
  );

  lcd.fillCircle(
    x + 3,
    y - 5,
    5,
    PURPLE
  );

  lcd.drawLine(
    x + 11,
    y,
    x + 11,
    y + 8,
    elephantColor
  );

  lcd.drawLine(
    x + 11,
    y + 8,
    x + 8,
    y + 10,
    elephantColor
  );

  lcd.drawLine(
    x - 5,
    y + 7,
    x - 5,
    y + 11,
    elephantColor
  );

  lcd.drawLine(
    x + 3,
    y + 7,
    x + 3,
    y + 11,
    elephantColor
  );

  lcd.fillCircle(
    x + 9,
    y - 5,
    1,
    BG
  );
}


// PEACOCK

void drawPeacock(int x, int y)
{
  uint16_t blue =
    lcd.color565(30, 120, 210);

  uint16_t feather =
    lcd.color565(40, 190, 130);

  lcd.fillCircle(
    x - 7,
    y,
    5,
    feather
  );

  lcd.fillCircle(
    x,
    y - 6,
    5,
    feather
  );

  lcd.fillCircle(
    x + 7,
    y,
    5,
    feather
  );

  lcd.fillCircle(
    x - 4,
    y + 6,
    5,
    feather
  );

  lcd.fillCircle(
    x + 4,
    y + 6,
    5,
    feather
  );

  lcd.fillCircle(
    x - 7,
    y,
    2,
    GOLD
  );

  lcd.fillCircle(
    x,
    y - 6,
    2,
    GOLD
  );

  lcd.fillCircle(
    x + 7,
    y,
    2,
    GOLD
  );

  lcd.fillEllipse(
    x,
    y + 5,
    5,
    8,
    blue
  );

  lcd.fillCircle(
    x,
    y - 5,
    4,
    blue
  );

  lcd.fillTriangle(
    x + 4,
    y - 5,

    x + 9,
    y - 3,

    x + 4,
    y - 1,

    GOLD
  );

  lcd.fillCircle(
    x + 1,
    y - 6,
    1,
    WHITE
  );
}


// ============================================================
// DRAW PIECE
// ============================================================

void drawPiece(
  uint8_t type,
  int x,
  int y
)
{
  switch (type)
  {
    case 0:
      drawBarfi(x, y);
      break;

    case 1:
      drawLadoo(x, y);
      break;

    case 2:
      drawJalebi(x, y);
      break;

    case 3:
      drawChai(x, y);
      break;

    case 4:
      drawElephant(x, y);
      break;

    case 5:
      drawPeacock(x, y);
      break;
  }
}


// ============================================================
// LEVEL SETTINGS
// ============================================================

void loadLevelSettings()
{
  targetScore =
    150 + ((currentLevel - 1) * 35);

  maxMoves =
    12 + ((currentLevel - 1) / 4);

  if (maxMoves > 18)
    maxMoves = 18;
}


// ============================================================
// SAVE DATA
// ============================================================

void saveProgress()
{
  prefs.putUInt(
    "cards",
    unlockedCards
  );

  prefs.putUChar(
    "level",
    highestUnlockedLevel
  );
}


// ============================================================
// CARD UNLOCK
// ============================================================

bool cardUnlocked(int card)
{
  if (card < 0 || card >= TOTAL_LEVELS)
    return false;

  return
    unlockedCards &
    (1UL << card);
}


void unlockCard(int card)
{
  if (
    card < 0 ||
    card >= TOTAL_LEVELS
  )
    return;

  unlockedCards |=
    (1UL << card);

  saveProgress();
}


// ============================================================
// BOARD GENERATION
// ============================================================

bool createsImmediateMatch(
  int row,
  int col,
  uint8_t piece
)
{
  if (
    col >= 2 &&
    board[row][col - 1] == piece &&
    board[row][col - 2] == piece
  )
    return true;

  if (
    row >= 2 &&
    board[row - 1][col] == piece &&
    board[row - 2][col] == piece
  )
    return true;

  return false;
}


void generateBoard()
{
  for (int r = 0; r < ROWS; r++)
  {
    for (int c = 0; c < COLS; c++)
    {
      uint8_t piece;

      do
      {
        piece =
          random(PIECE_COUNT);
      }
      while (
        createsImmediateMatch(
          r,
          c,
          piece
        )
      );

      board[r][c] =
        piece;
    }
  }
}


// ============================================================
// GAME HEADER
// ============================================================

void drawHeader() {
  lcd.fillRect(0,0,320,38,BG);
  lcd.setTextColor(CREAM); lcd.setTextSize(1);
  lcd.setCursor(12,5); lcd.printf("LV %d",currentLevel);
  lcd.setCursor(12,20); lcd.printf("SCORE %ld/%ld",score,targetScore);
  lcd.setCursor(205,5); lcd.print("MOVES");
  lcd.setTextSize(2); lcd.setCursor(250,3); lcd.print(maxMoves-movesUsed);
  // Central left-side HOME button: 56x64 visible, 69x85 touch target.
  lcd.fillRect(0,82,69,85,BG);
  lcd.fillRoundRect(8,92,56,64,10,PURPLE);
  lcd.drawRoundRect(8,92,56,64,10,GOLD);
  lcd.setTextColor(CREAM); lcd.setTextSize(2);
  lcd.setCursor(12,117); lcd.print("HOME");
}


// ============================================================
// BOARD DRAWING
// ============================================================

void drawBoard()
{
  lcd.fillRoundRect(
    BOARD_X - 3,
    BOARD_Y - 3,

    COLS * CELL + 6,
    ROWS * CELL + 6,

    8,

    BOARD_BG
  );


  for (int r = 0; r < ROWS; r++)
  {
    for (int c = 0; c < COLS; c++)
    {
      int x =
        BOARD_X +
        c * CELL;

      int y =
        BOARD_Y +
        r * CELL;


      lcd.drawRect(
        x,
        y,
        CELL,
        CELL,
        BG
      );


      if (
        board[r][c] <
        PIECE_COUNT
      )
      {
        drawPiece(
          board[r][c],

          x + CELL / 2,
          y + CELL / 2
        );
      }
    }
  }
}


void redrawGame()
{
  lcd.fillScreen(BG);

  drawHeader();
  drawBoard();
}


// ============================================================
// MATCH DETECTION
// ============================================================

bool findMatches(
  bool matched[ROWS][COLS]
)
{
  bool any = false;


  for (int r = 0; r < ROWS; r++)
  {
    for (int c = 0; c < COLS; c++)
    {
      matched[r][c] = false;
    }
  }


  // Horizontal

  for (int r = 0; r < ROWS; r++)
  {
    int start = 0;


    while (start < COLS)
    {
      uint8_t piece =
        board[r][start];


      int end =
        start + 1;


      while (
        end < COLS &&
        board[r][end] ==
        piece
      )
      {
        end++;
      }


      if (
        piece < PIECE_COUNT &&
        end - start >= 3
      )
      {
        any = true;


        for (
          int c = start;
          c < end;
          c++
        )
        {
          matched[r][c] =
            true;
        }
      }


      start = end;
    }
  }


  // Vertical

  for (int c = 0; c < COLS; c++)
  {
    int start = 0;


    while (start < ROWS)
    {
      uint8_t piece =
        board[start][c];


      int end =
        start + 1;


      while (
        end < ROWS &&
        board[end][c] ==
        piece
      )
      {
        end++;
      }


      if (
        piece < PIECE_COUNT &&
        end - start >= 3
      )
      {
        any = true;


        for (
          int r = start;
          r < end;
          r++
        )
        {
          matched[r][c] =
            true;
        }
      }


      start = end;
    }
  }


  return any;
}


// ============================================================
// CLEAR MATCHES
// ============================================================

void clearMatches(
  bool matched[ROWS][COLS]
)
{
  playSound(3);
  int cleared = 0;


  for (int r = 0; r < ROWS; r++)
  {
    for (int c = 0; c < COLS; c++)
    {
      if (
        matched[r][c]
      )
      {
        board[r][c] =
          255;

        cleared++;
      }
    }
  }


  score +=
    cleared * 10;
}


// ============================================================
// COLLAPSE BOARD
// ============================================================

void collapseBoard()
{
  for (int c = 0; c < COLS; c++)
  {
    int writeRow =
      ROWS - 1;


    for (
      int r = ROWS - 1;
      r >= 0;
      r--
    )
    {
      if (
        board[r][c] != 255
      )
      {
        board[writeRow][c] =
          board[r][c];


        if (
          writeRow != r
        )
        {
          board[r][c] =
            255;
        }


        writeRow--;
      }
    }


    while (
      writeRow >= 0
    )
    {
      board[writeRow][c] =
        random(PIECE_COUNT);

      writeRow--;
    }
  }
}


// ============================================================
// RESOLVE MATCHES
// ============================================================

void resolveBoard()
{
  bool matched[ROWS][COLS];


  while (
    findMatches(matched)
  )
  {
    clearMatches(matched);

    redrawGame();

    delay(140);


    collapseBoard();

    redrawGame();

    delay(140);
  }
}


// ============================================================
// SWAP
// ============================================================

bool validCell(
  int r,
  int c
)
{
  return
    r >= 0 &&
    r < ROWS &&
    c >= 0 &&
    c < COLS;
}


void swapCells(
  int r1,
  int c1,
  int r2,
  int c2
)
{
  uint8_t temp =
    board[r1][c1];


  board[r1][c1] =
    board[r2][c2];


  board[r2][c2] =
    temp;
}


// ============================================================
// MEMORY CARD SCREEN
// ============================================================

void drawCardFace(int card, bool won) {
  lcd.fillScreen(BG);
  lcd.setTextSize(1); lcd.setTextColor(CREAM);
  lcd.setCursor(12,12); lcd.printf(won ? "MEMORY UNLOCKED %02d" : "MEMORY %02d",card+1);
  lcd.fillRoundRect(246,4,68,28,6,PURPLE);
  lcd.setCursor(266,14); lcd.print("BACK");
  lcd.fillRoundRect(36,34,248,168,8,CREAM);
  if (!cardBack && card < PHOTO_COUNT) {
    if (!lcd.drawJpg(memoryPhotos[card].data,memoryPhotos[card].size,40,38)) {
      lcd.setTextColor(DARK_PURPLE); lcd.setCursor(60,100); lcd.print("Photo decode error");
    }
  } else {
    lcd.setTextColor(DARK_PURPLE); lcd.setTextSize(2);
    lcd.setCursor(53,53); lcd.print(cardTitles[card]);
    lcd.setTextSize(1);
    if (!cardBack) { lcd.setCursor(53,86); lcd.print("No photo supplied for this card."); }
    String words=cardMessages[card]; String line=""; int y=cardBack ? 94 : 110;
    while (words.length()) {
      int sep=words.indexOf(' '); if(sep<0) sep=words.length();
      String word=words.substring(0,sep); words.remove(0,sep+1);
      if (lcd.textWidth(line+word)>210) { lcd.setCursor(53,y); lcd.print(line); line=""; y+=16; }
      line+=word+" ";
    }
    lcd.setCursor(53,y); lcd.print(line);
  }
  lcd.drawRoundRect(36,34,248,168,8,GOLD);
  lcd.setTextColor(CREAM); lcd.setTextSize(1);
  lcd.setCursor(8,218); lcd.print("Swipe to flip");
  if (won) {
    lcd.fillRoundRect(210,207,104,30,7,PURPLE); lcd.setCursor(241,218);
    lcd.print(currentLevel<TOTAL_LEVELS ? "NEXT" : "HOME");
  } else { lcd.setCursor(220,218); lcd.printf("%d/%d",card+1,TOTAL_LEVELS); }
}

void drawMemoryCard(int card) {
  selectedCard=card; cardBack=false; drawCardFace(card,true);
}


// ============================================================
// LEVEL COMPLETE
// ============================================================

void levelComplete()
{
  playSound(4);
  int card =
    currentLevel - 1;


  unlockCard(card);


  if (
    currentLevel <
    TOTAL_LEVELS &&
    currentLevel >=
    highestUnlockedLevel
  )
  {
    highestUnlockedLevel =
      currentLevel + 1;

    saveProgress();
  }


  screenState =
    SCREEN_WIN_CARD;


  drawMemoryCard(
    card
  );
}


// ============================================================
// LOSS SCREEN
// ============================================================

void drawLoseScreen()
{
  playSound(5);
  screenState =
    SCREEN_LOSE;


  lcd.fillScreen(
    BG
  );


  lcd.setTextColor(
    CREAM
  );


  lcd.setTextSize(3);

  lcd.setCursor(
    65,
    40
  );

  lcd.print(
    "SO CLOSE!"
  );


  lcd.setTextSize(1);

  lcd.setCursor(
    108,
    92
  );

  lcd.print(
    "Score"
  );


  lcd.setTextSize(2);

  lcd.setCursor(
    125,
    108
  );

  lcd.print(score);


  lcd.fillRoundRect(
    45,
    155,

    105,
    45,

    10,

    PURPLE
  );


  lcd.fillRoundRect(
    170,
    155,

    105,
    45,

    10,

    BOARD_BG
  );


  lcd.setTextColor(
    CREAM
  );


  lcd.setTextSize(2);


  lcd.setCursor(
    67,
    171
  );

  lcd.print(
    "RETRY"
  );


  lcd.setCursor(
    193,
    171
  );

  lcd.print(
    "HOME"
  );
}


// ============================================================
// START LEVEL
// ============================================================

void startLevel(
  int level
)
{
  playSound(7);
  currentLevel =
    level;


  score = 0;
  movesUsed = 0;


  loadLevelSettings();


  generateBoard();


  screenState =
    SCREEN_GAME;


  redrawGame();
}


// ============================================================
// ATTEMPT SWAP
// ============================================================

void attemptSwap(
  int r1,
  int c1,
  int r2,
  int c2
)
{
  if (
    !validCell(r1, c1) ||
    !validCell(r2, c2)
  )
    return;


  playSound(1);
  swapCells(
    r1,
    c1,
    r2,
    c2
  );


  bool matched[ROWS][COLS];


  if (
    findMatches(matched)
  )
  {
    movesUsed++;


    resolveBoard();


    if (
      score >=
      targetScore
    )
    {
      levelComplete();

      return;
    }


    if (
      movesUsed >=
      maxMoves
    )
    {
      drawLoseScreen();

      return;
    }


    redrawGame();
  }
  else
  {
    playSound(2);
    swapCells(
      r1,
      c1,
      r2,
      c2
    );


    redrawGame();
  }
}


// ============================================================
// TOUCH TO CELL
// ============================================================

bool screenToCell(
  uint16_t x,
  uint16_t y,
  int &row,
  int &col
)
{
  if (
    x < BOARD_X ||

    x >=
    BOARD_X +
    COLS * CELL ||

    y < BOARD_Y ||

    y >=
    BOARD_Y +
    ROWS * CELL
  )
  {
    return false;
  }


  col =
    (x - BOARD_X) /
    CELL;


  row =
    (y - BOARD_Y) /
    CELL;


  return true;
}


// ============================================================
// SWIPE
// ============================================================

void handleSwipe(
  uint16_t startX,
  uint16_t startY,

  uint16_t endX,
  uint16_t endY
)
{
  int row;
  int col;


  if (
    !screenToCell(
      startX,
      startY,
      row,
      col
    )
  )
    return;


  int dx =
    (int)endX -
    (int)startX;


  int dy =
    (int)endY -
    (int)startY;


  if (
    abs(dx) < 12 &&
    abs(dy) < 12
  )
    return;


  int targetRow =
    row;


  int targetCol =
    col;


  if (
    abs(dx) >
    abs(dy)
  )
  {
    if (dx > 0)
      targetCol++;
    else
      targetCol--;
  }
  else
  {
    if (dy > 0)
      targetRow++;
    else
      targetRow--;
  }


  attemptSwap(
    row,
    col,

    targetRow,
    targetCol
  );
}


// ============================================================
// HOME SCREEN
// ============================================================

void drawHomeScreen()
{
  screenState =
    SCREEN_HOME;


  lcd.fillScreen(
    BG
  );


  lcd.setTextColor(
    CREAM
  );


  lcd.setTextSize(3);

  lcd.setCursor(
    37,
    25
  );

  lcd.print(
    "NAANI CRUSH"
  );


  lcd.setTextSize(2);

  lcd.setCursor(
    89,
    63
  );

  lcd.print(
    "DESI EDITION"
  );


  drawBarfi(
    51,
    110
  );

  drawLadoo(
    94,
    110
  );

  drawJalebi(
    137,
    110
  );

  drawChai(
    180,
    110
  );

  drawElephant(
    223,
    110
  );

  drawPeacock(
    266,
    110
  );


  // PLAY

  lcd.fillRoundRect(
    35,
    155,

    115,
    50,

    12,

    PURPLE
  );


  lcd.setTextSize(2);

  lcd.setCursor(
    57,
    172
  );

  lcd.print(
    "LEVELS"
  );


  // CARDS

  lcd.fillRoundRect(
    170,
    155,

    115,
    50,

    12,

    BOARD_BG
  );


  lcd.setCursor(
    195,
    172
  );

  lcd.print(
    "CARDS"
  );


  lcd.setTextSize(1);

  lcd.setCursor(
    108,
    220
  );

  lcd.print(
    "Level "
  );

  lcd.print(
    highestUnlockedLevel
  );

  lcd.print(
    "/"
  );

  lcd.print(
    TOTAL_LEVELS
  );
  lcd.fillRoundRect(232,214,84,24,5,BOARD_BG);
  lcd.setTextSize(1); lcd.setTextColor(CREAM); lcd.setCursor(244,222);
  lcd.print(soundEnabled ? "SOUND ON" : "SOUND OFF");
}


// ============================================================
// COLLECTION SCREEN
// ============================================================

void drawCollection()
{
  screenState =
    SCREEN_COLLECTION;


  lcd.fillScreen(
    BG
  );


  lcd.setTextColor(
    CREAM
  );


  lcd.setTextSize(2);

  lcd.setCursor(
    12,
    9
  );

  lcd.print(
    "MEMORY CARDS"
  );


  lcd.setTextSize(1);

  lcd.setCursor(
    235,
    12
  );

  lcd.print(
    "BACK"
  );


  int firstCard =
    collectionPage *
    CARDS_PER_PAGE;


  for (
    int i = 0;
    i < CARDS_PER_PAGE;
    i++
  )
  {
    int card =
      firstCard + i;


    if (
      card >=
      TOTAL_LEVELS
    )
      break;


    int column =
      i % 4;


    int row =
      i / 4;


    int x =
      15 +
      column * 76;


    int y =
      50 +
      row * 80;


    bool unlocked =
      cardUnlocked(card);


    uint16_t boxColor =
      unlocked ?
      CREAM :
      LOCKED;


    lcd.fillRoundRect(
      x,
      y,

      62,
      65,

      7,

      boxColor
    );


    if (unlocked)
    {
      lcd.setTextColor(
        DARK_PURPLE
      );


      lcd.setTextSize(2);

      lcd.setCursor(
        x + 20,
        y + 15
      );

      lcd.print(
        card + 1
      );


      lcd.setTextSize(1);

      lcd.setCursor(
        x + 9,
        y + 43
      );

      lcd.print(
        "OPEN"
      );
    }
    else
    {
      lcd.setTextColor(
        CREAM
      );


      lcd.setTextSize(3);

      lcd.setCursor(
        x + 22,
        y + 17
      );

      lcd.print(
        "?"
      );
    }
  }


  lcd.setTextColor(
    CREAM
  );


  lcd.setTextSize(2);

  lcd.setCursor(
    72,
    216
  );

  lcd.print(
    "<"
  );


  lcd.setCursor(
    238,
    216
  );

  lcd.print(
    ">"
  );


  lcd.setTextSize(1);

  lcd.setCursor(
    145,
    219
  );

  lcd.print(
    collectionPage + 1
  );

  lcd.print(
    "/3"
  );
}


// ============================================================
// CARD DETAIL
// ============================================================

void drawCardDetail(int card) {
  selectedCard=card; cardBack=false; screenState=SCREEN_CARD_DETAIL;
  drawCardFace(card,false);
}


// ============================================================
// HOME TOUCH
// ============================================================

void drawLevels() {
  screenState=SCREEN_LEVELS;
  lcd.fillScreen(BG); lcd.setTextColor(CREAM); lcd.setTextSize(2);
  lcd.setCursor(12,8); lcd.print("CHOOSE A LEVEL");
  lcd.setTextSize(1); lcd.setCursor(12,30); lcd.print("Gold dot = completed. Dark = locked.");
  for(int i=0;i<TOTAL_LEVELS;++i) {
    int x=10+(i%6)*50,y=44+(i/6)*39;
    bool open=i+1<=highestUnlockedLevel;
    lcd.fillRoundRect(x,y,46,35,6,open ? CREAM : LOCKED);
    lcd.setTextColor(open ? DARK_PURPLE : CREAM); lcd.setTextSize(2);
    lcd.setCursor(x+(i+1<10 ? 17 : 11),y+10); lcd.print(i+1);
    if(cardUnlocked(i)) lcd.fillCircle(x+39,y+5,3,GOLD);
  }
  lcd.fillRoundRect(10,205,140,30,7,PURPLE);
  lcd.fillRoundRect(170,205,140,30,7,BOARD_BG);
  lcd.setTextColor(CREAM); lcd.setTextSize(2);
  lcd.setCursor(54,213); lcd.print("HOME");
  lcd.setCursor(210,213); lcd.print("CARDS");
}

void handleLevelsTap(int x,int y) {
  if(y>=205 && y<=235) {
    if(x>=10 && x<=150) drawHomeScreen();
    if(x>=170 && x<=310) { collectionPage=0; drawCollection(); }
    return;
  }
  for(int i=0;i<TOTAL_LEVELS;++i) {
    int bx=10+(i%6)*50,by=44+(i/6)*39;
    if(x>=bx && x<bx+46 && y>=by && y<by+35) {
      if(i+1<=highestUnlockedLevel) startLevel(i+1); else playSound(2);
      return;
    }
  }
}

void handleHomeTap(int x,int y) {
  if(x>=35 && x<=150 && y>=155 && y<=205) { drawLevels(); return; }
  if(x>=170 && x<=285 && y>=155 && y<=205) { collectionPage=0; drawCollection(); return; }
  if(x>=232 && x<=316 && y>=214 && y<=238) {
    soundEnabled=!soundEnabled; prefs.putBool("sound",soundEnabled);
    if(soundEnabled) playSound(0);
    drawHomeScreen();
  }
}


// ============================================================
// GAME HOME BUTTON
// ============================================================

void handleGameTap(int x,int y) {
  if(gameHomeHit(x,y)) { playSound(0); drawHomeScreen(); }
}


// ============================================================
// COLLECTION TOUCH
// ============================================================

void handleCollectionTap(
  int x,
  int y
)
{
  if (
    x >= 225 &&
    y <= 35
  )
  {
    drawHomeScreen();
    return;
  }


  if (
    x < 120 &&
    y > 205
  )
  {
    if (
      collectionPage > 0
    )
    {
      collectionPage--;
      drawCollection();
    }

    return;
  }


  if (
    x > 200 &&
    y > 205
  )
  {
    if (
      collectionPage < 2
    )
    {
      collectionPage++;
      drawCollection();
    }

    return;
  }


  for (
    int i = 0;
    i < CARDS_PER_PAGE;
    i++
  )
  {
    int card =
      collectionPage *
      CARDS_PER_PAGE +
      i;


    if (
      card >=
      TOTAL_LEVELS
    )
      break;


    int column =
      i % 4;


    int row =
      i / 4;


    int bx =
      15 +
      column * 76;


    int by =
      50 +
      row * 80;


    if (
      x >= bx &&
      x <= bx + 62 &&

      y >= by &&
      y <= by + 65
    )
    {
      if (
        cardUnlocked(card)
      )
      {
        drawCardDetail(card);
      }

      return;
    }
  }
}


// ============================================================
// WIN TOUCH
// ============================================================

void handleWinTap(int x,int y) {
  if(x>=246 && y<=32) { collectionPage=selectedCard/CARDS_PER_PAGE; drawCollection(); return; }
  if(x>=210 && y>=207) {
    if(currentLevel<TOTAL_LEVELS) startLevel(currentLevel+1); else drawHomeScreen();
  }
}


// ============================================================
// LOSE TOUCH
// ============================================================

void handleLoseTap(
  int x,
  int y
)
{
  if (
    x >= 45 &&
    x <= 150 &&

    y >= 155 &&
    y <= 200
  )
  {
    startLevel(
      currentLevel
    );

    return;
  }


  if (
    x >= 170 &&
    x <= 275 &&

    y >= 155 &&
    y <= 200
  )
  {
    drawHomeScreen();

    return;
  }
}


// ============================================================
// SETUP
// ============================================================

void setup()
{
  Serial.begin(
    115200
  );


  randomSeed(
    esp_random()
  );


  pinMode(
    TFT_BL,
    OUTPUT
  );


  digitalWrite(
    TFT_BL,
    HIGH
  );


  lcd.init();


  lcd.setRotation(1);


  Wire.begin(
    TOUCH_SDA,
    TOUCH_SCL
  );


  BG =
    lcd.color565(
      55,
      20,
      70
    );


  BOARD_BG =
    lcd.color565(
      88,
      45,
      100
    );


  CREAM =
    lcd.color565(
      255,
      240,
      210
    );


  GOLD =
    lcd.color565(
      244,
      184,
      50
    );


  PINK =
    lcd.color565(
      235,
      80,
      130
    );


  ORANGE =
    lcd.color565(
      255,
      110,
      20
    );


  GREEN =
    lcd.color565(
      70,
      190,
      90
    );


  CYAN =
    lcd.color565(
      85,
      190,
      210
    );


  PURPLE =
    lcd.color565(
      135,
      70,
      190
    );


  WHITE =
    lcd.color565(
      255,
      255,
      255
    );


  DARK_PURPLE =
    lcd.color565(
      55,
      20,
      70
    );


  LOCKED =
    lcd.color565(
      70,
      60,
      80
    );


  prefs.begin(
    "naani",
    false
  );


  unlockedCards =
    prefs.getUInt(
      "cards",
      0
    );


  highestUnlockedLevel =
    prefs.getUChar(
      "level",
      1
    );


  if (
    highestUnlockedLevel < 1
  )
  {
    highestUnlockedLevel = 1;
  }


  if (
    highestUnlockedLevel >
    TOTAL_LEVELS
  )
  {
    highestUnlockedLevel =
      TOTAL_LEVELS;
  }


  soundEnabled=prefs.getBool("sound",true);
  initAudio();
  drawHomeScreen();
}


// ============================================================
// MAIN LOOP
// ============================================================

void loop()
{
  uint16_t x;
  uint16_t y;


  bool touching =
    readTouch(
      x,
      y
    );


  // Finger down

  if (
    touching &&
    !wasTouching
  )
  {
    touchStartX =
      x;

    touchStartY =
      y;


    lastTouchX =
      x;

    lastTouchY =
      y;


    wasTouching = true;
    touchHandled = false;
    if(screenState==SCREEN_GAME && gameHomeHit(x,y)) {
      handleGameTap(x,y);
      touchHandled = true;
    }
  }


  // Finger moving

  if (
    touching &&
    wasTouching
  )
  {
    lastTouchX =
      x;

    lastTouchY =
      y;
  }


  // Finger released

  if (
    !touching &&
    wasTouching
  )
  {
    wasTouching = false;
    if(touchHandled) { touchHandled=false; return; }

    if(screenState!=SCREEN_GAME &&
       abs((int)lastTouchX-touchStartX)<24 && abs((int)lastTouchY-touchStartY)<24) playSound(0);

    switch (
      screenState
    )
    {

      case SCREEN_LEVELS:
        handleLevelsTap(touchStartX,touchStartY);
        break;

      case SCREEN_HOME:

        handleHomeTap(
          touchStartX,
          touchStartY
        );

        break;


      case SCREEN_GAME:

        // HOME BUTTON

        if (
          gameHomeHit(touchStartX,touchStartY)
        )
        {
          handleGameTap(
            touchStartX,
            touchStartY
          );
        }

        else
        {
          handleSwipe(
            touchStartX,
            touchStartY,

            lastTouchX,
            lastTouchY
          );
        }

        break;


      case SCREEN_WIN_CARD:
        if (abs((int)lastTouchX-touchStartX)>=24 && abs((int)lastTouchX-touchStartX)>abs((int)lastTouchY-touchStartY)) {
          playSound(6); cardBack=!cardBack; drawCardFace(selectedCard,true); break;
        }
        handleWinTap(
          touchStartX,
          touchStartY
        );

        break;


      case SCREEN_LOSE:

        handleLoseTap(
          touchStartX,
          touchStartY
        );

        break;


      case SCREEN_COLLECTION:

        handleCollectionTap(
          touchStartX,
          touchStartY
        );

        break;


      case SCREEN_CARD_DETAIL:
        if (abs((int)lastTouchX-touchStartX)>=24 && abs((int)lastTouchX-touchStartX)>abs((int)lastTouchY-touchStartY)) {
          playSound(6); cardBack=!cardBack; drawCardFace(selectedCard,false); break;
        }
        if (
          touchStartX >= 246 &&
          touchStartY <= 32
        )
        {
          drawCollection();
        }

        break;
    }


    delay(
      80
    );
  }


  delay(
    10
  );
}
