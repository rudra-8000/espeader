/*
 * ESP32-S3 N16R8 + WeAct 4.2" B/W ePaper eReader
 * 
 * Features:
 *  - WiFi AP for uploading .epub files (and web-based library management)
 *  - EPUB parsing (content.opf → spine → chapter HTML, images)
 *  - Renders text with large, readable font; renders embedded images
 *  - 4-button navigation (NEXT, PREV, SELECT, BACK)
 *  - BACK long-press → deep sleep with cover image shown on display
 *  - Resumes last book + page on wake
 *  - LittleFS for storage (16MB flash → ~14MB for books)
 * 
 * Libraries required (install via Arduino Library Manager):
 *  - GxEPD2            (ZinggJM/GxEPD2)
 *  - Adafruit_GFX      (Adafruit)
 *  - AsyncTCP          (ESP32 AsyncTCP by dvarrel / me-no-dev)
 *  - ESPAsyncWebServer (lacamera / me-no-dev)
 *  - ArduinoJson       (Benoit Blanchon) v7.x
 *  - miniz             (included via esp32 sdk, used for ZIP/EPUB extraction)
 *
 * Board: ESP32-S3 (Arduino IDE: "ESP32S3 Dev Module")
 * Partition scheme: "16M Flash (3MB APP/9MB FATFS)" or similar with large LittleFS
 *   → In Arduino IDE, set: Tools > Partition Scheme > "16M Flash (3MB APP/9.9MB FATFS)"
 *
 * Pin Wiring:
 *   EPD_MOSI  → GPIO 11  (SPI MOSI / SDA on display)
 *   EPD_SCK   → GPIO 12  (SPI SCK  / SCL on display)
 *   EPD_MISO  → GPIO 13  (not used by EPD, keep for SPI bus)
 *   EPD_CS    → GPIO 10
 *   EPD_DC    → GPIO 9
 *   EPD_RST   → GPIO 3
 *   EPD_BUSY  → GPIO 46
 *
 *   BTN_NEXT   → GPIO 4  (active LOW, internal pullup)
 *   BTN_PREV   → GPIO 5  (active LOW, internal pullup)
 *   BTN_SELECT → GPIO 6  (active LOW, internal pullup)
 *   BTN_BACK   → GPIO 7  (active LOW, internal pullup)
 *
 * NOTE on WeAct 4.2" display driver:
 *   The WeAct 4.2" B/W uses the GDEY042T81 panel (SSD1683 driver).
 *   Use GxEPD2_420_GDEY042T81 if your panel is the newer version (most units sold 2023+).
 *   If it doesn't work, try GxEPD2_420 (older GDEW042T2).
 */

// ─────────────────────────────────────────
//  INCLUDES
// ─────────────────────────────────────────
#include <Arduino.h>
#include <SPI.h>
#include <FFat.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>

// GxEPD2 display
#include <GxEPD2_BW.h>
#include <Adafruit_GFX.h>
// Large fonts from Adafruit_GFX
#include <Fonts/FreeSerifBold18pt7b.h>
#include <Fonts/FreeSerif12pt7b.h>
#include <Fonts/FreeSerif9pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>

// miniz for ZIP extraction (EPUB = ZIP)
// The ESP32 Arduino core bundles miniz; if not available, use #include "miniz.h"

// and add miniz.c to your sketch folder.
#include "miniz.h"

#include <map>
#include <vector>
#include <algorithm>
#include <functional>   // for std::function in extractEpub callback

// ─────────────────────────────────────────
//  PIN DEFINITIONS
// ─────────────────────────────────────────
#define EPD_MOSI  11
#define EPD_SCK   12
#define EPD_MISO  13
#define EPD_CS    10
#define EPD_DC     9
#define EPD_RST    3
#define EPD_BUSY  46

#define BTN_NEXT    4
#define BTN_PREV    5
#define BTN_SELECT  6
#define BTN_BACK    7

// ─────────────────────────────────────────
//  DISPLAY SETUP
// ─────────────────────────────────────────
// WeAct 4.2" B/W: 400x300, SSD1683 → GxEPD2_420_GDEY042T81
// If this doesn't work for your panel revision, try GxEPD2_420
#define DISPLAY_CLASS GxEPD2_420_GDEY042T81

GxEPD2_BW<DISPLAY_CLASS, DISPLAY_CLASS::HEIGHT> display(
  DISPLAY_CLASS(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY)
);

// ─────────────────────────────────────────
//  DISPLAY DIMENSIONS
// ─────────────────────────────────────────
#define DISPLAY_W  300  // logical width  after rotation=1 (portrait)
#define DISPLAY_H  400  // logical height after rotation=1 (portrait)

// ─────────────────────────────────────────
//  WIFI AP CONFIG
// ─────────────────────────────────────────
#define WIFI_AP_SSID   "eReader"
#define WIFI_AP_PASS   "readbooks"   // min 8 chars; set "" for open AP
#define WIFI_AP_IP     IPAddress(192, 168, 4, 1)

// ─────────────────────────────────────────
//  FILE SYSTEM PATHS
// ─────────────────────────────────────────
#define BOOKS_DIR      "/books"
#define STATE_FILE     "/state.json"
#define COVER_BMP      "/cover.bmp"   // 400x300 1-bit BMP of last book's cover

// ─────────────────────────────────────────
//  BUTTON TIMING
// ─────────────────────────────────────────
#define BTN_DEBOUNCE_MS    50
#define BTN_LONG_PRESS_MS  800

// ─────────────────────────────────────────
//  RTC MEMORY (persists through deep sleep)
// ─────────────────────────────────────────
RTC_DATA_ATTR char  rtc_book[128]  = "";  // currently open book filename
RTC_DATA_ATTR int   rtc_page       = 0;   // current page index
RTC_DATA_ATTR bool  rtc_slept      = false; // true if waking from deep sleep

// ─────────────────────────────────────────
//  STATE
// ─────────────────────────────────────────
enum AppMode {
  MODE_LIBRARY,   // showing book list
  MODE_READING,   // showing page of book
  MODE_WIFI,      // WiFi AP active for uploads
};

AppMode  appMode    = MODE_LIBRARY;

// Library
std::vector<String> bookList;
int  libCursor      = 0;   // selected book index
int  libPage        = 0;   // which page of the library list

// Reading
String  currentBook  = "";  // filename in BOOKS_DIR
int     currentPage  = 0;
int     totalPages   = 0;

// Text pages extracted from current EPUB chapter
std::vector<String> pageLines;  // each element is one screen's worth of text

AsyncWebServer server(80);
bool wifiActive = false;

// ─────────────────────────────────────────────────────────────────────────────
//  FORWARD DECLARATIONS
// ─────────────────────────────────────────────────────────────────────────────
void loadState();
void saveState();
void scanBooks();
void drawLibrary();
void drawReadingPage();
void openBook(const String& filename);
// bool extractEpub(const String& filename, std::vector<String>& outSpine, String& coverPath);
bool extractEpub(const String& epubPath, String& coverPath,
                 std::function<void(int,int,const String&)> chapterCb);
String extractTextFromHtml(const String& html);
void paginateReset();
void paginateAppend(const String& text);
void paginateFlushAll();
void drawStatusBar(const String& left, const String& right);
void drawCenteredText(const String& text, int y, const GFXfont* font);
void startWifi();
void stopWifi();
void handleButtons();
void goToSleep();
void displayCoverForSleep();
void renderBmpFromFlash(const String& path, int x, int y);

// ─────────────────────────────────────────────────────────────────────────────
//  SETUP
// ─────────────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);

  // Button pins
  pinMode(BTN_NEXT,   INPUT_PULLUP);
  pinMode(BTN_PREV,   INPUT_PULLUP);
  pinMode(BTN_SELECT, INPUT_PULLUP);
  pinMode(BTN_BACK,   INPUT_PULLUP);

  // Init SPI for EPD on custom pins
  SPI.begin(EPD_SCK, EPD_MISO, EPD_MOSI, EPD_CS);

  // Init display
  display.init(115200, true, 2, false);
  display.setRotation(1);  // landscape: 0 or 2
  display.setTextWrap(false);

  // Init LittleFS
  // FFat matches the "FATFS" partition in "16M Flash (3MB APP/9.9MB FATFS)" scheme
  if (!FFat.begin(true)) {
    Serial.println("FFat mount failed — formatting...");
    FFat.format();
    if (!FFat.begin(true)) {
      Serial.println("FFat mount failed even after format. Check partition scheme.");
    }
  }

  // Ensure books directory exists
  if (!FFat.exists(BOOKS_DIR)) {
    FFat.mkdir(BOOKS_DIR);
  }

  // Restore state from RTC or JSON
  loadState();

  // If we just woke from sleep, just go to reading mode
  if (rtc_slept) {
    rtc_slept = false;
    currentBook = String(rtc_book);
    currentPage = rtc_page;
    if (currentBook.length() > 0) {
      appMode = MODE_READING;
      openBook(currentBook);
    } else {
      appMode = MODE_LIBRARY;
    }
  }

  scanBooks();

  // Initial draw
  if (appMode == MODE_READING && currentBook.length() > 0) {
    drawReadingPage();
  } else {
    appMode = MODE_LIBRARY;
    drawLibrary();
  }
}

