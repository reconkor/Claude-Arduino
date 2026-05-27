#define FIRMWARE_VERSION "1.0.0"

// 기본 종목 목록 (NVS 비어있을 때만 사용)
const char* DEFAULT_TICKERS = "AAPL,NVDA,TSLA,COIN:BTC,COIN:ETH,COIN:SOL";
const int MAX_TICKERS = 20;

// 차트 기간 옵션
struct ChartPeriod {
  const char* label;     // 웹 UI 버튼 라벨 (짧게)
  const char* display;   // 디스플레이 표시용 (상세)
  const char* yRange;    // Yahoo Finance range
  const char* yInterval; // Yahoo Finance interval
  int cgDays;            // CoinGecko days
};

const ChartPeriod PERIODS[] = {
  {"1D", "Past 1 Day",   "1d",  "30m", 1},
  {"1W", "Past 1 Week",  "5d",  "1h",  7},
  {"1M", "Past 1 Month", "1mo", "1d",  30},
  {"1Y", "Past 1 Year",  "1y",  "1wk", 365},
};
const int PERIOD_COUNT = sizeof(PERIODS) / sizeof(PERIODS[0]);

int chartPeriodIdx = 0;           // 기본 1D
unsigned long switchMs = 5000;    // 기본 5초
unsigned long dataRefreshMs = 300000; // 기본 5분
unsigned long lastDataRefresh = 0;

// FS.h를 먼저 포함하고 전역 네임스페이스로 올려야
// ESP32 core 3.3.8의 WebServer.h와 충돌하지 않음
#include <FS.h>
using fs::FS;

#include <WiFiManager.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <Preferences.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>

String tickers[MAX_TICKERS];
String tickerNames[MAX_TICKERS];   // shortName 캐시 (KR 주식용)
int tickerCount = 0;
WebServer webServer(80);

TFT_eSPI tft = TFT_eSPI();
Preferences prefs;

enum AssetType { TYPE_STOCK, TYPE_CRYPTO, TYPE_KSTOCK };

const int MAX_CHART_POINTS = 60;

struct TickerData {
  char symbol[24];
  float price;
  float prevClose;
  float change;
  bool valid;
  AssetType type;
  float chart[MAX_CHART_POINTS];
  int chartCount;
  float chartMin;
  float chartMax;
};

// ── 컬러 팔레트 ───────────────────────────────────────
#define COL_BG       TFT_BLACK
#define COL_TEXT     TFT_WHITE
#define COL_DIM      0x7BEF                    // 회색
#define COL_DIVIDER  0x18C3                    // 진한 회색
#define COL_UP       0x3666                    // 에메랄드 그린
#define COL_DOWN     0xE9C5                    // 알리자린 레드
#define COL_STOCK    0x4D9F                    // 파란 (US 주식)
#define COL_CRYPTO   0xFCA0                    // 오렌지 (코인)
#define COL_KSTOCK   0xC81F                    // 보라 (한국 주식)
#define COL_PRICE    0xFFFF                    // 화이트 (가격)

int currentIndex = 0;
unsigned long lastRefresh = 0;
char finnhubKey[64] = "";
volatile bool bleConnected = false;
char otaUrl[128] = "";           // e.g. http://192.168.1.10/firmware
unsigned long lastOtaCheck = 0;
const unsigned long OTA_CHECK_INTERVAL = 3600000UL; // 1시간

unsigned long priceRefreshMs = 30000;  // 가격 갱신 주기 (기본 30초, 주식/한국주식만)
unsigned long lastPriceRefresh = 0;
TickerData currentData = {};           // 현재 표시 중인 티커 데이터

// ── WiFiManager ───────────────────────────────────────
WiFiManager wm;
WiFiManagerParameter paramFinnhub("finnhub_key", "Finnhub API Key", "", 63);

void saveParamCallback() {
  strncpy(finnhubKey, paramFinnhub.getValue(), sizeof(finnhubKey) - 1);
  prefs.begin("ticker", false);
  prefs.putString("finnhub_key", finnhubKey);
  prefs.end();
}

// ── OTA ──────────────────────────────────────────────
void drawOtaScreen(int pct, const char* msg) {
  tft.fillScreen(COL_BG);
  tft.fillRoundRect(10, 6, 80, 20, 4, COL_STOCK);
  tft.setTextFont(2); tft.setTextSize(1);
  tft.setTextColor(COL_BG, COL_STOCK);
  tft.setCursor(18, 9); tft.print("UPDATING");

  tft.setTextFont(4); tft.setTextSize(1);
  tft.setTextColor(COL_TEXT, COL_BG);
  char buf[24]; snprintf(buf, sizeof(buf), "%d%%", pct);
  int w = tft.textWidth(buf);
  tft.setCursor((320 - w) / 2, 55); tft.print(buf);

  // 진행바
  int barW = 280, barH = 12, barX = 20, barY = 90;
  tft.drawRect(barX, barY, barW, barH, COL_DIM);
  tft.fillRect(barX + 1, barY + 1, (barW - 2) * pct / 100, barH - 2, COL_STOCK);

  tft.setTextFont(2); tft.setTextSize(1);
  tft.setTextColor(COL_DIM, COL_BG);
  int mw = tft.textWidth(msg);
  tft.setCursor((320 - mw) / 2, 115); tft.print(msg);
}

void checkOtaUpdate() {
  if (strlen(otaUrl) == 0) return;

  String base = String(otaUrl);
  if (!base.endsWith("/")) base += "/";
  String versionUrl = base + "version.txt";
  String firmwareUrl = base + "firmware.bin";
  bool isHttps = base.startsWith("https");

  Serial.printf("[OTA] Checking %s\n", versionUrl.c_str());

  // version.txt 확인
  HTTPClient http;
  http.setTimeout(8000);
  if (isHttps) {
    static WiFiClientSecure sc;
    sc.setInsecure();
    http.begin(sc, versionUrl);
  } else {
    http.begin(versionUrl);
  }
  int code = http.GET();
  if (code != 200) {
    Serial.printf("[OTA] version.txt HTTP %d\n", code);
    http.end(); return;
  }
  String remoteVer = http.getString();
  remoteVer.trim();
  http.end();

  Serial.printf("[OTA] remote=%s local=%s\n", remoteVer.c_str(), FIRMWARE_VERSION);
  if (remoteVer == FIRMWARE_VERSION) return;

  // 새 버전 발견 → 다운로드 & 플래시
  String label = "v" + String(FIRMWARE_VERSION) + " -> v" + remoteVer;
  drawOtaScreen(0, label.c_str());

  httpUpdate.onProgress([](int cur, int total) {
    if (total > 0) drawOtaScreen(cur * 100 / total, "Downloading...");
  });

  t_httpUpdate_return ret;
  if (isHttps) {
    static WiFiClientSecure sc2;
    sc2.setInsecure();
    ret = httpUpdate.update(sc2, firmwareUrl);
  } else {
    WiFiClient plain;
    ret = httpUpdate.update(plain, firmwareUrl);
  }

  switch (ret) {
    case HTTP_UPDATE_OK:
      drawOtaScreen(100, "Done! Restarting...");
      delay(1500); ESP.restart(); break;
    case HTTP_UPDATE_FAILED:
      Serial.printf("[OTA] Failed: %s\n", httpUpdate.getLastErrorString().c_str());
      break;
    default: break;
  }
}

