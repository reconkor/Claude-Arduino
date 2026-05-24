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

String tickers[MAX_TICKERS];
int tickerCount = 0;
WebServer webServer(80);

TFT_eSPI tft = TFT_eSPI();
Preferences prefs;

enum AssetType { TYPE_STOCK, TYPE_CRYPTO, TYPE_KSTOCK };

const int MAX_CHART_POINTS = 60;

struct TickerData {
  char symbol[16];
  float price;
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

// ── WiFiManager ───────────────────────────────────────
WiFiManager wm;
WiFiManagerParameter paramFinnhub("finnhub_key", "Finnhub API Key", "", 63);

void saveParamCallback() {
  strncpy(finnhubKey, paramFinnhub.getValue(), sizeof(finnhubKey) - 1);
  prefs.begin("ticker", false);
  prefs.putString("finnhub_key", finnhubKey);
  prefs.end();
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
  String joined;
  for (int i = 0; i < tickerCount; i++) {
    if (i > 0) joined += ",";
    joined += tickers[i];
  }
  prefs.begin("ticker", false);
  prefs.putString("tickers", joined);
  prefs.end();
}

void loadTickers() {
  prefs.begin("ticker", true);
  String saved = prefs.getString("tickers", DEFAULT_TICKERS);
  prefs.end();
  parseTickerList(saved);
}

void loadSettings() {
  prefs.begin("ticker", true);
  chartPeriodIdx = prefs.getInt("period", 0);
  if (chartPeriodIdx < 0 || chartPeriodIdx >= PERIOD_COUNT) chartPeriodIdx = 0;
  switchMs = prefs.getULong("switch_ms", 5000);
  if (switchMs < 2000) switchMs = 2000;
  if (switchMs > 60000) switchMs = 60000;
  prefs.end();
}

void saveSettings() {
  prefs.begin("ticker", false);
  prefs.putInt("period", chartPeriodIdx);
  prefs.putULong("switch_ms", switchMs);
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

void handleRoot() {
  String html =
    "<!DOCTYPE html><html><head>"
    "<meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Ticker Settings</title>"
    "<style>"
    "body{font-family:-apple-system,BlinkMacSystemFont,Segoe UI,sans-serif;"
    "max-width:480px;margin:0 auto;padding:20px 16px;background:#111;color:#eee;}"
    "h1{font-size:22px;margin:0 0 4px;}"
    ".sub{color:#888;font-size:13px;margin-bottom:20px;}"
    ".item{display:flex;align-items:center;padding:12px;background:#1e1e1e;"
    "border-radius:8px;margin-bottom:6px;}"
    ".sym{flex:1;font-weight:600;font-size:15px;}"
    ".tag{font-size:10px;padding:3px 8px;border-radius:10px;margin-right:10px;"
    "color:#000;font-weight:700;letter-spacing:0.5px;}"
    ".stock{background:#5b9bd5;} .crypto{background:#f6ad3d;} .kstock{background:#c454f0;}"
    "form{display:inline;margin:0;}"
    "button{padding:6px 12px;border:none;border-radius:6px;cursor:pointer;font-size:13px;}"
    ".del{background:transparent;color:#e57373;border:1px solid #444;}"
    ".del:hover{background:#e34c4c;color:#fff;border-color:#e34c4c;}"
    ".add{margin-top:24px;padding:16px;background:#1e1e1e;border-radius:8px;}"
    ".add h2{font-size:14px;color:#888;margin:0 0 12px;text-transform:uppercase;letter-spacing:1px;}"
    ".row{display:flex;gap:8px;}"
    "select,input{padding:10px;background:#2a2a2a;color:#fff;border:1px solid #444;"
    "border-radius:6px;font-size:14px;}"
    "input{flex:1;}"
    ".addbtn{background:#3666e6;color:#fff;padding:10px 20px;font-weight:600;}"
    ".empty{padding:20px;text-align:center;color:#666;font-style:italic;}"
    "</style></head><body>"
    "<h1>Ticker Settings</h1>"
    "<div class='sub'>"; html += String(tickerCount) + " / " + String(MAX_TICKERS) + " configured</div>";

  // ── Display Settings 섹션 ──────────────────────────
  html += "<div class='add' style='margin-top:0;margin-bottom:24px;'>"
          "<h2>Display Settings</h2>"
          "<form method='POST' action='/settings'>"
          "<div style='display:flex;flex-direction:column;gap:12px;'>"
          "<div><div style='font-size:12px;color:#888;margin-bottom:6px;'>Chart Period</div>"
          "<div style='display:flex;gap:6px;'>";
  for (int i = 0; i < PERIOD_COUNT; i++) {
    bool sel = (i == chartPeriodIdx);
    html += "<button type='submit' name='period' value='" + String(i) +
            "' style='flex:1;padding:10px;border-radius:6px;border:1px solid #444;"
            "background:" + String(sel ? "#3666e6" : "#2a2a2a") +
            ";color:" + String(sel ? "#fff" : "#aaa") +
            ";font-weight:" + String(sel ? "600" : "400") + ";'>";
    html += PERIODS[i].label;
    html += "</button>";
  }
  html += "</div></div>";

  html += "<div><div style='font-size:12px;color:#888;margin-bottom:6px;'>"
          "Switch Interval (seconds)</div>"
          "<div style='display:flex;gap:8px;'>"
          "<input type='number' name='interval' min='2' max='60' value='" +
          String(switchMs / 1000) + "' style='flex:1;'>"
          "<button type='submit' name='save_interval' value='1' class='addbtn'>Save</button>"
          "</div></div>"
          "</div></form></div>";


  if (tickerCount == 0) {
    html += "<div class='empty'>No tickers. Add one below.</div>";
  } else {
    for (int i = 0; i < tickerCount; i++) {
      String display = tickers[i];
      String tagClass, tagText;
      if (tickers[i].startsWith("COIN:")) {
        display = tickers[i].substring(5);
        tagClass = "crypto"; tagText = "CRYPTO";
      } else if (tickers[i].startsWith("KR:")) {
        display = tickers[i].substring(3);
        tagClass = "kstock"; tagText = "K-STOCK";
      } else {
        tagClass = "stock"; tagText = "STOCK";
      }
      html += "<div class='item'>";
      html += "<span class='tag " + tagClass + "'>" + tagText + "</span>";
      html += "<span class='sym'>" + htmlEscape(display) + "</span>";
      html += "<form method='POST' action='/delete'>";
      html += "<input type='hidden' name='idx' value='" + String(i) + "'>";
      html += "<button class='del'>Remove</button></form>";
      html += "</div>";
    }
  }

  html +=
    "<div class='add'><h2>Add Ticker</h2>"
    "<form method='POST' action='/add'><div class='row'>"
    "<select name='type'>"
    "<option value='stock'>STOCK (US)</option>"
    "<option value='crypto'>CRYPTO</option>"
    "<option value='kstock'>K-STOCK</option>"
    "</select>"
    "<input name='symbol' placeholder='AAPL / BTC / 005930' maxlength='10' required autocapitalize='characters'>"
    "<button class='addbtn'>Add</button>"
    "</div></form></div>"
    "<p style='color:#666;font-size:12px;margin-top:24px;text-align:center;line-height:1.6;'>"
    "Crypto: BTC, ETH, SOL, BNB, XRP, DOGE<br>"
    "K-Stock: 005930 (Samsung), 000660 (SK Hynix), 035720 (Kakao)</p>"
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
    String full;
    if      (type == "crypto") full = "COIN:" + symbol;
    else if (type == "kstock") full = "KR:" + symbol;
    else                       full = symbol;
    tickers[tickerCount++] = full;
    saveTickers();
  }
  webServer.sendHeader("Location", "/");
  webServer.send(303);
}

void handleDelete() {
  int idx = webServer.arg("idx").toInt();
  if (idx >= 0 && idx < tickerCount) {
    for (int i = idx; i < tickerCount - 1; i++) tickers[i] = tickers[i + 1];
    tickers[tickerCount - 1] = "";
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
  if (changed) saveSettings();

  webServer.sendHeader("Location", "/");
  webServer.send(303);
}

void setupWebServer() {
  webServer.on("/", HTTP_GET, handleRoot);
  webServer.on("/add", HTTP_POST, handleAdd);
  webServer.on("/delete", HTTP_POST, handleDelete);
  webServer.on("/settings", HTTP_POST, handleSettings);
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
  http.begin(url);
  http.setUserAgent("Mozilla/5.0");

  int status = http.GET();
  if (status == 200) {
    JsonDocument doc;
    if (!deserializeJson(doc, http.getString())) {
      auto result = doc["chart"]["result"][0];
      auto meta = result["meta"];
      data.price = meta["regularMarketPrice"];
      data.valid = data.price > 0;

      // 차트: close 배열에서 null 제외하고 채움 (긴 기간이면 다운샘플)
      auto closes = result["indicators"]["quote"][0]["close"];
      int total = 0;
      for (JsonVariant cv : closes.as<JsonArray>()) {
        if (!cv.isNull()) total++;
      }
      int step = (total > MAX_CHART_POINTS) ? total / MAX_CHART_POINTS : 1;
      int seen = 0;
      for (JsonVariant cv : closes.as<JsonArray>()) {
        if (cv.isNull()) continue;
        if (seen % step == 0 && data.chartCount < MAX_CHART_POINTS) {
          data.chart[data.chartCount++] = cv.as<float>();
        }
        seen++;
      }

      // 변화율: 차트 시작점 대비
      if (data.chartCount > 0) {
        float first = data.chart[0];
        data.change = first > 0 ? ((data.price - first) / first) * 100.0f : 0;
      }
      computeChartRange(data);
    }
  }
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
  http.begin(url);
  http.addHeader("accept", "application/json");
  http.setTimeout(15000);  // 큰 응답을 위해 타임아웃 여유

  int code = http.GET();
  if (code == 200) {
    JsonDocument doc;
    if (!deserializeJson(doc, http.getString())) {
      // 응답: [[ts, open, high, low, close], ...]
      auto items = doc.as<JsonArray>();
      int total = items.size();
      int step = total > MAX_CHART_POINTS ? total / MAX_CHART_POINTS : 1;
      int i = 0;
      for (JsonVariant item : items) {
        if (i % step == 0 && data.chartCount < MAX_CHART_POINTS) {
          data.chart[data.chartCount++] = item[4].as<float>();  // close
        }
        i++;
      }
      if (data.chartCount > 0) {
        data.price = data.chart[data.chartCount - 1];
        float first = data.chart[0];
        data.change = first > 0 ? ((data.price - first) / first) * 100.0f : 0;
        data.valid = true;
        computeChartRange(data);
      }
    } else {
      Serial.println("CoinGecko JSON parse error");
    }
  } else {
    Serial.printf("CoinGecko HTTP %d\n", code);
  }
  http.end();
  return data;
}

TickerData fetchTicker(const char* ticker) {
  if (strncmp(ticker, "COIN:", 5) == 0)
    return fetchCrypto(ticker + 5);
  if (strncmp(ticker, "KR:", 3) == 0)
    return fetchKoreanStock(ticker + 3);
  return fetchStock(ticker);
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

  // ── 좌측: 심볼 (작게) + 변화율 (아래) ─────────────
  tft.setTextFont(4);
  tft.setTextSize(1);
  tft.setTextColor(COL_TEXT, COL_BG);
  tft.setCursor(15, 38);
  tft.print(d.symbol);
  int symW = tft.textWidth(d.symbol);
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
  tft.setTextSize(2);
  tft.setTextColor(COL_PRICE, COL_BG);
  int pw = tft.textWidth(priceBuf);
  if (pw > 200) {
    tft.setTextSize(1);
    pw = tft.textWidth(priceBuf);
  }
  tft.setCursor(310 - pw, 38);
  tft.print(priceBuf);

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
  setupWebServer();
  Serial.printf("Web UI: http://%s/\n", WiFi.localIP().toString().c_str());

  setupBLE();
  Serial.println("BLE advertising as 'TickerDisplay'");

  if (tickerCount > 0) {
    drawLoading(tickers[currentIndex].c_str());
    drawTicker(fetchTicker(tickers[currentIndex].c_str()));
  } else {
    drawNoTickers();
  }
  lastRefresh = millis();
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

  // 5초 경과 또는 버튼 누름 → 다음 종목으로 전환
  if (nextTickerRequested || millis() - lastRefresh >= switchMs) {
    nextTickerRequested = false;
    currentIndex = (currentIndex + 1) % tickerCount;
    drawLoading(tickers[currentIndex].c_str());
    drawTicker(fetchTicker(tickers[currentIndex].c_str()));
    lastRefresh = millis();
  } else {
    // 연결 상태만 실시간 갱신 (전체 화면 갱신 없이)
    static bool lastBleState = false;
    if (bleConnected != lastBleState) {
      lastBleState = bleConnected;
      drawStatusBar();
    }
  }
}