// ─────────────────────────────────────────────────────────────────────────────
//  LOOP
// ─────────────────────────────────────────────────────────────────────────────
void loop() {
  handleButtons();
  delay(10);
}

// ─────────────────────────────────────────────────────────────────────────────
//  STATE PERSISTENCE
// ─────────────────────────────────────────────────────────────────────────────
void loadState() {
  if (!FFat.exists(STATE_FILE)) return;
  File f = FFat.open(STATE_FILE, "r");
  if (!f) return;
  JsonDocument doc;
  deserializeJson(doc, f);
  f.close();
  currentBook = doc["book"] | String("");
  currentPage = doc["page"] | 0;
  libCursor   = doc["libCursor"] | 0;
  if (currentBook.length() > 0) {
    strlcpy(rtc_book, currentBook.c_str(), sizeof(rtc_book));
    rtc_page = currentPage;
    appMode = MODE_READING;
  }
}

void saveState() {
  File f = FFat.open(STATE_FILE, "w");
  if (!f) return;
  JsonDocument doc;
  doc["book"]      = currentBook;
  doc["page"]      = currentPage;
  doc["libCursor"] = libCursor;
  serializeJson(doc, f);
  f.close();
  strlcpy(rtc_book, currentBook.c_str(), sizeof(rtc_book));
  rtc_page = currentPage;
}

// ─────────────────────────────────────────────────────────────────────────────
//  BOOK SCANNING
// ─────────────────────────────────────────────────────────────────────────────
void scanBooks() {
  bookList.clear();
  File dir = FFat.open(BOOKS_DIR);
  if (!dir || !dir.isDirectory()) return;
  File entry;
  while ((entry = dir.openNextFile())) {
    String name = entry.name();
    if (name.endsWith(".epub")) {
      bookList.push_back(name);
    }
    entry.close();
  }
  dir.close();
  // Sort alphabetically
  std::sort(bookList.begin(), bookList.end());
}

// ─────────────────────────────────────────────────────────────────────────────
//  LIBRARY SCREEN   (shows ~5 books per page)
// ─────────────────────────────────────────────────────────────────────────────
#define LIB_BOOKS_PER_PAGE  6   // portrait 400px tall fits 6 books
#define LIB_LINE_HEIGHT    50
#define LIB_TOP_Y          62
#define LIB_LEFT_X         16

void drawLibrary() {
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);

    // Header
    display.setFont(&FreeSerifBold18pt7b);
    display.setTextColor(GxEPD_BLACK);
    drawCenteredText("My Library", 35, &FreeSerifBold18pt7b);

    // Divider
    display.drawFastHLine(10, 46, DISPLAY_W - 20, GxEPD_BLACK);

    if (bookList.empty()) {
      display.setFont(&FreeSerif12pt7b);
      drawCenteredText("No books found.", DISPLAY_H / 2, &FreeSerif12pt7b);
      display.setFont(&FreeSerif9pt7b);
      drawCenteredText("Press SELECT to enable WiFi", DISPLAY_H / 2 + 30, &FreeSerif9pt7b);
      drawCenteredText("and upload EPUBs.", DISPLAY_H / 2 + 52, &FreeSerif9pt7b);
    } else {
      int startIdx = libPage * LIB_BOOKS_PER_PAGE;
      int endIdx   = min((int)bookList.size(), startIdx + LIB_BOOKS_PER_PAGE);

      for (int i = startIdx; i < endIdx; i++) {
        int row = i - startIdx;
        int y   = LIB_TOP_Y + row * LIB_LINE_HEIGHT;
        bool selected = (i == libCursor);

        if (selected) {
          // Highlight bar
          display.fillRect(8, y - 22, DISPLAY_W - 16, LIB_LINE_HEIGHT - 4, GxEPD_BLACK);
          display.setTextColor(GxEPD_WHITE);
        } else {
          display.setTextColor(GxEPD_BLACK);
        }

        // Strip .epub suffix for display
        String title = bookList[i];
        title.replace(".epub", "");
        // Truncate if too long
        if (title.length() > 30) title = title.substring(0, 28) + "..";

        display.setFont(&FreeSerif12pt7b);
        display.setCursor(LIB_LEFT_X + (selected ? 8 : 4), y + 8);
        display.print(title);

        // Reset text color
        display.setTextColor(GxEPD_BLACK);
      }

      // Footer: page indicator and controls hint
      drawStatusBar(
        String(libCursor + 1) + "/" + String(bookList.size()),
        "SEL=Open  BACK=WiFi"
      );
    }
  } while (display.nextPage());
}

// ─────────────────────────────────────────────────────────────────────────────
//  READING SCREEN
// ─────────────────────────────────────────────────────────────────────────────
#define READ_TOP_Y       50  // text starts below header  (portrait: 300x400)
#define READ_BOTTOM_Y   370  // text ends above footer
#define READ_LEFT_X      12
#define READ_RIGHT_X    288  // portrait width=300, margin 12px each side
#define READ_LINE_H      24  // pixels per line at FreeSerif12pt7b
#define READ_LINES_MAX   13  // (370-50)/24 = 13 lines fit in portrait

void drawReadingPage() {
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setTextColor(GxEPD_BLACK);

    // ── Header: book title truncated ──
    display.setFont(&FreeSansBold9pt7b);
    String hdr = currentBook;
    hdr.replace(".epub", "");
    if (hdr.length() > 32) hdr = hdr.substring(0, 30) + "..";
    display.setCursor(READ_LEFT_X, 20);
    display.print(hdr);
    display.drawFastHLine(10, 28, DISPLAY_W - 20, GxEPD_BLACK);

    // ── Body text ──
    if (currentPage >= 0 && currentPage < (int)pageLines.size()) {
      display.setFont(&FreeSerif12pt7b);
      // Each entry in pageLines is a full page of text (newline-separated)
      String pg = pageLines[currentPage];
      int lineY = READ_TOP_Y;
      int start = 0;
      while (start < (int)pg.length() && lineY < READ_BOTTOM_Y) {
        int nl = pg.indexOf('\n', start);
        String line = (nl == -1) ? pg.substring(start) : pg.substring(start, nl);
        display.setCursor(READ_LEFT_X, lineY);
        display.print(line);
        lineY += READ_LINE_H;
        if (nl == -1) break;
        start = nl + 1;
      }
    } else {
      // Blank / end
      display.setFont(&FreeSerif12pt7b);
      drawCenteredText("End of book.", DISPLAY_H / 2, &FreeSerif12pt7b);
    }

    // ── Footer ──
    display.drawFastHLine(10, 378, DISPLAY_W - 20, GxEPD_BLACK);
    drawStatusBar(
      "Page " + String(currentPage + 1) + "/" + String(pageLines.size()),
      "< PREV  NEXT >  BACK=Menu"
    );
  } while (display.nextPage());

  saveState();
}

// // ─────────────────────────────────────────────────────────────────────────────
// //  OPEN BOOK  – extracts EPUB, builds pageLines
// // ─────────────────────────────────────────────────────────────────────────────
// void openBook(const String& filename) {
//   currentBook = filename;
//   currentPage = 0;
//   pageLines.clear();
//   paginateReset();

//   String fullPath = String(BOOKS_DIR) + "/" + filename;
//   Serial.println("Opening: " + fullPath);
//   Serial.printf("Free heap before open: %u bytes\n", ESP.getFreeHeap());
//   Serial.printf("Free PSRAM: %u bytes\n", ESP.getFreePsram());

//   std::vector<String> spine;
//   String coverPath;
//   if (!extractEpub(fullPath, spine, coverPath)) {
//     pageLines.push_back("Error: could not parse EPUB.\nCheck Serial for details.");
//     return;
//   }