void drawSetupScreen() {
  tft.fillScreen(COL_BG);

  // ── 헤더 태그 ─────────────────────────────────
  tft.fillRoundRect(10, 6, 110, 22, 4, COL_CRYPTO);
  tft.setTextFont(2);
  tft.setTextSize(1);
  tft.setTextColor(COL_BG, COL_CRYPTO);
  tft.setCursor(20, 10);
  tft.print("SETUP MODE");

  // 상단 디바이더
  tft.drawFastHLine(10, 35, 300, COL_DIVIDER);

  // ── 단계별 안내 (좌측 번호 + 라벨 + 강조값) ───────
  struct Step { const char* num; const char* label; const char* value; int y; };
  Step steps[] = {
    {"1.", "WiFi:",    "Ticker-Setup",       48},
    {"2.", "Browser:", "192.168.4.1",        72},
    {"3.", "Enter:",   "WiFi + Finnhub key", 96},
  };

  tft.setTextFont(2);
  tft.setTextSize(1);
  for (const auto& s : steps) {
    // 번호 (오렌지)
    tft.setTextColor(COL_CRYPTO, COL_BG);
    tft.setCursor(15, s.y);
    tft.print(s.num);
    // 라벨 (회색)
    tft.setTextColor(COL_DIM, COL_BG);
    tft.setCursor(35, s.y);
    tft.print(s.label);
    // 값 (흰색)
    tft.setTextColor(COL_TEXT, COL_BG);
    tft.setCursor(105, s.y);
    tft.print(s.value);
  }

  // 하단 디바이더
  tft.drawFastHLine(10, 128, 300, COL_DIVIDER);

  // 상태 메시지
  tft.fillCircle(16, 148, 4, COL_CRYPTO);
  tft.setTextColor(COL_DIM, COL_BG);
  tft.setCursor(28, 141);
  tft.print("Waiting for configuration...");
}

static bool paramAdded = false;

void ensureParamAdded() {
  if (!paramAdded) {
    wm.addParameter(&paramFinnhub);
    paramAdded = true;
  }
}

void startConfigPortal() {
  drawSetupScreen();
  ensureParamAdded();
  wm.setSaveParamsCallback(saveParamCallback);
  wm.setConfigPortalTimeout(180);
  wm.startConfigPortal("Ticker-Setup");
}

// ── 종목 저장/로드 ────────────────────────────────────
void parseTickerList(const String& csv) {
  tickerCount = 0;
  int start = 0;
  while (start < (int)csv.length() && tickerCount < MAX_TICKERS) {
    int comma = csv.indexOf(',', start);
    if (comma < 0) comma = csv.length();
    String t = csv.substring(start, comma);
    t.trim();
    if (t.length() > 0) tickers[tickerCount++] = t;
    start = comma + 1;
  }
}

void saveTickers() {
  String joined, joinedNames;
  for (int i = 0; i < tickerCount; i++) {
    if (i > 0) { joined += ","; joinedNames += "|"; }
    joined += tickers[i];
    joinedNames += tickerNames[i];
  }
  prefs.begin("ticker", false);
  prefs.putString("tickers", joined);
  prefs.putString("tnames", joinedNames);
  prefs.end();
}

void loadTickers() {
  prefs.begin("ticker", true);
  String saved = prefs.getString("tickers", DEFAULT_TICKERS);
  String savedNames = prefs.getString("tnames", "");
  prefs.end();
  parseTickerList(saved);
  // tickerNames 로드 ('|' 구분)
  int idx = 0, start = 0;
  while (start <= (int)savedNames.length() && idx < tickerCount) {
    int sep = savedNames.indexOf('|', start);
    if (sep < 0) sep = savedNames.length();
    tickerNames[idx++] = savedNames.substring(start, sep);
    start = sep + 1;
  }
}

// Yahoo Finance에서 longName 조회 (yfSymbol: "AAPL", "005930.KS", "BTC-USD" 등)
String fetchLongName(const String& yfSymbol) {
  HTTPClient http;
  String url = "https://query1.finance.yahoo.com/v8/finance/chart/" + yfSymbol + "?interval=1d&range=1d";
  http.begin(url);
  http.setUserAgent("Mozilla/5.0");
  http.setTimeout(8000);
  if (http.GET() != 200) { http.end(); return ""; }
  String body = http.getString();
  http.end();
  JsonDocument doc;
  if (deserializeJson(doc, body)) return "";
  const char* name = doc["chart"]["result"][0]["meta"]["longName"] | "";
  return String(name);
}

// Yahoo Finance에서 현재가만 빠르게 조회 (차트 제외)
float fetchPriceYahoo(const String& yfSym) {
  HTTPClient http;
  String url = "https://query1.finance.yahoo.com/v8/finance/chart/" + yfSym + "?interval=1d&range=1d";
  http.begin(url);
  http.setUserAgent("Mozilla/5.0");
  http.setTimeout(8000);
  if (http.GET() != 200) { http.end(); return -1.0f; }
  String body = http.getString();
  http.end();
  JsonDocument doc;
  if (deserializeJson(doc, body)) return -1.0f;
  return doc["chart"]["result"][0]["meta"]["regularMarketPrice"] | -1.0f;
}

// 이름이 없는 종목의 shortName을 일괄 조회 (부팅 시 1회)
void fetchMissingNames() {
  bool changed = false;
  for (int i = 0; i < tickerCount; i++) {
    if (tickerNames[i].length() > 0) continue;  // 이미 있으면 skip
    String name;
    if (tickers[i].startsWith("COIN:")) {
      String sym = tickers[i].substring(5);      // BTC
      name = fetchLongName(sym + "-USD");        // BTC-USD
      if (name.endsWith(" USD")) name = name.substring(0, name.length() - 4);
      if (name.length() == 0) name = sym;
    } else if (tickers[i].startsWith("KR:")) {
      String code = tickers[i].substring(3);
      name = fetchLongName(code + ".KS");
      if (name.length() == 0) name = code;
    } else {
      name = fetchLongName(tickers[i]);
      if (name.length() == 0) name = tickers[i];
    }
    tickerNames[i] = name;
    changed = true;
  }
  if (changed) saveTickers();
}

void loadSettings() {
  prefs.begin("ticker", true);
  chartPeriodIdx = prefs.getInt("period", 0);
  if (chartPeriodIdx < 0 || chartPeriodIdx >= PERIOD_COUNT) chartPeriodIdx = 0;
  switchMs = prefs.getULong("switch_ms", 5000);
  if (switchMs < 2000) switchMs = 2000;
  if (switchMs > 60000) switchMs = 60000;
  dataRefreshMs = prefs.getULong("refresh_ms", 300000);
  if (dataRefreshMs < 60000) dataRefreshMs = 60000;
  if (dataRefreshMs > 3600000) dataRefreshMs = 3600000;
  priceRefreshMs = prefs.getULong("price_ms", 30000);
  if (priceRefreshMs < 10000) priceRefreshMs = 10000;
  if (priceRefreshMs > 3600000) priceRefreshMs = 3600000;
  String savedOta = prefs.getString("ota_url", "");
  strncpy(otaUrl, savedOta.c_str(), sizeof(otaUrl) - 1);
  prefs.end();
}

void saveSettings() {
  prefs.begin("ticker", false);
  prefs.putInt("period", chartPeriodIdx);
  prefs.putULong("switch_ms", switchMs);
  prefs.putULong("refresh_ms", dataRefreshMs);
  prefs.putULong("price_ms", priceRefreshMs);
  prefs.putString("ota_url", otaUrl);
  prefs.end();
}

