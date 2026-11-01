// ---------------------------------------------------------- push-to-talk ---
// Hold A to talk (on an approval card: deny it and tell Claude why). Audio is recorded at 16 kHz
// while the button is held, sent as a WAV to an OpenAI-compatible speech-to-text
// endpoint (e.g. speaches / faster-whisper on your network), and the text is shown
// for confirmation before it goes to Claude like any phone-typed instruction.
// Included from main.cpp; shares its state.

static const uint32_t VOICE_RATE = 16000;
static const size_t   VOICE_MAX  = VOICE_RATE * 20;          // 20 s cap
static const size_t   VOICE_CHUNK = 1600;                    // 100 ms per mic read
static int16_t* voiceBuf = nullptr;

enum VoiceState : uint8_t { V_IDLE, V_REC, V_THINK, V_CONFIRM, V_ERROR, V_QUEUED };   // V_QUEUED: sent, waiting for Claude; A = send now
static VoiceState voiceState = V_IDLE;
static String     voiceText, voiceErr, voiceTarget;   // target = session id
static uint32_t   voiceQueuedAt = 0;
static uint32_t   voiceStopReq = 0;                   // Done/Question card it answers, if any
static uint32_t   voiceApproval = 0;                  // approval it denies (with what you said), if any
static uint32_t   voiceErrUntil = 0;
static size_t     voiceLen = 0;
static int        voiceLevel = 0;

static void voiceFail(const String& why) {
  voiceErr = why; voiceState = V_ERROR; voiceErrUntil = millis() + 3500; beep(3); dirty = true;
  tel.voiceFail++;
}

static void writeLE(uint8_t* p, uint32_t v, int n) { for (int i = 0; i < n; i++) p[i] = (v >> (8 * i)) & 0xFF; }

// 44-byte WAV header for 16-bit mono PCM.
static void wavHeader(uint8_t* p, size_t pcmBytes) {
  memcpy(p, "RIFF", 4); writeLE(p + 4, 36 + pcmBytes, 4); memcpy(p + 8, "WAVEfmt ", 8);
  writeLE(p + 16, 16, 4); writeLE(p + 20, 1, 2); writeLE(p + 22, 1, 2); writeLE(p + 24, VOICE_RATE, 4);
  writeLE(p + 28, VOICE_RATE * 2, 4); writeLE(p + 32, 2, 2); writeLE(p + 34, 16, 2);
  memcpy(p + 36, "data", 4); writeLE(p + 40, pcmBytes, 4);
}

// Speech to text through the bridge: a WAV in, {"text": ...} out. The bridge holds the speech server's address and key.
static bool bridgeStt(const uint8_t* wav, size_t len, String& outText) {
  String out;
  int code = bridgeRequest("POST", "/stt", "", out, 30000, "audio/wav", wav, len);
  if (code != 200) {
    JsonDocument e; String why = (code > 0 && !deserializeJson(e, out)) ? String((const char*)(e["error"] | "")) : String();
    voiceErr = "bridge: " + (why.length() ? why : code > 0 ? "error " + String(code) : bridgeErr);
    return false;
  }
  JsonDocument d;
  if (deserializeJson(d, out)) { voiceErr = "bridge: bad reply"; return false; }
  outText = (const char*)(d["text"] | "");
  outText.trim();
  return true;
}

// Multipart body: file (WAV) + model + optional language, all in PSRAM.
static bool sttRequest(String& outText) {
  if (bridgeUp) {   // away from home: the bridge knows the speech server, so send it just the WAV
    size_t pcm = voiceLen * 2;
    uint8_t* wav = (uint8_t*)ps_malloc(44 + pcm);
    if (!wav) return false;
    wavHeader(wav, pcm);
    memcpy(wav + 44, voiceBuf, pcm);
    bool ok = bridgeStt(wav, 44 + pcm, outText);
    free(wav);
    return ok;
  }
  const String boundary = "----claudestick7f3a";
  size_t pcmBytes = voiceLen * 2;
  String head = "--" + boundary + "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"speech.wav\"\r\n"
                "Content-Type: audio/wav\r\n\r\n";
  String tail = "\r\n--" + boundary + "\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\n" + conf.sttModel +
                "\r\n--" + boundary + "\r\nContent-Disposition: form-data; name=\"response_format\"\r\n\r\njson";
  if (conf.sttLang.length())
    tail += "\r\n--" + boundary + "\r\nContent-Disposition: form-data; name=\"language\"\r\n\r\n" + conf.sttLang;
  tail += "\r\n--" + boundary + "--\r\n";
  size_t total = head.length() + 44 + pcmBytes + tail.length();
  uint8_t* body = (uint8_t*)ps_malloc(total);
  if (!body) return false;
  uint8_t* p = body;
  memcpy(p, head.c_str(), head.length()); p += head.length();
  wavHeader(p, pcmBytes); p += 44;
  memcpy(p, voiceBuf, pcmBytes); p += pcmBytes;
  memcpy(p, tail.c_str(), tail.length());

  HTTPClient http;
  WiFiClientSecure tls;
  bool ok = conf.sttUrl.startsWith("https") ? (tls.setInsecure(), http.begin(tls, conf.sttUrl)) : http.begin(conf.sttUrl);
  if (!ok) { free(body); return false; }
  http.setTimeout(30000);
  http.addHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
  if (conf.sttKey.length()) http.addHeader("Authorization", "Bearer " + conf.sttKey);
  int code = http.POST(body, total);
  free(body);
  String resp = code > 0 ? http.getString() : "";
  http.end();
  if (code != 200) { voiceErr = "speech server: " + (code > 0 ? String(code) : http.errorToString(code)); return false; }
  JsonDocument d;
  if (deserializeJson(d, resp)) { voiceErr = "speech server: bad reply"; return false; }
  outText = (const char*)(d["text"] | "");
  outText.trim();
  return true;
}