//   // ── Process spine items one at a time to avoid one giant String ──
//   // Each chapter is HTML-stripped then fed into the paginator incrementally.
//   // The paginator accumulates pageLines as it goes.
//   for (size_t i = 0; i < spine.size(); i++) {
//     // Show progress on display for long books
//     if (i % 5 == 0) {
//       display.setFullWindow();
//       display.firstPage();
//       do {
//         display.fillScreen(GxEPD_WHITE);
//         display.setFont(&FreeSerif12pt7b);
//         display.setTextColor(GxEPD_BLACK);
//         String msg = "Loading... " + String((int)((i * 100) / spine.size())) + "%";
//         drawCenteredText(msg, DISPLAY_H / 2, &FreeSerif12pt7b);
//         String pg = String(i + 1) + "/" + String(spine.size()) + " chapters";
//         drawCenteredText(pg, DISPLAY_H / 2 + 30, &FreeSerif9pt7b);
//       } while (display.nextPage());
//     }

//     String chapterText = extractTextFromHtml(spine[i]);
//     spine[i] = "";  // free immediately after use
//     paginateAppend(chapterText);  // append pages incrementally
//     chapterText = "";

//     Serial.printf("Chapter %d done, heap: %u, pages so far: %d\n",
//                   (int)i, ESP.getFreeHeap(), (int)pageLines.size());
//     yield();
//   }

//   paginateFlushAll();  // flush last partial page
//   totalPages = pageLines.size();
//   Serial.printf("Done: %d pages, heap: %u\n", totalPages, ESP.getFreeHeap());

//   strlcpy(rtc_book, filename.c_str(), sizeof(rtc_book));
//   rtc_page = 0;
// }

// // ─────────────────────────────────────────────────────────────────────────────
// //  EPUB EXTRACTION
// //  Opens ZIP (EPUB), parses OPF manifest for spine order, extracts each
// //  XHTML chapter. Saves cover image to /cover_raw.(jpg|png).
// // ─────────────────────────────────────────────────────────────────────────────
// bool extractEpub(const String& epubPath, std::vector<String>& outSpine, String& coverPath) {
//   File epubFile = FFat.open(epubPath, "r");
//   if (!epubFile) { Serial.println("Cannot open epub"); return false; }

//   size_t fileSize = epubFile.size();
//   Serial.printf("EPUB size: %u bytes\n", fileSize);

//   uint8_t* buf = (uint8_t*)ps_malloc(fileSize);
//   if (!buf) {
//     // fallback to regular heap for small files
//     buf = (uint8_t*)malloc(fileSize);
//     if (!buf) { Serial.println("malloc failed for epub"); epubFile.close(); return false; }
//   }
//   epubFile.read(buf, fileSize);
//   epubFile.close();

//   mz_zip_archive zip;
//   memset(&zip, 0, sizeof(zip));
//   if (!mz_zip_reader_init_mem(&zip, buf, fileSize, 0)) {
//     Serial.println("miniz: failed to open zip");
//     free(buf); return false;
//   }

//   int numFiles = (int)mz_zip_reader_get_num_files(&zip);
//   Serial.printf("ZIP entries: %d\n", numFiles);

//   // ── 1. Find OPF path via container.xml ──
//   String opfPath = "";
//   {
//     size_t sz = 0;
//     void* data = mz_zip_reader_extract_file_to_heap(&zip, "META-INF/container.xml", &sz, 0);
//     if (data) {
//       String container((char*)data, sz);
//       mz_free(data);
//       int s = container.indexOf("full-path=\"");
//       if (s != -1) { s += 11; opfPath = container.substring(s, container.indexOf("\"", s)); }
//     }
//   }
//   if (opfPath.length() == 0) {
//     // Fallback: scan for .opf
//     for (int i = 0; i < numFiles; i++) {
//       mz_zip_archive_file_stat st; mz_zip_reader_file_stat(&zip, i, &st);
//       String fn = st.m_filename;
//       if (fn.endsWith(".opf")) { opfPath = fn; break; }
//     }
//   }
//   if (opfPath.length() == 0) { Serial.println("No OPF found"); mz_zip_reader_end(&zip); free(buf); return false; }
//   Serial.println("OPF: " + opfPath);

//   String opfDir = "";
//   int ls = opfPath.lastIndexOf('/');
//   if (ls != -1) opfDir = opfPath.substring(0, ls + 1);

//   // ── 2. Parse OPF manifest + spine ──
//   String opfContent = "";
//   {
//     size_t sz = 0;
//     void* data = mz_zip_reader_extract_file_to_heap(&zip, opfPath.c_str(), &sz, 0);
//     if (data) { opfContent = String((char*)data, sz); mz_free(data); }
//   }

//   std::map<String, String> manifest;
//   {
//     int pos = 0;
//     while (true) {
//       int is = opfContent.indexOf("<item ", pos);
//       if (is == -1) break;
//       int ie = opfContent.indexOf(">", is);
//       String tag = opfContent.substring(is, ie + 1);

//       auto attr = [&](const char* name) -> String {
//         String key = String(name) + "=\"";
//         int a = tag.indexOf(key);
//         if (a == -1) return "";
//         a += key.length();
//         return tag.substring(a, tag.indexOf("\"", a));
//       };

//       String id    = attr("id");
//       String href  = attr("href");
//       String mtype = attr("media-type");
//       String props = attr("properties");

//       if (id.length() && href.length()) manifest[id] = href;

//       if (coverPath.length() == 0 &&
//           (props.indexOf("cover-image") != -1 || id.indexOf("cover") != -1) &&
//           mtype.indexOf("image") != -1) {
//         coverPath = opfDir + href;
//       }
//       pos = ie + 1;
//     }
//   }
//   opfContent = "";  // free it

//   // ── 3. Extract cover image ──
//   if (coverPath.length() > 0) {
//     size_t sz = 0;
//     void* imgData = mz_zip_reader_extract_file_to_heap(&zip, coverPath.c_str(), &sz, 0);
//     if (imgData) {
//       String ext = coverPath.substring(coverPath.lastIndexOf('.'));
//       String savePath = "/cover_raw" + ext;
//       File cf = FFat.open(savePath, "w");
//       if (cf) { cf.write((uint8_t*)imgData, sz); cf.close(); coverPath = savePath; }
//       mz_free(imgData);
//     }
//   }

//   // ── 4. Build spine order ──
//   std::vector<String> spineIds;
//   {
//     int pos = opfContent.length() > 0 ? opfContent.indexOf("<spine") : 0;
//     // Re-extract OPF just for spine (opfContent was freed above — use zip again)
//     size_t sz2 = 0;
//     void* opfData2 = mz_zip_reader_extract_file_to_heap(&zip, opfPath.c_str(), &sz2, 0);
//     if (opfData2) {
//       String opf2((char*)opfData2, sz2);
//       mz_free(opfData2);
//       pos = 0;
//       while (true) {
//         int ir = opf2.indexOf("<itemref ", pos);
//         if (ir == -1) break;
//         int ie = opf2.indexOf(">", ir);
//         String tag = opf2.substring(ir, ie + 1);
//         int ia = tag.indexOf("idref=\"");
//         if (ia != -1) { ia += 7; spineIds.push_back(tag.substring(ia, tag.indexOf("\"", ia))); }
//         pos = ie + 1;
//       }
//     }
//   }

//   // ── 5. Extract each spine document ──
//   for (auto& sid : spineIds) {
//     if (manifest.count(sid) == 0) continue;
//     String href = opfDir + manifest[sid];
//     int frag = href.indexOf('#'); if (frag != -1) href = href.substring(0, frag);

//     size_t sz = 0;
//     void* data = mz_zip_reader_extract_file_to_heap(&zip, href.c_str(), &sz, 0);
//     if (data) {
//       String* s = new String();
//       s->reserve(sz + 1);
//       s->concat((char*)data, sz);
//       mz_free(data);
//       outSpine.push_back(*s);
//       delete s;
//     }
//     if (outSpine.size() > 200) break;
//     yield();
//   }

//   mz_zip_reader_end(&zip);
//   free(buf);
//   Serial.printf("Spine docs extracted: %d\n", (int)outSpine.size());
//   return outSpine.size() > 0;
// }