// ── 웹 인터페이스 ────────────────────────────────────
String htmlEscape(const String& s) {
  String r;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if      (c == '<') r += "&lt;";
    else if (c == '>') r += "&gt;";
    else if (c == '&') r += "&amp;";
    else if (c == '"') r += "&quot;";
    else r += c;
  }
  return r;
}

static const char* COMMON_CSS =
  "body{font-family:-apple-system,BlinkMacSystemFont,Segoe UI,sans-serif;"
  "max-width:480px;margin:0 auto;padding:0 16px 32px;background:#111;color:#eee;}"
  ".topbar{display:flex;align-items:center;padding:16px 0 4px;}"
  ".topbar h1{flex:1;font-size:20px;margin:0;}"
  ".icon-btn{background:none;border:none;color:#888;font-size:22px;cursor:pointer;"
  "padding:4px 8px;border-radius:6px;text-decoration:none;line-height:1;}"
  ".icon-btn:hover{background:#222;color:#eee;}"
  ".sub{color:#888;font-size:13px;margin-bottom:16px;}"
  ".item{display:flex;align-items:center;padding:12px;background:#1e1e1e;"
  "border-radius:8px;margin-bottom:6px;}"
  ".sym{flex:1;font-weight:600;font-size:15px;}"
  ".tag{font-size:10px;padding:3px 8px;border-radius:10px;margin-right:10px;"
  "color:#000;font-weight:700;letter-spacing:0.5px;}"
  ".stock{background:#5b9bd5;}.crypto{background:#f6ad3d;}.kstock{background:#c454f0;}"
  "form{display:inline;margin:0;}"
  "button{padding:6px 12px;border:none;border-radius:6px;cursor:pointer;font-size:13px;}"
  ".del{background:transparent;color:#e57373;border:1px solid #444;}"
  ".del:hover{background:#e34c4c;color:#fff;border-color:#e34c4c;}"
  ".card{margin-top:20px;padding:16px;background:#1e1e1e;border-radius:8px;}"
  ".card h2{font-size:13px;color:#888;margin:0 0 14px;text-transform:uppercase;letter-spacing:1px;}"
  ".row{display:flex;gap:8px;}"
  ".label{font-size:12px;color:#888;margin-bottom:6px;}"
  "select,input[type=text],input[type=number],input[type=file]{"
  "padding:10px;background:#2a2a2a;color:#fff;border:1px solid #444;"
  "border-radius:6px;font-size:14px;}"
  "input[type=text],input[type=number]{flex:1;}"
  ".addbtn{background:#3666e6;color:#fff;padding:10px 20px;font-weight:600;}"
  ".empty{padding:20px;text-align:center;color:#666;font-style:italic;}"
  ".divider{border:none;border-top:1px solid #2a2a2a;margin:16px 0;}"
  ".wide-btn{width:100%;padding:11px;background:#2a2a2a;color:#aaa;"
  "border:1px solid #444;border-radius:6px;cursor:pointer;font-size:13px;margin-top:10px;}";

void handleRoot() {
  String html =
    "<!DOCTYPE html><html><head>"
    "<meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Ticker</title>"
    "<style>"; html += COMMON_CSS; html += "</style></head><body>";

  // ── 헤더 ──────────────────────────────────────────
  html += "<div class='topbar'>"
          "<h1>Ticker</h1>"
          "<a href='/config' class='icon-btn' title='Settings'>&#9881;</a>"
          "</div>"
          "<div class='sub'>" + String(tickerCount) + " / " + String(MAX_TICKERS) + " tickers</div>";

  // ── 종목 목록 ─────────────────────────────────────
  if (tickerCount == 0) {
    html += "<div class='empty'>No tickers. Add one below.</div>";
  } else {
    for (int i = 0; i < tickerCount; i++) {
      String code, tagClass, tagText;
      if (tickers[i].startsWith("COIN:")) {
        code = tickers[i].substring(5);
        tagClass = "crypto"; tagText = "CRYPTO";
      } else if (tickers[i].startsWith("KR:")) {
        code = tickers[i].substring(3);
        tagClass = "kstock"; tagText = "K-STOCK";
      } else {
        code = tickers[i];
        tagClass = "stock"; tagText = "STOCK";
      }
      // name: 저장된 shortName 우선, 없으면 코드
      String name = tickerNames[i].length() > 0 ? tickerNames[i] : code;
      String sub  = (name != code) ? "<br><span style='font-size:11px;color:#666;'>" + htmlEscape(code) + "</span>" : "";
      html += "<div class='item'>"
              "<span class='tag " + tagClass + "'>" + tagText + "</span>"
              "<span class='sym'>" + htmlEscape(name) + sub + "</span>"
              "<form method='POST' action='/delete'>"
              "<input type='hidden' name='idx' value='" + String(i) + "'>"
              "<button class='del'>Remove</button></form>"
              "</div>";
    }
  }

  // ── 종목 추가 ─────────────────────────────────────
  html += "<div class='card'><h2>Add Ticker</h2>"
          "<form method='POST' action='/add'><div class='row'>"
          "<select name='type'>"
          "<option value='stock'>STOCK (US)</option>"
          "<option value='crypto'>CRYPTO</option>"
          "<option value='kstock'>K-STOCK</option>"
          "</select>"
          "<input type='text' name='symbol' placeholder='AAPL / BTC / 005930'"
          " maxlength='10' required autocapitalize='characters'>"
          "<button class='addbtn'>Add</button>"
          "</div></form></div>"
          "<p style='color:#555;font-size:12px;margin-top:20px;text-align:center;line-height:1.7;'>"
          "Crypto: BTC, ETH, SOL, BNB, XRP, DOGE<br>"
          "K-Stock: 005930 (Samsung), 000660 (SK Hynix)</p>"
          "</body></html>";

  webServer.send(200, "text/html", html);
}