static void drawVoiceRec(uint32_t ms);   // defined with the other screens

// Blocks while A is held: records, then transcribes. Called from buttons().
static void voiceRecord();

static bool voiceReady() {
  if (!conf.sttUrl.length() && !bridgeUp) { voiceFail("voice isn't set up: open settings"); return false; }
  if (WiFi.status() != WL_CONNECTED) { voiceFail("no wi-fi"); return false; }
  if (!voiceBuf) voiceBuf = (int16_t*)ps_malloc(VOICE_MAX * sizeof(int16_t));
  if (!voiceBuf) { voiceFail("out of memory"); return false; }
  return true;
}

static void voiceCapture(StopReq* card) {
  if (!voiceReady()) return;
  Session* s = targetSession();
  voiceTarget = card ? card->session : (s ? s->id : String());
  voiceStopReq = card ? card->id : 0;
  voiceApproval = 0;
  if (!voiceTarget.length()) { voiceFail("no claude session to talk to"); return; }
  voiceRecord();
}

// Hold A on an approval card: record why not; A on the result denies with that message.
static void voiceDenyCapture(Approval* a) {
  if (!a) { voiceState = V_IDLE; voiceApproval = 0; dirty = true; return; }
  if (!voiceReady()) return;
  voiceTarget = a->session; voiceStopReq = 0; voiceApproval = a->id;
  voiceRecord();
  if (voiceState != V_CONFIRM) voiceApproval = 0;
}

static void voiceRecord() {

  M5.Speaker.end();                    // speaker and mic share the codec
  M5.Mic.begin();
  voiceState = V_REC; voiceLen = 0;
  uint32_t t0 = millis(), lastPaint = 0;
  while (voiceLen + VOICE_CHUNK <= VOICE_MAX) {
    M5.Mic.record(voiceBuf + voiceLen, VOICE_CHUNK, VOICE_RATE);   // queues; blocks when both slots are busy
    voiceLen += VOICE_CHUNK;
    M5.update();
    if (!M5.BtnA.isPressed()) break;
    if (millis() - lastPaint > 150) {
      lastPaint = millis();
      int peak = 0;
      if (voiceLen >= 3 * VOICE_CHUNK)
        for (size_t i = voiceLen - 3 * VOICE_CHUNK; i < voiceLen - 2 * VOICE_CHUNK; i += 8) peak = max(peak, abs((int)voiceBuf[i]));
      voiceLevel = peak;
      drawVoiceRec(millis() - t0);
    }
  }
  while (M5.Mic.isRecording()) delay(1);
  M5.Mic.end();
  M5.Speaker.begin();
  M5.Speaker.setVolume(volume);

  if (voiceLen < VOICE_RATE / 2) { voiceState = V_IDLE; dirty = true; return; }   // tap, not talk
  voiceState = V_THINK; dirty = true;
  drawVoiceRec(0);
  String text;
  if (!sttRequest(text)) { voiceFail(voiceErr.length() ? voiceErr : "speech request failed"); return; }
  if (!text.length()) { voiceFail("didn't catch that"); return; }
  voiceText = text; voiceState = V_CONFIRM; scrollLine = 0; beep(0); touch();
  tel.voiceOk++;
}

static void voiceSend() {
  Session* s = findSession(voiceTarget, false);
  bool ok = false;
  if (voiceApproval) {
    Approval* a = approvalById(voiceApproval);
    voiceApproval = 0;
    if (!a || a->decision != D_PENDING) { voiceFail("that approval was already answered"); return; }
    a->reason = voiceText;
    decide(a, D_DENY);
    voiceState = V_IDLE; dirty = true;
    return;
  }
  if (voiceStopReq) {
    for (auto& x : stops)
      if (x.used && x.id == voiceStopReq && x.result == -1) { x.custom = voiceText; x.result = -3; ok = true; }
  }
  bool answeredCard = ok, hadCard = false;
  if (!ok) {
    if (s) for (auto& x : stops) if (x.used && x.result == -1 && x.session == s->id) hadCard = true;   // deliverText answers it
    ok = deliverText(s, voiceText);
  }
  if (ok) { noteStick(); beep(2); heartUntil = millis() + 1500; }
  else voiceFail("that session is gone");
  // Text for a session that isn't waiting on a card is queued: say so, and offer to send it now.
  if (ok) { voiceState = (!answeredCard && !hadCard) ? V_QUEUED : V_IDLE; voiceQueuedAt = millis(); }
  dirty = true;
}