// ─────────────────────────────────────────────────────────────────────────────
//  OPEN BOOK  – opens the EPUB and processes chapters one at a time
// ─────────────────────────────────────────────────────────────────────────────
void openBook(const String& filename) {
  currentBook = filename;
  currentPage = 0;
  pageLines.clear();
  paginateReset();

  String fullPath = String(BOOKS_DIR) + "/" + filename;
  Serial.println("Opening: " + fullPath);
  Serial.printf("Free heap: %u  PSRAM: %u\n", ESP.getFreeHeap(), ESP.getFreePsram());

  String coverPath;
  int chapterCount = 0;

  bool ok = extractEpub(fullPath, coverPath,
    [&](int chIdx, int totalChapters, const String& chapterHtml) {
      if (chIdx % 5 == 0) {
        display.setFullWindow();
        display.firstPage();
        do {
          display.fillScreen(GxEPD_WHITE);
          display.setFont(&FreeSerif12pt7b);
          display.setTextColor(GxEPD_BLACK);
          int pct = totalChapters > 0 ? (chIdx * 100) / totalChapters : 0;
          drawCenteredText("Loading... " + String(pct) + "%", DISPLAY_H/2, &FreeSerif12pt7b);
          drawCenteredText(String(chIdx+1)+"/"+String(totalChapters)+" chapters",
                           DISPLAY_H/2+30, &FreeSerif9pt7b);
        } while (display.nextPage());
      }
      String text = extractTextFromHtml(chapterHtml);
      paginateAppend(text);
      chapterCount++;
      Serial.printf("Ch%d  heap:%u  psram:%u  pages:%d\n",
                    chIdx, ESP.getFreeHeap(), ESP.getFreePsram(), (int)pageLines.size());
      yield();
    }
  );

  if (!ok) {
    pageLines.push_back("Error: could not parse EPUB.\nCheck Serial for details.");
    return;
  }

  paginateFlushAll();
  totalPages = pageLines.size();
  Serial.printf("Done: %d pages from %d chapters\n", totalPages, chapterCount);
  strlcpy(rtc_book, filename.c_str(), sizeof(rtc_book));
  rtc_page = 0;
}

// ─────────────────────────────────────────────────────────────────────────────
//  ZIP FILE READ CALLBACK
//  miniz uses this to read the archive directly from FFat — no RAM buffer needed.
// ─────────────────────────────────────────────────────────────────────────────
// struct ZipFileCtx {
//   File   file;
//   size_t size;
// };

// static size_t zipFileReadCb(void* pOpaque, mz_uint64 file_ofs, void* pBuf, size_t n) {
//   ZipFileCtx* ctx = (ZipFileCtx*)pOpaque;
//   if (!ctx->file || file_ofs >= ctx->size) return 0;
//   if (file_ofs + n > ctx->size) n = ctx->size - (size_t)file_ofs;
//   ctx->file.seek((size_t)file_ofs);
//   return ctx->file.read((uint8_t*)pBuf, n);
// }

// // ─────────────────────────────────────────────────────────────────────────────
// //  ZIP HELPER — decompress one entry into PSRAM (or heap for tiny files).
// //  Caller must free() the result.
// // ─────────────────────────────────────────────────────────────────────────────
// static uint8_t* zipExtractToPsram(mz_zip_archive* zip, const char* filename, size_t* outSize) {
//   int idx = mz_zip_reader_locate_file(zip, filename, nullptr, 0);
//   if (idx < 0) { Serial.printf("zip: '%s' not found\n", filename); return nullptr; }
//   mz_zip_archive_file_stat st;
//   if (!mz_zip_reader_file_stat(zip, idx, &st)) return nullptr;
//   size_t sz = (size_t)st.m_uncomp_size;
//   *outSize = sz;
//   uint8_t* b = (sz > 2048)
//                ? (uint8_t*)heap_caps_malloc(sz + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
//                : (uint8_t*)malloc(sz + 1);
//   if (!b) { b = (uint8_t*)malloc(sz + 1); }   // fallback to heap
//   if (!b) { Serial.printf("zip: alloc %u failed\n", (unsigned)sz); return nullptr; }
//   if (!mz_zip_reader_extract_to_mem(zip, idx, b, sz, 0)) {
//     Serial.printf("zip: decompress failed '%s'\n", filename);
//     free(b); return nullptr;
//   }
//   b[sz] = 0;
//   return b;
// }