void handleConfig() {
  String html =
    "<!DOCTYPE html><html><head>"
    "<meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Settings</title>"
    "<style>"; html += COMMON_CSS; html += "</style></head><body>";

  // ── 헤더 ──────────────────────────────────────────
  html += "<div class='topbar'>"
          "<a href='/' class='icon-btn' title='Back'>&#8592;</a>"
          "<h1 style='text-align:center;'>Settings</h1>"
          "<span style='width:38px;'></span>"
          "</div>";

  // ── 디스플레이 설정 ───────────────────────────────
  html += "<div class='card'><h2>Display</h2>"
          "<form method='POST' action='/settings'>"
          "<div style='display:flex;flex-direction:column;gap:14px;'>"
          "<div><div class='label'>Chart Period</div>"
          "<div style='display:flex;gap:6px;'>";
  for (int i = 0; i < PERIOD_COUNT; i++) {
    bool sel = (i == chartPeriodIdx);
    html += "<button type='submit' name='period' value='" + String(i) +
            "' style='flex:1;padding:10px;border-radius:6px;border:1px solid #444;"
            "background:" + String(sel ? "#3666e6" : "#2a2a2a") +
            ";color:" + String(sel ? "#fff" : "#aaa") +
            ";font-weight:" + String(sel ? "600" : "400") + ";'>"
            + PERIODS[i].label + "</button>";
  }
  html += "</div></div>"
          "<div><div class='label'>Switch Interval (seconds)</div>"
          "<div class='row'>"
          "<input type='number' name='interval' min='2' max='60' value='" + String(switchMs / 1000) + "'>"
          "<button type='submit' name='save_interval' value='1' class='addbtn'>Save</button>"
          "</div></div>"
          "<div><div class='label'>Auto-Refresh Interval (minutes)</div>"
          "<div class='row'>"
          "<input type='number' name='refresh_min' min='1' max='60' value='" + String(dataRefreshMs / 60000) + "'>"
          "<button type='submit' name='save_refresh' value='1' class='addbtn'>Save</button>"
          "</div></div>"
          "<div><div class='label'>Price Refresh — Stock &amp; K-Stock only (seconds)</div>"
          "<div class='row'>"
          "<input type='number' name='price_sec' min='10' max='3600' value='" + String(priceRefreshMs / 1000) + "'>"
          "<button type='submit' name='save_price' value='1' class='addbtn'>Save</button>"
          "</div></div>"
          "</div></form></div>";

  // ── 펌웨어 업데이트 ───────────────────────────────
  html += "<div class='card'><h2>Firmware Update</h2>"
          "<div class='label'>Current version: <b style='color:#eee;'>v" FIRMWARE_VERSION "</b></div>"
          "<hr class='divider'>"
          "<div class='label'>Auto Update — Server URL</div>"
          "<form method='POST' action='/ota'>"
          "<div class='row'>"
          "<input type='text' name='ota_url' placeholder='http://192.168.1.x/firmware' value='" +
          htmlEscape(String(otaUrl)) + "'>"
          "<button type='submit' class='addbtn'>Save</button>"
          "</div>"
          "<div style='font-size:11px;color:#555;margin-top:8px;'>"
          "Place <b>version.txt</b> &amp; <b>firmware.bin</b> at the URL</div>"
          "</form>"
          "<form method='POST' action='/ota-now'>"
          "<button type='submit' class='wide-btn'>&#8635; Check Now</button>"
          "</form>"
          "<hr class='divider'>"
          "<div class='label'>Direct Upload (.bin)</div>"
          "<form method='POST' action='/upload' enctype='multipart/form-data'>"
          "<input type='file' name='firmware' accept='.bin' required"
          " style='width:100%;box-sizing:border-box;margin-bottom:8px;'>"
          "<button type='submit' class='addbtn' style='width:100%;'>Upload</button>"
          "</form>"
          "</div>"
          "</body></html>";

  webServer.send(200, "text/html", html);
}

void handleAdd() {
  if (tickerCount >= MAX_TICKERS) {
    webServer.send(400, "text/plain", "Max tickers reached");
    return;
  }
  String type = webServer.arg("type");
  String symbol = webServer.arg("symbol");
  symbol.trim();
  symbol.toUpperCase();

  if (symbol.length() > 0) {
    String full, name;
    if (type == "crypto") {
      full = "COIN:" + symbol;
      name = fetchLongName(symbol + "-USD");
      if (name.endsWith(" USD")) name = name.substring(0, name.length() - 4);
      if (name.length() == 0) name = symbol;
    } else if (type == "kstock") {
      full = "KR:" + symbol;
      name = fetchLongName(symbol + ".KS");
    } else {
      full = symbol;
      name = fetchLongName(symbol);        // US 주식도 shortName 조회
    }
    tickerNames[tickerCount] = name;
    tickers[tickerCount++] = full;
    saveTickers();
  }
  webServer.sendHeader("Location", "/");
  webServer.send(303);
}

void handleDelete() {
  int idx = webServer.arg("idx").toInt();
  if (idx >= 0 && idx < tickerCount) {
    for (int i = idx; i < tickerCount - 1; i++) {
      tickers[i] = tickers[i + 1];
      tickerNames[i] = tickerNames[i + 1];
    }
    tickers[tickerCount - 1] = "";
    tickerNames[tickerCount - 1] = "";
    tickerCount--;
    if (currentIndex >= tickerCount) currentIndex = 0;
    saveTickers();
  }
  webServer.sendHeader("Location", "/");
  webServer.send(303);
}

void handleSettings() {
  bool changed = false;
  // 차트 기간 변경 (period 버튼 클릭)
  if (webServer.hasArg("period")) {
    int p = webServer.arg("period").toInt();
    if (p >= 0 && p < PERIOD_COUNT && p != chartPeriodIdx) {
      chartPeriodIdx = p;
      changed = true;
    }
  }
  // 전환 주기 저장
  if (webServer.hasArg("save_interval")) {
    int sec = webServer.arg("interval").toInt();
    if (sec >= 2 && sec <= 60) {
      switchMs = (unsigned long)sec * 1000UL;
      changed = true;
    }
  }
  // 자동 갱신 주기 저장
  if (webServer.hasArg("save_refresh")) {
    int mins = webServer.arg("refresh_min").toInt();
    if (mins >= 1 && mins <= 60) {
      dataRefreshMs = (unsigned long)mins * 60000UL;
      lastDataRefresh = millis();
      changed = true;
    }
  }
  // 가격 갱신 주기 저장
  if (webServer.hasArg("save_price")) {
    int sec = webServer.arg("price_sec").toInt();
    if (sec >= 10 && sec <= 3600) {
      priceRefreshMs = (unsigned long)sec * 1000UL;
      lastPriceRefresh = millis();
      changed = true;
    }
  }
  if (changed) saveSettings();

  webServer.sendHeader("Location", "/config");
  webServer.send(303);
}

void handleOtaSave() {
  String url = webServer.arg("ota_url");
  url.trim();
  strncpy(otaUrl, url.c_str(), sizeof(otaUrl) - 1);
  otaUrl[sizeof(otaUrl) - 1] = '\0';
  saveSettings();
  webServer.sendHeader("Location", "/config");
  webServer.send(303);
}

void handleOtaNow() {
  webServer.sendHeader("Location", "/config");
  webServer.send(303);
  checkOtaUpdate();
}

void handleUploadDone() {
  bool ok = !Update.hasError();
  String html =
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>OTA</title>"
    "<style>body{font-family:-apple-system,sans-serif;max-width:480px;margin:60px auto;"
    "padding:20px;background:#111;color:#eee;text-align:center;}"
    "h2{font-size:24px;} .ok{color:#3c6;} .err{color:#e44;}"
    "a{color:#5b9;}</style></head><body>";
  if (ok) {
    html += "<h2 class='ok'>Upload successful!</h2>"
            "<p>Device is restarting...</p>";
  } else {
    html += "<h2 class='err'>Upload failed</h2>"
            "<p>" + String(Update.errorString()) + "</p>"
            "<p><a href='/config'>Back</a></p>";
  }
  html += "</body></html>";
  webServer.send(200, "text/html", html);
  if (ok) { delay(1000); ESP.restart(); }
}

void handleUploadFile() {
  HTTPUpload& upload = webServer.upload();
  if (upload.status == UPLOAD_FILE_START) {
    Serial.printf("[WebOTA] Upload start: %s\n", upload.filename.c_str());
    drawOtaScreen(0, "Uploading via browser...");
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize)
      Update.printError(Serial);
    // 진행률은 total 크기를 모르므로 수신 바이트만 표시
    Serial.printf("[WebOTA] Written %u bytes\n", upload.totalSize);
  } else if (upload.status == UPLOAD_FILE_END) {
    if (Update.end(true)) {
      drawOtaScreen(100, "Done! Restarting...");
      Serial.printf("[WebOTA] Success: %u bytes\n", upload.totalSize);
    } else {
      Update.printError(Serial);
    }
  }
}

// 종목 데이터 페치 후 화면 표시 (이름 오버라이드 포함)
// lastPriceRefresh는 건드리지 않음 — 가격 갱신 타이머는 종목 전환과 독립
void doFetchAndDraw(int idx) {
  currentData = fetchTicker(tickers[idx].c_str());
  if (tickerNames[idx].length() > 0) {
    strncpy(currentData.symbol, tickerNames[idx].c_str(), sizeof(currentData.symbol) - 1);
    currentData.symbol[sizeof(currentData.symbol) - 1] = '\0';
  }
  drawTicker(currentData);
}

void setupWebServer() {
  webServer.on("/", HTTP_GET, handleRoot);
  webServer.on("/config", HTTP_GET, handleConfig);
  webServer.on("/add", HTTP_POST, handleAdd);
  webServer.on("/delete", HTTP_POST, handleDelete);
  webServer.on("/settings", HTTP_POST, handleSettings);
  webServer.on("/ota", HTTP_POST, handleOtaSave);
  webServer.on("/ota-now", HTTP_POST, handleOtaNow);
  webServer.on("/upload", HTTP_POST, handleUploadDone, handleUploadFile);
  webServer.begin();
}

// ── 데이터 페치 ──────────────────────────────────────

// 차트 min/max 계산
void computeChartRange(TickerData& d) {
  if (d.chartCount == 0) return;
  d.chartMin = d.chart[0];
  d.chartMax = d.chart[0];
  for (int i = 1; i < d.chartCount; i++) {
    if (d.chart[i] < d.chartMin) d.chartMin = d.chart[i];
    if (d.chart[i] > d.chartMax) d.chartMax = d.chart[i];
  }
}

// Yahoo Finance: US/KR 주식 모두 처리 (suffix="" 이면 US, ".KS" 이면 KR)
TickerData fetchYahoo(const char* symbol, const char* suffix, AssetType type) {
  TickerData data = {};
  data.type = type;
  strncpy(data.symbol, symbol, sizeof(data.symbol) - 1);

  const ChartPeriod& p = PERIODS[chartPeriodIdx];

  HTTPClient http;
  String url = "https://query1.finance.yahoo.com/v8/finance/chart/";
  url += symbol;
  url += suffix;
  url += "?interval="; url += p.yInterval;
  url += "&range=";    url += p.yRange;

  Serial.printf("[Yahoo] %s%s period=%s → GET %s\n",
                symbol, suffix, p.label, url.c_str());

  unsigned long t0 = millis();
  http.begin(url);
  http.setUserAgent("Mozilla/5.0");

  int status = http.GET();
  unsigned long elapsed = millis() - t0;

  if (status != 200) {
    Serial.printf("[Yahoo] HTTP %d (%lums)\n", status, elapsed);
    http.end();
    return data;
  }

  String body = http.getString();
  Serial.printf("[Yahoo] HTTP 200 (%lums, %u bytes)\n", elapsed, body.length());

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    Serial.printf("[Yahoo] JSON error: %s\n", err.c_str());
    http.end();
    return data;
  }

  auto result = doc["chart"]["result"][0];
  auto meta = result["meta"];
  data.price    = meta["regularMarketPrice"].as<float>();
  data.prevClose = meta["chartPreviousClose"].as<float>();
  if (data.prevClose == 0) data.prevClose = meta["previousClose"].as<float>();
  data.valid = data.price > 0;

  // 주식: longName을 심볼로 사용 (API에서 직접 취득)
  if (type == TYPE_KSTOCK || type == TYPE_STOCK) {
    const char* name = meta["longName"] | "";
    if (strlen(name) > 0) {
      strncpy(data.symbol, name, sizeof(data.symbol) - 1);
      data.symbol[sizeof(data.symbol) - 1] = '\0';
    }
  }

  auto closes = result["indicators"]["quote"][0]["close"];
  int total = 0;
  for (JsonVariant cv : closes.as<JsonArray>()) if (!cv.isNull()) total++;
  int step = (total > MAX_CHART_POINTS) ? total / MAX_CHART_POINTS : 1;
  int seen = 0;
  for (JsonVariant cv : closes.as<JsonArray>()) {
    if (cv.isNull()) continue;
    if (seen % step == 0 && data.chartCount < MAX_CHART_POINTS) {
      data.chart[data.chartCount++] = cv.as<float>();
    }
    seen++;
  }

  // change% 기준: 전일종가 우선, 없으면 첫 차트 포인트
  float base = (data.prevClose > 0) ? data.prevClose
             : (data.chartCount > 0) ? data.chart[0] : 0;
  data.change = (base > 0) ? ((data.price - base) / base) * 100.0f : 0;
  computeChartRange(data);

  Serial.printf("[Yahoo] price=%.4f prevClose=%.4f change=%+.2f%% points=%d (raw %d, step %d)\n",
                data.price, data.prevClose, data.change, data.chartCount, total, step);

  http.end();
  return data;
}

TickerData fetchStock(const char* symbol) {
  return fetchYahoo(symbol, "", TYPE_STOCK);
}
TickerData fetchKoreanStock(const char* code) {
  return fetchYahoo(code, ".KS", TYPE_KSTOCK);
}

TickerData fetchCrypto(const char* id) {
  TickerData data = {};
  data.type = TYPE_CRYPTO;
  struct { const char* abbr; const char* cgId; } map[] = {
    {"BTC", "bitcoin"}, {"ETH", "ethereum"}, {"SOL", "solana"},
    {"BNB", "binancecoin"}, {"XRP", "ripple"}, {"DOGE", "dogecoin"}
  };
  const char* cgId = nullptr;
  for (auto& m : map)
    if (strcmp(id, m.abbr) == 0) { cgId = m.cgId; break; }
  if (!cgId) return data;

  snprintf(data.symbol, sizeof(data.symbol), "%s", id);

  // OHLC 엔드포인트: market_chart보다 응답이 훨씬 작아 1M/1Y도 안전하게 처리
  HTTPClient http;
  String url = "https://api.coingecko.com/api/v3/coins/";
  url += cgId;
  url += "/ohlc?vs_currency=usd&days=";
  url += String(PERIODS[chartPeriodIdx].cgDays);

  Serial.printf("[Crypto] %s (%s) period=%s → GET %s\n",
                id, cgId, PERIODS[chartPeriodIdx].label, url.c_str());

  unsigned long t0 = millis();
  http.begin(url);
  http.addHeader("accept", "application/json");
  http.setTimeout(15000);

  int code = http.GET();
  unsigned long elapsed = millis() - t0;

  if (code != 200) {
    Serial.printf("[Crypto] HTTP %d (%lums)\n", code, elapsed);
    http.end();
    return data;
  }

  String body = http.getString();
  Serial.printf("[Crypto] HTTP 200 (%lums, %u bytes)\n", elapsed, body.length());

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    Serial.printf("[Crypto] JSON error: %s\n", err.c_str());
    http.end();
    return data;
  }

  // 응답: [[ts, open, high, low, close], ...]
  auto items = doc.as<JsonArray>();
  int total = items.size();
  int step = total > MAX_CHART_POINTS ? total / MAX_CHART_POINTS : 1;
  int i = 0;
  for (JsonVariant item : items) {
    if (i % step == 0 && data.chartCount < MAX_CHART_POINTS) {
      data.chart[data.chartCount++] = item[4].as<float>();
    }
    i++;
  }

  http.end();

  if (data.chartCount == 0) return data;

  float firstOhlc = data.chart[0];

  // simple/price 로 실시간 현재가 취득 (OHLC 캔들 지연 최대 30분 보정)
  HTTPClient http2;
  String priceUrl = "https://api.coingecko.com/api/v3/simple/price?ids=";
  priceUrl += cgId;
  priceUrl += "&vs_currencies=usd";
  Serial.printf("[Crypto] simple/price → GET %s\n", priceUrl.c_str());

  unsigned long t1 = millis();
  http2.begin(priceUrl);
  http2.addHeader("accept", "application/json");
  http2.setTimeout(10000);
  int code2 = http2.GET();
  Serial.printf("[Crypto] simple/price HTTP %d (%lums)\n", code2, millis() - t1);

  if (code2 == 200) {
    String body2 = http2.getString();
    JsonDocument doc2;
    if (!deserializeJson(doc2, body2)) {
      float livePrice = doc2[cgId]["usd"].as<float>();
      if (livePrice > 0) data.price = livePrice;
    }
  }
  http2.end();

  data.change = firstOhlc > 0 ? ((data.price - firstOhlc) / firstOhlc) * 100.0f : 0;
  data.valid = true;
  computeChartRange(data);

  Serial.printf("[Crypto] price=%.4f(live) change=%+.2f%% points=%d (raw %d, step %d)\n",
                data.price, data.change, data.chartCount, total, step);

  return data;
}