static uint8_t* zipExtractFile(mz_zip_archive* zip,
                               const char* filename,
                               size_t* outSize) {
  int idx = mz_zip_reader_locate_file(zip, filename, nullptr, 0);
  if (idx < 0) { Serial.printf("zip: '%s' not found\n", filename); return nullptr; }
  mz_zip_archive_file_stat st;
  if (!mz_zip_reader_file_stat(zip, idx, &st)) return nullptr;
  size_t sz = (size_t)st.m_uncomp_size;
  *outSize  = sz;
  uint8_t* b = (sz > 2048)
    ? (uint8_t*)heap_caps_malloc(sz + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
    : (uint8_t*)malloc(sz + 1);
  if (!b) b = (uint8_t*)malloc(sz + 1);
  if (!b) { Serial.printf("alloc %u failed\n", (unsigned)sz); return nullptr; }
  if (!mz_zip_reader_extract_to_mem(zip, idx, b, sz, 0)) {
    free(b); return nullptr;
  }
  b[sz] = 0;
  return b;
}

bool extractEpub(const String& epubPath, String& coverPath,
                 std::function<void(int,int,const String&)> chapterCb) {

  File epubFile = FFat.open(epubPath, "r");
  if (!epubFile) { Serial.println("Cannot open epub"); return false; }
  size_t fileSize = epubFile.size();
  Serial.printf("EPUB size: %u bytes\n", fileSize);

  // Load compressed archive into PSRAM — stays there for the whole parse
  uint8_t* buf = (uint8_t*)heap_caps_malloc(fileSize,
                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!buf) buf = (uint8_t*)malloc(fileSize);
  if (!buf) { Serial.println("alloc failed"); epubFile.close(); return false; }
  epubFile.read(buf, fileSize);
  epubFile.close();

  mz_zip_archive zip;
  memset(&zip, 0, sizeof(zip));
  // *** KEY FIX: use init_mem, not init with custom callbacks ***
  if (!mz_zip_reader_init_mem(&zip, buf, fileSize, 0)) {
    Serial.printf("miniz: zip open failed\n");
    free(buf); return false;
  }
  Serial.printf("ZIP entries: %d\n", (int)mz_zip_reader_get_num_files(&zip));
// ─────────────────────────────────────────────────────────────────────────────
//  EPUB EXTRACTION — file-callback edition
//  miniz reads ZIP headers directly from FFat — zero heap/PSRAM for the archive.
//  Only one decompressed chapter lives in PSRAM at a time.
//  chapterCb(index, total, htmlString) called per spine document.
// ─────────────────────────────────────────────────────────────────────────────
bool extractEpub(const String& epubPath, String& coverPath,
                 std::function<void(int,int,const String&)> chapterCb) {

  ZipFileCtx ctx;
  ctx.file = FFat.open(epubPath, "r");
  if (!ctx.file) { Serial.println("Cannot open epub"); return false; }
  ctx.size = ctx.file.size();
  Serial.printf("EPUB size: %u bytes  heap:%u  psram:%u\n",
                ctx.size, ESP.getFreeHeap(), ESP.getFreePsram());

  // Init miniz with read callback — m_pState allocation is only ~few KB from heap
  mz_zip_archive zip;
  memset(&zip, 0, sizeof(zip));
  zip.m_pRead      = zipFileReadCb;
  zip.m_pIO_opaque = &ctx;
  if (!mz_zip_reader_init(&zip, ctx.size, 0)) {
    Serial.printf("miniz: init failed (err %d)\n", (int)mz_zip_get_last_error(&zip));
    ctx.file.close(); return false;
  }
  int numFiles = (int)mz_zip_reader_get_num_files(&zip);
  Serial.printf("ZIP entries: %d  heap after init:%u\n", numFiles, ESP.getFreeHeap());

  // ── 1. OPF path from META-INF/container.xml ──
  String opfPath = "";
  {
    size_t sz = 0;
    uint8_t* d = zipExtractToPsram(&zip, "META-INF/container.xml", &sz);
    if (d) {
      const char* p = strstr((char*)d, "full-path=\"");
      if (p) { p += 11; const char* e = strchr(p, '"'); if (e) opfPath = String(p, e-p); }
      free(d);
    }
  }
  if (opfPath.length() == 0) {
    for (int i = 0; i < numFiles; i++) {
      mz_zip_archive_file_stat st; mz_zip_reader_file_stat(&zip, i, &st);
      if (String(st.m_filename).endsWith(".opf")) { opfPath = st.m_filename; break; }
    }
  }
  if (opfPath.length() == 0) {
    Serial.println("No OPF found");
    mz_zip_reader_end(&zip); ctx.file.close(); return false;
  }
  Serial.println("OPF: " + opfPath);
  String opfDir = "";
  int ls = opfPath.lastIndexOf('/');
  if (ls != -1) opfDir = opfPath.substring(0, ls + 1);

  // ── 2. Parse manifest + spine from OPF ──
  std::map<String, String> manifest;
  std::vector<String> spineIds;   // just short idref strings — negligible memory
  {
    size_t sz = 0;
    uint8_t* d = zipExtractToPsram(&zip, opfPath.c_str(), &sz);
    if (!d) {
      Serial.println("Cannot extract OPF");
      mz_zip_reader_end(&zip); ctx.file.close(); return false;
    }
    char* opf = (char*)d;

    char* p = opf;
    while ((p = strstr(p, "<item ")) != nullptr) {
      char* e = strchr(p, '>'); if (!e) break;
      char sv = *e; *e = 0;
      auto ga = [&](const char* attr) -> String {
        char key[40]; snprintf(key, sizeof(key), " %s=\"", attr);
        char* x = strstr(p, key); if (!x) return "";
        x += strlen(key);
        char* y = strchr(x, '"'); if (!y) return "";
        return String(x, y - x);
      };
      String id = ga("id"), href = ga("href"),
             mt = ga("media-type"), pr = ga("properties");
      *e = sv;
      href.replace("%20", " ");
      if (id.length() && href.length()) manifest[id] = href;
      if (coverPath.length() == 0 &&
          (pr.indexOf("cover-image") != -1 || id.indexOf("cover") != -1) &&
          mt.indexOf("image") != -1) {
        coverPath = opfDir + href;
      }
      p = e + 1;
    }
    p = opf;
    while ((p = strstr(p, "<itemref ")) != nullptr) {
      char* e = strchr(p, '>'); if (!e) break;
      char sv = *e; *e = 0;
      char* a = strstr(p, "idref=\"");
      if (a) { a += 7; char* b = strchr(a, '"'); if (b) spineIds.push_back(String(a, b-a)); }
      *e = sv; p = e + 1;
    }
    free(d);
  }
  Serial.printf("Manifest:%d  Spine:%d  heap:%u\n",
                (int)manifest.size(), (int)spineIds.size(), ESP.getFreeHeap());

  // ── 3. Save cover image ──
  if (coverPath.length() > 0) {
    size_t sz = 0;
    uint8_t* img = zipExtractToPsram(&zip, coverPath.c_str(), &sz);
    if (img) {
      String ext = coverPath.substring(coverPath.lastIndexOf('.'));
      File cf = FFat.open("/cover_raw" + ext, "w");
      if (cf) { cf.write(img, sz); cf.close(); coverPath = "/cover_raw" + ext; }
      free(img);
    }
  }

  // ── 4. Extract chapters one at a time ──
  int total = (int)spineIds.size();
  int count = 0;
  for (int i = 0; i < total && count < 300; i++) {
    if (manifest.count(spineIds[i]) == 0) continue;
    String href = opfDir + manifest[spineIds[i]];
    int frag = href.indexOf('#'); if (frag != -1) href = href.substring(0, frag);

    size_t sz = 0;
    uint8_t* d = zipExtractToPsram(&zip, href.c_str(), &sz);
    if (d) {
      String chapter((char*)d, sz);
      free(d);   // free immediately — never accumulate
      chapterCb(count, total, chapter);
      count++;
    } else {
      Serial.printf("  skip '%s'\n", href.c_str());
    }
    yield();
  }

  mz_zip_reader_end(&zip);
  ctx.file.close();
  Serial.printf("Done: %d/%d chapters  heap:%u  psram:%u\n",
                count, total, ESP.getFreeHeap(), ESP.getFreePsram());
  return count > 0;
}
// // ─────────────────────────────────────────────────────────────────────────────
// //  ZIP HELPER — decompress one file from zip into a ps_malloc'd buffer.
// //  Uses mz_zip_reader_extract_to_mem() so miniz never internally MZ_MALLOCs
// //  the output — we hand it our PSRAM pointer directly.
// // ─────────────────────────────────────────────────────────────────────────────
// static uint8_t* zipExtractToPsram(mz_zip_archive* zip, const char* filename, size_t* outSize) {
//   int idx = mz_zip_reader_locate_file(zip, filename, nullptr, 0);
//   if (idx < 0) { Serial.printf("zip: '%s' not found\n", filename); return nullptr; }
//   mz_zip_archive_file_stat st;
//   if (!mz_zip_reader_file_stat(zip, idx, &st)) return nullptr;
//   size_t sz = (size_t)st.m_uncomp_size;
//   *outSize = sz;
//   uint8_t* b = (sz > 2048) ? (uint8_t*)ps_malloc(sz+1) : (uint8_t*)malloc(sz+1);
//   if (!b) { Serial.printf("zip: alloc %u failed\n", (unsigned)sz); return nullptr; }
//   if (!mz_zip_reader_extract_to_mem(zip, idx, b, sz, 0)) {
//     free(b); return nullptr;
//   }
//   b[sz] = 0;
//   return b;
// }

// // ─────────────────────────────────────────────────────────────────────────────
// //  EPUB EXTRACTION — callback edition
// //  chapterCb(index, total, htmlString) called per spine doc. One chapter
// //  in memory at a time — safe for large books.
// // ─────────────────────────────────────────────────────────────────────────────
// bool extractEpub(const String& epubPath, String& coverPath,
//                  std::function<void(int,int,const String&)> chapterCb) {

//   File epubFile = FFat.open(epubPath, "r");
//   if (!epubFile) { Serial.println("Cannot open epub"); return false; }
//   size_t fileSize = epubFile.size();
//   Serial.printf("EPUB size: %u bytes\n", fileSize);

//   // Load compressed ZIP into PSRAM — this is just raw compressed bytes, fine in PSRAM
//   uint8_t* buf = (uint8_t*)ps_malloc(fileSize);
//   if (!buf) buf = (uint8_t*)malloc(fileSize);
//   if (!buf) { Serial.println("alloc failed"); epubFile.close(); return false; }
//   epubFile.read(buf, fileSize);
//   epubFile.close();
//   Serial.printf("Loaded into %s\n", esp_ptr_external_ram(buf) ? "PSRAM" : "heap");

//   mz_zip_archive zip;
//   memset(&zip, 0, sizeof(zip));
//   if (!mz_zip_reader_init_mem(&zip, buf, fileSize, 0)) {
//     Serial.println("miniz: zip open failed"); free(buf); return false;
//   }
//   Serial.printf("ZIP entries: %d\n", (int)mz_zip_reader_get_num_files(&zip));

//   // ── 1. Find OPF path ──
//   String opfPath = "";
//   {
//     size_t sz = 0;
//     uint8_t* d = zipExtractToPsram(&zip, "META-INF/container.xml", &sz);
//     if (d) {
//       const char* p = strstr((char*)d, "full-path=\"");
//       if (p) { p+=11; const char* e=strchr(p,'"'); if(e) opfPath=String(p,e-p); }
//       free(d);
//     }
//   }
//   if (opfPath.length() == 0) {
//     int n = (int)mz_zip_reader_get_num_files(&zip);
//     for (int i=0;i<n;i++) {
//       mz_zip_archive_file_stat st; mz_zip_reader_file_stat(&zip,i,&st);
//       if (String(st.m_filename).endsWith(".opf")) { opfPath=st.m_filename; break; }
//     }
//   }
//   if (opfPath.length()==0) {
//     Serial.println("No OPF"); mz_zip_reader_end(&zip); free(buf); return false;
//   }
//   Serial.println("OPF: " + opfPath);
//   String opfDir = "";
//   int ls = opfPath.lastIndexOf('/');
//   if (ls != -1) opfDir = opfPath.substring(0, ls+1);

//   // ── 2. Parse manifest + spine from OPF ──
//   std::map<String,String> manifest;
//   std::vector<String> spineIds;   // just short ID strings, not content
//   {
//     size_t sz = 0;
//     uint8_t* d = zipExtractToPsram(&zip, opfPath.c_str(), &sz);
//     if (!d) { mz_zip_reader_end(&zip); free(buf); return false; }
//     char* opf = (char*)d;

//     char* p = opf;
//     while ((p = strstr(p, "<item ")) != nullptr) {
//       char* e = strchr(p, '>'); if (!e) break;
//       char sv=*e; *e=0;
//       auto ga = [&](const char* a) -> String {
//         char k[40]; snprintf(k,sizeof(k)," %s=\"",a);
//         char* x=strstr(p,k); if(!x) return "";
//         x+=strlen(k); char* y=strchr(x,'"'); if(!y) return "";
//         return String(x,y-x);
//       };
//       String id=ga("id"),href=ga("href"),mt=ga("media-type"),pr=ga("properties");
//       *e=sv;
//       href.replace("%20"," ");
//       if (id.length()&&href.length()) manifest[id]=href;
//       if (coverPath.length()==0 &&
//           (pr.indexOf("cover-image")!=-1||id.indexOf("cover")!=-1) &&
//           mt.indexOf("image")!=-1) {
//         coverPath = opfDir+href;
//       }
//       p=e+1;
//     }

//     p = opf;
//     while ((p = strstr(p, "<itemref ")) != nullptr) {
//       char* e=strchr(p,'>'); if(!e) break;
//       char sv=*e; *e=0;
//       char* a=strstr(p,"idref=\"");
//       if(a){a+=7; char*b=strchr(a,'"'); if(b) spineIds.push_back(String(a,b-a));}
//       *e=sv; p=e+1;
//     }
//     free(d);
//   }
//   Serial.printf("Manifest:%d  Spine:%d\n",(int)manifest.size(),(int)spineIds.size());

//   // ── 3. Save cover image ──
//   if (coverPath.length() > 0) {
//     size_t sz=0;
//     uint8_t* img = zipExtractToPsram(&zip, coverPath.c_str(), &sz);
//     if (img) {
//       String ext = coverPath.substring(coverPath.lastIndexOf('.'));
//       File cf = FFat.open("/cover_raw"+ext, "w");
//       if (cf) { cf.write(img,sz); cf.close(); coverPath="/cover_raw"+ext; }
//       free(img);
//     }
//   }

//   // ── 4. Extract + callback each spine chapter ──
//   int total = (int)spineIds.size();
//   int count = 0;
//   for (int i=0; i<total && count<300; i++) {
//     if (manifest.count(spineIds[i])==0) continue;
//     String href = opfDir + manifest[spineIds[i]];
//     int frag=href.indexOf('#'); if(frag!=-1) href=href.substring(0,frag);

//     size_t sz=0;
//     uint8_t* d = zipExtractToPsram(&zip, href.c_str(), &sz);
//     if (d) {
//       String chapter((char*)d, sz);
//       free(d);                      // free PSRAM immediately — don't accumulate
//       chapterCb(count, total, chapter);
//       count++;
//     }
//     yield();
//   }

//   mz_zip_reader_end(&zip);
//   free(buf);
//   Serial.printf("Extracted %d chapters  heap:%u  psram:%u\n",
//                 count, ESP.getFreeHeap(), ESP.getFreePsram());
//   return count > 0;
// }
// ─────────────────────────────────────────────────────────────────────────────
//  HTML → PLAIN TEXT
//  Strips tags, decodes common entities, collapses whitespace.
// ─────────────────────────────────────────────────────────────────────────────
String extractTextFromHtml(const String& html) {
  String out;
  out.reserve(html.length() / 2);
  bool inTag    = false;
  bool lastSpace = true;

  for (int i = 0; i < (int)html.length(); i++) {
    char c = html[i];

    if (inTag) {
      if (c == '>') inTag = false;
      continue;
    }

    if (c == '<') {
      int tagStart = i + 1;
      int tagEnd   = html.indexOf('>', tagStart);
      if (tagEnd == -1) { inTag = true; continue; }
      String tagContent = html.substring(tagStart, tagEnd);
      String tagName    = tagContent;
      int sp = tagContent.indexOf(' '); if (sp != -1) tagName = tagContent.substring(0, sp);
      tagName.trim(); tagName.toLowerCase();
      bool closing = tagName.startsWith("/"); if (closing) tagName = tagName.substring(1);

      if (tagName == "p"  || tagName == "div" || tagName == "br"  ||
          tagName == "h1" || tagName == "h2"  || tagName == "h3"  ||
          tagName == "h4" || tagName == "h5"  || tagName == "h6"  ||
          tagName == "li" || tagName == "tr") {
        if (!out.endsWith("\n")) out += "\n";
        if (tagName.length() == 2 && tagName[0] == 'h') { if (!out.endsWith("\n\n")) out += "\n"; }
      }
      i = tagEnd; lastSpace = true; continue;
    }

    if (c == '&') {
      int semi = html.indexOf(';', i);
      if (semi != -1 && semi - i < 12) {
        String entity = html.substring(i + 1, semi);
        char mapped = 0;
        if      (entity == "amp")  mapped = '&';
        else if (entity == "lt")   mapped = '<';
        else if (entity == "gt")   mapped = '>';
        else if (entity == "quot") mapped = '"';
        else if (entity == "apos") mapped = '\'';
        else if (entity == "nbsp" || entity == "#160") mapped = ' ';
        else if (entity.startsWith("#")) {
          long code = entity.substring(1).toInt();
          mapped = (code >= 32 && code < 128) ? (char)code : ' ';
        }
        if (mapped) { out += mapped; i = semi; lastSpace = (mapped == ' '); continue; }
      }
    }

    if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    if (c == ' ') {
      if (!lastSpace && !out.endsWith("\n")) { out += ' '; }
      lastSpace = true;
    } else {
      out += c; lastSpace = false;
    }
  }
  return out;
}


// ─────────────────────────────────────────────────────────────────────────────
//  PAGINATE TEXT (incremental — called per chapter, appends to pageLines)
//  Uses pixel-based word wrap via Adafruit_GFX getTextBounds.
// ─────────────────────────────────────────────────────────────────────────────
#define PAGE_TEXT_W  (READ_RIGHT_X - READ_LEFT_X)

// State carried across chapter boundaries so paragraphs don't get cut
static String  _pag_carryLine  = "";
static String  _pag_carryPage  = "";
static int     _pag_lineCount  = 0;

void paginateReset() {
  _pag_carryLine = "";
  _pag_carryPage = "";
  _pag_lineCount = 0;
}

void paginateFlushLine(const String& line) {
  if (_pag_lineCount >= READ_LINES_MAX) {
    if (_pag_carryPage.length() > 0) pageLines.push_back(_pag_carryPage);
    _pag_carryPage = "";
    _pag_lineCount = 0;
  }
  _pag_carryPage += line + "\n";
  _pag_lineCount++;
}

void paginateFlushAll() {
  if (_pag_carryLine.length() > 0) {
    paginateFlushLine(_pag_carryLine);
    _pag_carryLine = "";
  }
  if (_pag_carryPage.length() > 0) {
    pageLines.push_back(_pag_carryPage);
    _pag_carryPage = "";
    _pag_lineCount = 0;
  }
}

void paginateAppend(const String& text) {
  display.setFont(&FreeSerif12pt7b);
  int pos = 0;

  while (pos <= (int)text.length()) {
    // Paragraph break (double newline)
    if (pos < (int)text.length() && text[pos] == '\n') {
      bool isPara = (pos + 1 < (int)text.length() && text[pos + 1] == '\n');
      pos += isPara ? 2 : 1;

      // Flush current line
      if (_pag_carryLine.length() > 0) {
        paginateFlushLine(_pag_carryLine);
        _pag_carryLine = "";
      }
      if (isPara) {
        // Blank separator line between paragraphs
        if (_pag_lineCount < READ_LINES_MAX) {
          _pag_carryPage += "\n";
          _pag_lineCount++;
        }
      }
      continue;
    }

    if (pos == (int)text.length()) break;

    // Extract word
    int wordEnd = pos;
    while (wordEnd < (int)text.length() &&
           text[wordEnd] != ' ' && text[wordEnd] != '\n') {
      wordEnd++;
    }
    if (wordEnd == pos) { pos++; continue; }

    String word = text.substring(pos, wordEnd);
    pos = wordEnd + 1;

    // Test fit on current line
    String testLine = _pag_carryLine.length() > 0
                      ? _pag_carryLine + " " + word
                      : word;

    int16_t x1, y1; uint16_t w, h;
    display.getTextBounds(testLine, 0, 0, &x1, &y1, &w, &h);

    if ((int)w > PAGE_TEXT_W && _pag_carryLine.length() > 0) {
      paginateFlushLine(_pag_carryLine);
      _pag_carryLine = word;
    } else {
      _pag_carryLine = testLine;
    }
  }
  // Note: don't flush at chapter end — paragraphs can span chapters.
  // Call paginateFlushAll() after last chapter (done in openBook).
  yield();
}

void drawStatusBar(const String& left, const String& right) {
  display.setFont(&FreeSansBold9pt7b);
  display.setTextColor(GxEPD_BLACK);
  display.setCursor(10, DISPLAY_H - 5);
  display.print(left);
  // Right-align right string
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds(right, 0, 0, &x1, &y1, &w, &h);
  display.setCursor(DISPLAY_W - w - 10, DISPLAY_H - 5);
  display.print(right);
}

void drawCenteredText(const String& text, int y, const GFXfont* font) {
  display.setFont(font);
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((DISPLAY_W - w) / 2 - x1, y);
  display.print(text);
}

// ─────────────────────────────────────────────────────────────────────────────
//  WIFI MODE
// ─────────────────────────────────────────────────────────────────────────────
// Web UI served from a single HTML string in PROGMEM (keeps the LittleFS
// space free for books). The UI allows uploading EPUBs and deleting them.
static const char WEB_UI[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>eReader Library</title>
<style>
  *{box-sizing:border-box;margin:0;padding:0}
  body{font-family:Georgia,serif;background:#f5f0e8;color:#1a1a1a;padding:20px}
  h1{font-size:2rem;margin-bottom:6px;color:#2c1810}
  .sub{color:#666;margin-bottom:24px;font-size:.9rem}
  .card{background:#fff;border-radius:8px;padding:20px;margin-bottom:20px;
        box-shadow:0 2px 8px rgba(0,0,0,.08)}
  h2{font-size:1.2rem;margin-bottom:14px;color:#2c1810}
  .drop{border:2px dashed #c8b89a;border-radius:6px;padding:30px;text-align:center;
        cursor:pointer;transition:border-color .2s,background .2s;color:#888}
  .drop.over,.drop:hover{border-color:#2c1810;background:#fdf8f0}
  input[type=file]{display:none}
  .btn{display:inline-block;padding:10px 20px;border-radius:5px;border:none;
       cursor:pointer;font-size:.95rem;transition:background .2s}
  .btn-primary{background:#2c1810;color:#fff}
  .btn-primary:hover{background:#4a2c1a}
  .btn-danger{background:#c0392b;color:#fff;padding:6px 14px;font-size:.85rem}
  .btn-danger:hover{background:#a93226}
  progress{width:100%;height:8px;border-radius:4px;overflow:hidden;margin-top:10px}
  progress::-webkit-progress-bar{background:#eee}
  progress::-webkit-progress-value{background:#2c1810}
  #msg{margin-top:10px;font-size:.9rem;color:#2c1810}
  .book-list{list-style:none}
  .book-item{display:flex;align-items:center;justify-content:space-between;
             padding:10px 0;border-bottom:1px solid #eee}
  .book-item:last-child{border-bottom:none}
  .book-name{flex:1;font-size:1rem}
  .storage{color:#666;font-size:.85rem;margin-top:8px}
  .badge{display:inline-block;background:#e8f4e8;color:#2d6a2d;
         padding:2px 8px;border-radius:12px;font-size:.75rem;margin-left:8px}
</style>
</head>
<body>
<h1>📚 eReader</h1>
<p class="sub">Connected to eReader WiFi — upload EPUBs to your device</p>

<div class="card">
  <h2>Upload Book</h2>
  <div class="drop" id="drop" onclick="document.getElementById('fi').click()">
    <p>📖 Drop EPUB here or click to choose</p>
    <p style="font-size:.8rem;margin-top:6px">Supports .epub files (Project Gutenberg recommended)</p>
  </div>
  <input type="file" id="fi" accept=".epub">
  <progress id="prog" value="0" max="100" style="display:none"></progress>
  <p id="msg"></p>
</div>

<div class="card">
  <h2>Library</h2>
  <ul class="book-list" id="booklist"><li style="color:#999">Loading...</li></ul>
  <p class="storage" id="storage"></p>
</div>

<script>
const drop=document.getElementById('drop');
const fi=document.getElementById('fi');
const prog=document.getElementById('prog');
const msg=document.getElementById('msg');

drop.addEventListener('dragover',e=>{e.preventDefault();drop.classList.add('over')});
drop.addEventListener('dragleave',()=>drop.classList.remove('over'));
drop.addEventListener('drop',e=>{e.preventDefault();drop.classList.remove('over');
  const f=e.dataTransfer.files[0];if(f)uploadFile(f)});
fi.addEventListener('change',()=>{if(fi.files[0])uploadFile(fi.files[0])});

function uploadFile(f){
  if(!f.name.toLowerCase().endsWith('.epub')){msg.textContent='⚠ Only .epub files are supported.';return}
  const form=new FormData();form.append('file',f);
  const xhr=new XMLHttpRequest();
  xhr.upload.onprogress=e=>{
    if(e.lengthComputable){prog.style.display='block';prog.value=Math.round(e.loaded/e.total*100)}
  };
  xhr.onload=()=>{
    prog.style.display='none';
    if(xhr.status===200){msg.textContent='✓ '+f.name+' uploaded!';loadBooks()}
    else{msg.textContent='✗ Upload failed: '+xhr.responseText}
  };
  xhr.onerror=()=>{prog.style.display='none';msg.textContent='✗ Network error'};
  msg.textContent='Uploading…';prog.value=0;prog.style.display='block';
  xhr.open('POST','/upload');xhr.send(form);
}

function loadBooks(){
  fetch('/books').then(r=>r.json()).then(data=>{
    const ul=document.getElementById('booklist');
    document.getElementById('storage').textContent=
      'Storage: '+data.used+'KB used / '+data.total+'KB total';
    if(!data.books||data.books.length===0){
      ul.innerHTML='<li style="color:#999;padding:10px 0">No books yet — upload an EPUB!</li>';return;
    }
    ul.innerHTML=data.books.map(b=>`
      <li class="book-item">
        <span class="book-name">${esc(b.name)}<span class="badge">${b.size}KB</span></span>
        <button class="btn btn-danger" onclick="deleteBook('${esc(b.name)}')">Delete</button>
      </li>`).join('');
  });
}

function deleteBook(name){
  if(!confirm('Delete "'+name+'"?'))return;
  fetch('/delete?file='+encodeURIComponent(name),{method:'POST'}).then(()=>loadBooks());
}

function esc(s){return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;')}

loadBooks();
</script>
</body>
</html>
)rawliteral";

void startWifi() {
  // WiFi stack needs ~80KB contiguous heap. Check before starting.
  uint32_t freeHeap = ESP.getFreeHeap();
  Serial.printf("Free heap before WiFi: %u\n", freeHeap);
  if (freeHeap < 90000) {
    Serial.println("WARNING: Low heap before WiFi start — may crash.");
    // Force a GC moment
    delay(100);
  }
  WiFi.softAPConfig(WIFI_AP_IP, WIFI_AP_IP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASS[0] ? WIFI_AP_PASS : nullptr);
  wifiActive = true;

  // Serve web UI
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
    req->send_P(200, "text/html", WEB_UI);
  });

  // List books as JSON
  server.on("/books", HTTP_GET, [](AsyncWebServerRequest* req) {
    JsonDocument doc;
    JsonArray arr = doc["books"].to<JsonArray>();
    File dir = FFat.open(BOOKS_DIR);
    if (dir && dir.isDirectory()) {
      File entry;
      while ((entry = dir.openNextFile())) {
        String name = entry.name();
        if (name.endsWith(".epub")) {
          JsonObject o = arr.add<JsonObject>();
          o["name"] = name;
          o["size"] = (int)(entry.size() / 1024);
        }
        entry.close();
      }
    }
    // Storage info
    size_t total = FFat.totalBytes();
    size_t used  = FFat.usedBytes();
    doc["total"] = (int)(total / 1024);
    doc["used"]  = (int)(used  / 1024);
    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
  });

  // Delete book
  server.on("/delete", HTTP_POST, [](AsyncWebServerRequest* req) {
    if (req->hasParam("file")) {
      String name = req->getParam("file")->value();
      // Sanitise
      name.replace("/", "");
      name.replace("..", "");
      FFat.remove(String(BOOKS_DIR) + "/" + name);
      scanBooks();
    }
    req->send(200, "text/plain", "ok");
  });

  // Upload EPUB
  server.on("/upload", HTTP_POST,
    [](AsyncWebServerRequest* req) {
      req->send(200, "text/plain", "ok");
      scanBooks();
    },
    [](AsyncWebServerRequest* req, String filename, size_t index, uint8_t* data, size_t len, bool final) {
      static File uploadFile;
      if (index == 0) {
        filename.replace("/", "_");
        filename.replace("..", "");
        if (!filename.endsWith(".epub")) filename += ".epub";
        String path = String(BOOKS_DIR) + "/" + filename;
        Serial.println("Upload start: " + path);
        Serial.printf("  Free heap: %u  Free PSRAM: %u\n", ESP.getFreeHeap(), ESP.getFreePsram());
        // Ensure FS is mounted
        if (!FFat.totalBytes()) {
          FFat.begin(true);
        }
        if (!FFat.exists(BOOKS_DIR)) FFat.mkdir(BOOKS_DIR);
        uploadFile = FFat.open(path, "w");
        if (!uploadFile) Serial.println("ERROR: FFat file open failed! Check partition scheme.");
      }
      if (uploadFile) {
        uploadFile.write(data, len);
        if (final) {
          uploadFile.close();
          Serial.printf("Upload complete: %u bytes\n", index + len);
          scanBooks();
        }
      }
    }
  );

  server.begin();
  Serial.println("WiFi AP started: " + String(WIFI_AP_SSID));
  Serial.println("IP: " + WiFi.softAPIP().toString());

  // Show WiFi info screen
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setTextColor(GxEPD_BLACK);
    display.setFont(&FreeSerifBold18pt7b);
    drawCenteredText("WiFi Upload Mode", 40, &FreeSerifBold18pt7b);
    display.drawFastHLine(10, 52, DISPLAY_W - 20, GxEPD_BLACK);

    display.setFont(&FreeSerif12pt7b);
    drawCenteredText("Connect to WiFi network:", 90, &FreeSerif12pt7b);
    display.setFont(&FreeSerifBold18pt7b);
    drawCenteredText(WIFI_AP_SSID, 125, &FreeSerifBold18pt7b);

    display.setFont(&FreeSerif12pt7b);
    drawCenteredText("Password: " + String(WIFI_AP_PASS), 158, &FreeSerif12pt7b);
    drawCenteredText("Then open browser to:", 185, &FreeSerif12pt7b);
    display.setFont(&FreeSerifBold18pt7b);
    drawCenteredText(WiFi.softAPIP().toString(), 218, &FreeSerifBold18pt7b);

    display.drawFastHLine(10, 248, DISPLAY_W - 20, GxEPD_BLACK);
    display.setFont(&FreeSerif9pt7b);
    drawCenteredText("Press BACK to exit WiFi mode", 272, &FreeSerif9pt7b);
  } while (display.nextPage());
}

void stopWifi() {
  server.end();
  WiFi.softAPdisconnect(true);
  wifiActive = false;
  Serial.println("WiFi stopped");
}

// ─────────────────────────────────────────────────────────────────────────────
//  DEEP SLEEP  – shows cover image, then sleeps.
//  Wake up with any button (configure ext1 wakeup on btn pins).
// ─────────────────────────────────────────────────────────────────────────────
void displayCoverForSleep() {
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);

    // If we have a cover image, render it (JPEG decode → dithered B/W)
    // For simplicity we show a styled placeholder with the book title.
    // A full JPEG decoder (e.g. TJpgDec) can be added for real cover rendering.
    // See comments at bottom of file for the TJpgDec integration approach.

    // Stylised sleep screen
    display.setFont(&FreeSerifBold18pt7b);
    display.setTextColor(GxEPD_BLACK);

    // Book title (centered, multi-line safe)
    String title = currentBook;
    title.replace(".epub", "");
    // Simple border
    display.drawRect(8, 8, DISPLAY_W - 16, DISPLAY_H - 16, GxEPD_BLACK);
    display.drawRect(12, 12, DISPLAY_W - 24, DISPLAY_H - 24, GxEPD_BLACK);

    // Decorative top rule
    display.fillRect(20, 30, DISPLAY_W - 40, 2, GxEPD_BLACK);

    drawCenteredText(title, 100, &FreeSerifBold18pt7b);
    display.fillRect(20, 115, DISPLAY_W - 40, 1, GxEPD_BLACK);

    display.setFont(&FreeSerif9pt7b);
    drawCenteredText("Project Gutenberg", 150, &FreeSerif9pt7b);

    display.setFont(&FreeSerif12pt7b);
    drawCenteredText("Page " + String(currentPage + 1) + " of " + String(pageLines.size()), 200, &FreeSerif12pt7b);

    display.setFont(&FreeSerif9pt7b);
    drawCenteredText("Press any button to wake", 260, &FreeSerif9pt7b);

    display.fillRect(20, DISPLAY_H - 35, DISPLAY_W - 40, 1, GxEPD_BLACK);
  } while (display.nextPage());
}

void goToSleep() {
  if (wifiActive) stopWifi();
  saveState();
  rtc_slept = true;

  displayCoverForSleep();

  // Power down display (keeps image shown, draws ~0mA)
  display.powerOff();

  // Configure wakeup on any button (active LOW → wake on LOW level)
  // EXT1 wakeup: wake if any of the button pins goes LOW
  uint64_t wakeupMask = (1ULL << BTN_NEXT) | (1ULL << BTN_PREV) |
                        (1ULL << BTN_SELECT) | (1ULL << BTN_BACK);
  esp_sleep_enable_ext1_wakeup(wakeupMask, ESP_EXT1_WAKEUP_ANY_LOW);

  Serial.println("Going to deep sleep...");
  Serial.flush();
  esp_deep_sleep_start();
}

// ─────────────────────────────────────────────────────────────────────────────
//  BUTTON HANDLING
// ─────────────────────────────────────────────────────────────────────────────
struct Button {
  uint8_t pin;
  bool    pressed;
  bool    longFired;
  uint32_t downTime;
};

Button buttons[] = {
  {BTN_NEXT,   false, false, 0},
  {BTN_PREV,   false, false, 0},
  {BTN_SELECT, false, false, 0},
  {BTN_BACK,   false, false, 0},
};
#define BTN_NEXT_IDX    0
#define BTN_PREV_IDX    1
#define BTN_SELECT_IDX  2
#define BTN_BACK_IDX    3

void handleButtons() {
  uint32_t now = millis();

  for (auto& btn : buttons) {
    bool rawLow = (digitalRead(btn.pin) == LOW);

    if (rawLow && !btn.pressed) {
      btn.pressed   = true;
      btn.downTime  = now;
      btn.longFired = false;
    }
    if (!rawLow && btn.pressed) {
      btn.pressed = false;
      uint32_t held = now - btn.downTime;
      if (held >= BTN_DEBOUNCE_MS && !btn.longFired) {
        // Short press event
        onButtonPress(btn.pin, false);
      }
    }
    if (btn.pressed && !btn.longFired &&
        (now - btn.downTime) >= BTN_LONG_PRESS_MS) {
      btn.longFired = true;
      onButtonPress(btn.pin, true);
    }
  }
}

void onButtonPress(uint8_t pin, bool isLong) {
  // BACK long-press → sleep (any mode)
  if (pin == BTN_BACK && isLong) {
    goToSleep();
    return;
  }

  if (appMode == MODE_LIBRARY) {
    if (pin == BTN_NEXT || pin == BTN_SELECT) {
      // Move cursor down
      if (pin == BTN_NEXT) {
        if (libCursor < (int)bookList.size() - 1) libCursor++;
        // Page forward if needed
        if (libCursor >= (libPage + 1) * LIB_BOOKS_PER_PAGE) libPage++;
        drawLibrary();
      }
      // SELECT: open book
      if (pin == BTN_SELECT && !bookList.empty()) {
        appMode = MODE_READING;
        openBook(bookList[libCursor]);
        currentPage = 0;
        drawReadingPage();
      }
    } else if (pin == BTN_PREV) {
      if (libCursor > 0) libCursor--;
      if (libCursor < libPage * LIB_BOOKS_PER_PAGE) libPage--;
      drawLibrary();
    } else if (pin == BTN_BACK) {
      // Toggle WiFi mode
      if (!wifiActive) {
        appMode = MODE_WIFI;
        startWifi();
      }
    }
  }
  else if (appMode == MODE_READING) {
    if (pin == BTN_NEXT) {
      if (currentPage < (int)pageLines.size() - 1) {
        currentPage++;
        drawReadingPage();
      }
    } else if (pin == BTN_PREV) {
      if (currentPage > 0) {
        currentPage--;
        drawReadingPage();
      }
    } else if (pin == BTN_BACK) {
      // Short BACK: go back to library
      appMode = MODE_LIBRARY;
      drawLibrary();
    } else if (pin == BTN_SELECT) {
      // SELECT while reading: show quick options (go to library)
      appMode = MODE_LIBRARY;
      drawLibrary();
    }
  }
  else if (appMode == MODE_WIFI) {
    if (pin == BTN_BACK) {
      stopWifi();
      appMode = MODE_LIBRARY;
      scanBooks();
      drawLibrary();
    }
  }
}