TickerData fetchTicker(const char* ticker) {
  Serial.printf("\n─── fetchTicker(\"%s\") [%d/%d] ───\n",
                ticker, currentIndex + 1, tickerCount);
  TickerData d;
  if (strncmp(ticker, "COIN:", 5) == 0)      d = fetchCrypto(ticker + 5);
  else if (strncmp(ticker, "KR:", 3) == 0)   d = fetchKoreanStock(ticker + 3);
  else                                       d = fetchStock(ticker);

  if (!d.valid) Serial.println("[!] fetch failed — display will show Load Failed");
  Serial.println();
  return d;
}

// ── 디스플레이 (가로 모드 320 x 170) ─────────────────
//  ┌────────────────────────────────────────────┐
//  │ [TYPE]                              1 / 6  │  y=5~25 헤더
//  │                                            │
//  │  BTC               $43,521.67              │  y=40~95 심볼+가격
//  │  ────                                      │
//  │                    ┌─────────────┐         │
//  │                    │ ▲ +2.34%    │         │  y=98~125 변화율 배지
//  │                    └─────────────┘         │
//  │  ──────────────●──────────────────         │  y=132 강도 바
//  │ ─────────────────────────────────          │  y=145 디바이더
//  │ ● Remote Connected                         │  y=150~ 상태
//  └────────────────────────────────────────────┘

void drawStatusBar() {
  // 하단 디바이더
  tft.drawFastHLine(10, 145, 300, COL_DIVIDER);

  // 영역 클리어
  tft.fillRect(0, 150, 320, 18, COL_BG);
  tft.setTextFont(2);
  tft.setTextSize(1);

  // 좌측: 리모컨 상태 도트 + 텍스트
  if (bleConnected) {
    tft.fillCircle(16, 160, 4, COL_UP);
    tft.setTextColor(COL_UP, COL_BG);
    tft.setCursor(28, 153);
    tft.print("Remote ON");
  } else {
    tft.fillCircle(16, 160, 4, COL_DIM);
    tft.setTextColor(COL_DIM, COL_BG);
    tft.setCursor(28, 153);
    tft.print("Remote --");
  }

  // 우측: IP 주소 (설정용)
  if (WiFi.status() == WL_CONNECTED) {
    String ip = WiFi.localIP().toString();
    tft.setTextColor(COL_DIM, COL_BG);
    int ipW = tft.textWidth(ip.c_str());
    tft.setCursor(310 - ipW, 153);
    tft.print(ip);
  }
}

void drawHeader(const TickerData& d) {
  // 좌측: 타입 태그
  const char* tag;
  uint16_t tagColor;
  int tagW;
  switch (d.type) {
    case TYPE_CRYPTO:  tag = "CRYPTO";  tagColor = COL_CRYPTO;  tagW = 56; break;
    case TYPE_KSTOCK:  tag = "K-STOCK"; tagColor = COL_KSTOCK;  tagW = 62; break;
    default:           tag = "STOCK";   tagColor = COL_STOCK;   tagW = 50; break;
  }
  tft.fillRoundRect(10, 6, tagW, 18, 4, tagColor);
  tft.setTextFont(2);
  tft.setTextSize(1);
  tft.setTextColor(COL_BG, tagColor);
  tft.setCursor(15, 8);
  tft.print(tag);

  // 우측: 차트 기간 (밝게) + 구분점 + 인덱스 카운터 (흐리게)
  tft.setTextFont(2);
  tft.setTextSize(1);

  const char* periodStr = PERIODS[chartPeriodIdx].display;
  char idxBuf[16];
  snprintf(idxBuf, sizeof(idxBuf), "   %d / %d", currentIndex + 1, tickerCount);

  int periodW = tft.textWidth(periodStr);
  int idxW = tft.textWidth(idxBuf);
  int startX = 310 - periodW - idxW;

  // 기간 라벨 — 밝은 흰색으로 강조
  tft.setTextColor(COL_TEXT, COL_BG);
  tft.setCursor(startX, 8);
  tft.print(periodStr);

  // 인덱스 — 회색으로 보조 정보 처리
  tft.setTextColor(COL_DIM, COL_BG);
  tft.print(idxBuf);
}

void drawNoTickers() {
  tft.fillScreen(COL_BG);

  tft.setTextFont(4);
  tft.setTextSize(1);
  tft.setTextColor(COL_DIM, COL_BG);
  const char* msg = "No Tickers";
  int w = tft.textWidth(msg);
  tft.setCursor((320 - w) / 2, 50);
  tft.print(msg);

  tft.setTextFont(2);
  tft.setTextSize(1);
  tft.setTextColor(COL_DIM, COL_BG);
  const char* sub = "Open Web UI to add";
  int sw = tft.textWidth(sub);
  tft.setCursor((320 - sw) / 2, 95);
  tft.print(sub);

  drawStatusBar();
}

void drawLoading(const char* symbol) {
  tft.fillScreen(COL_BG);
  drawStatusBar();

  tft.setTextFont(4);
  tft.setTextSize(2);
  tft.setTextColor(COL_DIM, COL_BG);
  int sw = tft.textWidth(symbol);
  tft.setCursor((320 - sw) / 2, 50);
  tft.print(symbol);

  tft.setTextFont(2);
  tft.setTextSize(1);
  const char* msg = "Loading...";
  int mw = tft.textWidth(msg);
  tft.setCursor((320 - mw) / 2, 110);
  tft.print(msg);
}

// 차트 그리기 (라인 + 베이스라인 + 영역 채움)
void drawChart(const TickerData& d, int x, int y, int w, int h) {
  if (d.chartCount < 2) return;

  uint16_t lineColor = (d.change >= 0) ? COL_UP : COL_DOWN;
  // 어두운 그라데이션 색 (alpha 흉내)
  uint16_t fillColor = (d.change >= 0) ? 0x0BC4 : 0x4862;

  float vmin = d.chartMin;
  float vmax = d.chartMax;
  float range = vmax - vmin;
  if (range < 0.0001f) range = 1;
  // 패딩 5%
  vmin -= range * 0.05f;
  vmax += range * 0.05f;
  range = vmax - vmin;

  auto yAt = [&](float v) -> int {
    return y + h - (int)((v - vmin) / range * h);
  };

  // 시작가 (전일 종가) 베이스라인 점선
  if (d.chartCount > 0) {
    float baseline = d.chart[0];
    int by = yAt(baseline);
    if (by >= y && by < y + h) {
      for (int i = x; i < x + w; i += 4) {
        tft.drawPixel(i, by, COL_DIVIDER);
      }
    }
  }

  // 영역 채움 (각 x에서 라인까지 수직선)
  for (int i = 0; i < d.chartCount - 1; i++) {
    int x1 = x + i * w / (d.chartCount - 1);
    int x2 = x + (i + 1) * w / (d.chartCount - 1);
    int y1 = yAt(d.chart[i]);
    int y2 = yAt(d.chart[i + 1]);
    // x1~x2 구간을 보간하며 fill
    for (int px = x1; px < x2; px++) {
      float t = (x2 == x1) ? 0 : (float)(px - x1) / (x2 - x1);
      int py = y1 + (int)((y2 - y1) * t);
      if (py < y + h) {
        tft.drawFastVLine(px, py, y + h - py, fillColor);
      }
    }
  }

  // 라인 (2px 두께)
  for (int i = 1; i < d.chartCount; i++) {
    int x1 = x + (i - 1) * w / (d.chartCount - 1);
    int x2 = x + i * w / (d.chartCount - 1);
    int y1 = yAt(d.chart[i - 1]);
    int y2 = yAt(d.chart[i]);
    tft.drawLine(x1, y1, x2, y2, lineColor);
    tft.drawLine(x1, y1 + 1, x2, y2 + 1, lineColor);
  }
}

void drawTicker(const TickerData& d) {
  tft.fillScreen(COL_BG);
  drawHeader(d);

  if (!d.valid) {
    tft.setTextFont(4);
    tft.setTextSize(1);
    tft.setTextColor(COL_DOWN, COL_BG);
    const char* msg = "Load Failed";
    int w = tft.textWidth(msg);
    tft.setCursor((320 - w) / 2, 75);
    tft.print(msg);
    drawStatusBar();
    return;
  }

  bool up = d.change >= 0;
  uint16_t chColor = up ? COL_UP : COL_DOWN;
  uint16_t accent = (d.type == TYPE_CRYPTO) ? COL_CRYPTO :
                    (d.type == TYPE_KSTOCK) ? COL_KSTOCK : COL_STOCK;

  // ── 좌측: 심볼 + 변화율 ─────────────────────────────
  // Font4/size1(26px) 고정 — 넘칠 경우 "..."으로 잘라서 표시
  tft.setTextFont(4); tft.setTextSize(1);
  const int NAME_MAX_W = 195;
  char dispName[28];
  strncpy(dispName, d.symbol, sizeof(dispName) - 1);
  dispName[sizeof(dispName) - 1] = '\0';
  int symW = tft.textWidth(dispName);
  if (symW > NAME_MAX_W) {
    int len = strlen(dispName);
    while (len > 1) {
      dispName[--len] = '\0';
      char tmp[32];
      snprintf(tmp, sizeof(tmp), "%s...", dispName);
      if (tft.textWidth(tmp) <= NAME_MAX_W) {
        strncpy(dispName, tmp, sizeof(dispName) - 1);
        break;
      }
    }
    symW = tft.textWidth(dispName);
  }
  tft.setTextColor(COL_TEXT, COL_BG);
  tft.setCursor(15, 38);
  tft.print(dispName);
  // 액센트 라인
  tft.drawFastHLine(15, 64, symW, accent);

  // 변화율 (심볼 아래, 화살표 + 텍스트)
  char chBuf[16];
  snprintf(chBuf, sizeof(chBuf), "%+.2f%%", d.change);
  tft.setTextFont(2);
  tft.setTextSize(2);
  int arrowX = 15;
  int arrowCY = 84;
  if (up) {
    tft.fillTriangle(arrowX,     arrowCY + 5,
                     arrowX + 10, arrowCY + 5,
                     arrowX + 5,  arrowCY - 5, chColor);
  } else {
    tft.fillTriangle(arrowX,     arrowCY - 5,
                     arrowX + 10, arrowCY - 5,
                     arrowX + 5,  arrowCY + 5, chColor);
  }
  tft.setTextColor(chColor, COL_BG);
  tft.setCursor(arrowX + 16, arrowCY - 8);
  tft.print(chBuf);

  // ── 우측: 가격 ───────────────────────────────────
  char priceBuf[24];
  if (d.type == TYPE_KSTOCK) {
    long whole = (long)(d.price + 0.5f);
    char raw[16];
    snprintf(raw, sizeof(raw), "%ld", whole);
    int len = strlen(raw);
    int j = 0;
    priceBuf[j++] = 'W';
    for (int i = 0; i < len; i++) {
      if (i > 0 && (len - i) % 3 == 0) priceBuf[j++] = ',';
      priceBuf[j++] = raw[i];
    }
    priceBuf[j] = '\0';
  } else if (d.price >= 1) {
    snprintf(priceBuf, sizeof(priceBuf), "$%.2f", d.price);
  } else {
    snprintf(priceBuf, sizeof(priceBuf), "$%.4f", d.price);
  }

  tft.setTextFont(4);
  tft.setTextSize(1);
  tft.setTextColor(COL_PRICE, COL_BG);
  int pw = tft.textWidth(priceBuf);
  tft.setCursor(310 - pw, 35);
  tft.print(priceBuf);

  // ── 전일종가 (주식만, 현재가 아래) ──────────────────
  if (d.prevClose > 0) {
    char prevBuf[28];
    if (d.type == TYPE_KSTOCK) {
      long whole = (long)(d.prevClose + 0.5f);
      char raw[16]; snprintf(raw, sizeof(raw), "%ld", whole);
      int len = strlen(raw), j = 0;
      prevBuf[j++] = 'W';
      for (int i = 0; i < len; i++) {
        if (i > 0 && (len - i) % 3 == 0) prevBuf[j++] = ',';
        prevBuf[j++] = raw[i];
      }
      prevBuf[j] = '\0';
    } else if (d.prevClose >= 1) {
      snprintf(prevBuf, sizeof(prevBuf), "$%.2f", d.prevClose);
    } else {
      snprintf(prevBuf, sizeof(prevBuf), "$%.4f", d.prevClose);
    }
    tft.setTextFont(2);
    tft.setTextSize(1);
    tft.setTextColor(COL_DIM, COL_BG);
    int prevLabelW = tft.textWidth("Prev ");
    int prevValW   = tft.textWidth(prevBuf);
    tft.setCursor(310 - prevLabelW - prevValW, 68);
    tft.print("Prev ");
    tft.setTextColor(COL_TEXT, COL_BG);
    tft.print(prevBuf);
  }

  // ── 차트 (하단 전체 폭) ───────────────────────────
  drawChart(d, 15, 100, 290, 40);

  drawStatusBar();
}

// ── BLE 서버 ──────────────────────────────────────────
#define BLE_DEVICE_NAME "TickerDisplay"
#define BLE_SERVICE_UUID  "4FAFC201-1FB5-459E-8FCC-C5C9C331914B"
#define BLE_CHAR_UUID     "BEB5483E-36E1-4688-B7F5-EA07361B26A8"

volatile bool nextTickerRequested = false;

class RemoteCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic*) override {
    nextTickerRequested = true;
  }
};

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer*) override {
    bleConnected = true;
  }
  void onDisconnect(BLEServer*) override {
    bleConnected = false;
    // 클라이언트 끊김 후 광고 재시작 — 없으면 재연결 불가
    BLEDevice::getAdvertising()->start();
  }
};

void setupBLE() {
  BLEDevice::init(BLE_DEVICE_NAME);
  BLEServer* pServer = BLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());
  BLEService* pService = pServer->createService(BLE_SERVICE_UUID);
  BLECharacteristic* pChar = pService->createCharacteristic(
      BLE_CHAR_UUID, BLECharacteristic::PROPERTY_WRITE);
  pChar->setCallbacks(new RemoteCallbacks());
  pService->start();
  BLEAdvertising* pAdv = BLEDevice::getAdvertising();
  pAdv->addServiceUUID(BLE_SERVICE_UUID);
  pAdv->setScanResponse(true);
  pAdv->start();
}

// ── Setup ─────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  pinMode(0, INPUT_PULLUP);

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);
  tft.init();
  tft.setRotation(1);   // 가로 모드 (320 x 170)
  tft.fillScreen(TFT_BLACK);

  // 저장된 Finnhub 키 로드
  prefs.begin("ticker", true);
  String saved = prefs.getString("finnhub_key", "");
  prefs.end();
  strncpy(finnhubKey, saved.c_str(), sizeof(finnhubKey) - 1);

  // BOOT 버튼 3초 홀드 → 설정 초기화 후 포털 재진입
  if (digitalRead(0) == LOW) {
    delay(3000);
    if (digitalRead(0) == LOW) {
      wm.resetSettings();
      startConfigPortal();
      ESP.restart();
    }
  }

  // WiFi 연결 시도 — 저장된 자격증명 없으면 포털 열림
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextFont(2);
  tft.setCursor(10, 10);
  tft.print("Connecting WiFi...");

  ensureParamAdded();
  wm.setSaveParamsCallback(saveParamCallback);
  wm.setConfigPortalTimeout(180);

  if (!wm.autoConnect("Ticker-Setup")) {
    tft.fillScreen(TFT_BLACK);
    tft.setCursor(10, 140);
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.print("WiFi connect failed\nRestarting...");
    delay(3000);
    ESP.restart();
  }

  // WiFiManager 포털에서 키를 새로 입력했다면 이미 저장됨
  // 포털을 거치지 않았다면 NVS에서 로드한 값 사용
  if (strlen(finnhubKey) == 0) {
    prefs.begin("ticker", true);
    String k = prefs.getString("finnhub_key", "");
    prefs.end();
    strncpy(finnhubKey, k.c_str(), sizeof(finnhubKey) - 1);
  }

  loadTickers();
  loadSettings();
  fetchMissingNames();   // 기존 종목의 누락된 이름 일괄 조회
  setupWebServer();
  Serial.printf("Web UI: http://%s/\n", WiFi.localIP().toString().c_str());

  setupBLE();
  Serial.println("BLE advertising as 'TickerDisplay'");

  if (tickerCount > 0) {
    drawLoading(tickers[currentIndex].c_str());
    doFetchAndDraw(currentIndex);
  } else {
    drawNoTickers();
  }
  lastRefresh = millis();
  lastDataRefresh = millis();
  lastPriceRefresh = millis();  // 부팅 후 priceRefreshMs 뒤에 첫 갱신

  // 부팅 시 OTA 체크
  checkOtaUpdate();
  lastOtaCheck = millis();
}

// BOOT 버튼: 짧게 누름 → 종목 전환, 3초 홀드 → 설정 초기화
void handleButton() {
  static bool btnLast = HIGH;
  static unsigned long btnPressedAt = 0;
  static bool longPressFired = false;

  bool btnNow = digitalRead(0);

  if (btnLast == HIGH && btnNow == LOW) {
    // 눌리기 시작
    btnPressedAt = millis();
    longPressFired = false;
  } else if (btnNow == LOW && !longPressFired) {
    // 계속 눌림 — 3초 경과 감지
    if (millis() - btnPressedAt >= 3000) {
      longPressFired = true;
      wm.resetSettings();
      startConfigPortal();
      ESP.restart();
    }
  } else if (btnLast == LOW && btnNow == HIGH) {
    // 손 뗌
    if (!longPressFired && (millis() - btnPressedAt) >= 50) {
      nextTickerRequested = true;  // 짧게 눌렀음 → 종목 전환
    }
  }
  btnLast = btnNow;
}

void loop() {
  handleButton();
  webServer.handleClient();

  // 종목이 없으면 안내 화면
  if (tickerCount == 0) {
    static bool noTickerShown = false;
    if (!noTickerShown) {
      drawNoTickers();
      noTickerShown = true;
    }
    // 종목이 추가되면 다시 그릴 수 있도록
    static unsigned long lastCheck = 0;
    if (millis() - lastCheck > 1000) {
      lastCheck = millis();
      if (tickerCount > 0) noTickerShown = false;
    }
    return;
  }

  // OTA 주기 체크 (1시간마다)
  if (millis() - lastOtaCheck >= OTA_CHECK_INTERVAL) {
    lastOtaCheck = millis();
    checkOtaUpdate();
  }

  // 전체 데이터 갱신 (차트 포함, dataRefreshMs 마다)
  if (millis() - lastDataRefresh >= dataRefreshMs) {
    lastDataRefresh = millis();
    lastRefresh = millis();
    drawLoading(tickers[currentIndex].c_str());
    doFetchAndDraw(currentIndex);
    return;
  }

  // 종목 전환 (switchMs 마다 또는 버튼 누름)
  // lastPriceRefresh 건드리지 않음 — 가격 타이머는 독립적으로 흐름
  if (nextTickerRequested || millis() - lastRefresh >= switchMs) {
    nextTickerRequested = false;
    currentIndex = (currentIndex + 1) % tickerCount;
    lastRefresh = millis();
    lastDataRefresh = millis();
    drawLoading(tickers[currentIndex].c_str());
    doFetchAndDraw(currentIndex);
    return;
  }

  // 가격 빠른 갱신 (주식/한국주식만, priceRefreshMs 마다)
  if (currentData.valid && currentData.type != TYPE_CRYPTO &&
      millis() - lastPriceRefresh >= priceRefreshMs) {
    lastPriceRefresh = millis();
    String yfSym = tickers[currentIndex].startsWith("KR:")
                   ? tickers[currentIndex].substring(3) + ".KS"
                   : tickers[currentIndex];
    float newPrice = fetchPriceYahoo(yfSym);
    if (newPrice > 0) {
      currentData.price = newPrice;
      float base = (currentData.prevClose > 0) ? currentData.prevClose
                 : (currentData.chartCount > 0) ? currentData.chart[0] : 0;
      currentData.change = (base > 0) ? ((newPrice - base) / base) * 100.0f : 0;
      Serial.printf("[PriceRefresh] %s → %.4f (%+.2f%%)\n",
                    yfSym.c_str(), newPrice, currentData.change);
      drawTicker(currentData);
    }
    return;
  }

  // BLE 연결 상태 변경 시 상태바만 갱신
  static bool lastBleState = false;
  if (bleConnected != lastBleState) {
    lastBleState = bleConnected;
    drawStatusBar();
  }
}
