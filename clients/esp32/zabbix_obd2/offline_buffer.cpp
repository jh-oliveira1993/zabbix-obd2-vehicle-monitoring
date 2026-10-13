#include "offline_buffer.h"
#include <WiFi.h>

OfflineBuffer offlineBuffer;

static const char* STATE_FILE = "/buf_state.txt";
static const uint16_t SAMPLES_PER_SEGMENT = 50;
static const uint32_t MAX_QUEUED_SAMPLES = 50000;  // ~500 KB limit on Flash
static const time_t MIN_VALID_EPOCH = 1700000000;  // Nov 2023 threshold

OfflineBuffer::OfflineBuffer()
    : headSeq(1),
      tailSeq(1),
      currentSegmentSamples(0),
      totalSamplesCount(0),
      isFsMounted(false) {}

bool OfflineBuffer::begin() {
  if (!LittleFS.begin(true)) {
    Serial.println("[BUFFER] [ERROR] Failed to mount LittleFS!");
    isFsMounted = false;
    return false;
  }
  isFsMounted = true;
  Serial.printf("[BUFFER] LittleFS mounted! Used: %u / %u bytes\n",
                (uint32_t)LittleFS.usedBytes(), (uint32_t)LittleFS.totalBytes());

  // Configure NTP (UTC)
  configTime(0, 0, "pool.ntp.org", "time.google.com");

  loadState();
  Serial.printf("[BUFFER] Ready. Head: %u, Tail: %u, In tail: %u, Total queued: %u\n",
                headSeq, tailSeq, currentSegmentSamples, totalSamplesCount);
  return true;
}

void OfflineBuffer::syncTime() {
  if (isTimeSynced()) {
    return;
  }
  if (WiFi.status() == WL_CONNECTED) {
    configTime(0, 0, "pool.ntp.org", "time.google.com");
  }
}

bool OfflineBuffer::isTimeSynced() {
  time_t now = time(nullptr);
  return (now > MIN_VALID_EPOCH);
}

String OfflineBuffer::getSegmentPath(uint32_t seq) {
  char buf[32];
  snprintf(buf, sizeof(buf), "/b_%06u.bin", seq);
  return String(buf);
}

void OfflineBuffer::saveState() {
  if (!isFsMounted) return;
  File f = LittleFS.open(STATE_FILE, "w");
  if (!f) return;
  f.printf("%u %u %u %u\n", headSeq, tailSeq, currentSegmentSamples, totalSamplesCount);
  f.close();
}

void OfflineBuffer::loadState() {
  if (!isFsMounted) return;
  if (!LittleFS.exists(STATE_FILE)) {
    recoverStateFromDir();
    return;
  }
  File f = LittleFS.open(STATE_FILE, "r");
  if (!f) {
    recoverStateFromDir();
    return;
  }
  String line = f.readStringUntil('\n');
  f.close();

  if (sscanf(line.c_str(), "%u %u %u %u", &headSeq, &tailSeq,
             &currentSegmentSamples, &totalSamplesCount) != 4) {
    recoverStateFromDir();
  }
}

void OfflineBuffer::recoverStateFromDir() {
  headSeq = 1;
  tailSeq = 1;
  currentSegmentSamples = 0;
  totalSamplesCount = 0;

  File root = LittleFS.open("/");
  if (!root || !root.isDirectory()) {
    saveState();
    return;
  }

  uint32_t minSeq = 0xFFFFFFFF;
  uint32_t maxSeq = 0;

  File file = root.openNextFile();
  while (file) {
    String name = String(file.name());
    if (name.startsWith("/")) name = name.substring(1);
    if (name.startsWith("b_") && name.endsWith(".bin")) {
      uint32_t seq = (uint32_t)strtoul(name.substring(2, name.length() - 4).c_str(), NULL, 10);
      if (seq > 0) {
        if (seq < minSeq) minSeq = seq;
        if (seq > maxSeq) maxSeq = seq;
        totalSamplesCount += file.size() / sizeof(BufferedSample);
      }
    }
    file = root.openNextFile();
  }

  if (maxSeq > 0) {
    headSeq = minSeq;
    tailSeq = maxSeq;
    String tailPath = getSegmentPath(tailSeq);
    if (LittleFS.exists(tailPath)) {
      File tf = LittleFS.open(tailPath, "r");
      currentSegmentSamples = tf.size() / sizeof(BufferedSample);
      tf.close();
    }
  }

  saveState();
}

bool OfflineBuffer::pushSample(uint8_t pid, float value) {
  if (!isFsMounted) return false;

  // Ring buffer protection: if queue exceeds MAX_QUEUED_SAMPLES, drop oldest segment
  if (totalSamplesCount >= MAX_QUEUED_SAMPLES && headSeq < tailSeq) {
    String oldest = getSegmentPath(headSeq);
    if (LittleFS.exists(oldest)) {
      File of = LittleFS.open(oldest, "r");
      size_t count = of.size() / sizeof(BufferedSample);
      of.close();
      LittleFS.remove(oldest);
      if (totalSamplesCount >= count) {
        totalSamplesCount -= count;
      } else {
        totalSamplesCount = 0;
      }
    }
    headSeq++;
    saveState();
    Serial.printf("[BUFFER] [WARN] Flash buffer full, dropped oldest segment %u\n", headSeq - 1);
  }

  BufferedSample sample;
  if (isTimeSynced()) {
    sample.timestamp = (uint32_t)time(nullptr);
    sample.flags = 1;  // Absolute epoch
  } else {
    sample.timestamp = (uint32_t)(millis() / 1000);
    sample.flags = 0;  // Relative uptime seconds
  }
  sample.pid = pid;
  sample.value = value;

  String path = getSegmentPath(tailSeq);
  File f = LittleFS.open(path, "a");
  if (!f) {
    Serial.printf("[BUFFER] [ERROR] Failed to open %s for append!\n", path.c_str());
    return false;
  }

  size_t written = f.write((const uint8_t*)&sample, sizeof(BufferedSample));
  f.close();

  if (written == sizeof(BufferedSample)) {
    currentSegmentSamples++;
    totalSamplesCount++;

    if (currentSegmentSamples >= SAMPLES_PER_SEGMENT) {
      tailSeq++;
      currentSegmentSamples = 0;
      saveState();
    }
    return true;
  }
  return false;
}

bool OfflineBuffer::hasBacklog() {
  if (!isFsMounted) return false;
  return (headSeq < tailSeq) || (headSeq == tailSeq && currentSegmentSamples > 0);
}

uint32_t OfflineBuffer::getQueuedSamplesCount() {
  return totalSamplesCount;
}

size_t OfflineBuffer::getUsedBytes() {
  if (!isFsMounted) return 0;
  return LittleFS.usedBytes();
}

uint32_t OfflineBuffer::calculateEpoch(const BufferedSample& sample) {
  if (sample.flags == 1) {
    return sample.timestamp;
  }
  time_t now = time(nullptr);
  uint32_t currentUptime = (uint32_t)(millis() / 1000);
  if (now > MIN_VALID_EPOCH && currentUptime >= sample.timestamp) {
    uint32_t bootEpoch = (uint32_t)now - currentUptime;
    return bootEpoch + sample.timestamp;
  }
  return sample.timestamp;
}

bool OfflineBuffer::drainSingleBatch(BatchSenderFn sender, const char* host,
                                    PidSuffixLookupFn lookup) {
  if (!isFsMounted || !hasBacklog()) return false;

  // We should only drain if time is synced, to avoid sending epoch 1970
  if (!isTimeSynced()) {
    syncTime();
    return false;
  }

  if (headSeq == tailSeq && currentSegmentSamples == 0) {
    return false;
  }

  String path = getSegmentPath(headSeq);
  if (!LittleFS.exists(path)) {
    headSeq++;
    saveState();
    return false;
  }

  File f = LittleFS.open(path, "r");
  if (!f) {
    headSeq++;
    saveState();
    return false;
  }

  size_t fileSize = f.size();
  size_t sampleCount = fileSize / sizeof(BufferedSample);
  if (sampleCount == 0) {
    f.close();
    LittleFS.remove(path);
    headSeq++;
    saveState();
    return false;
  }

  BufferedSample samples[SAMPLES_PER_SEGMENT];
  size_t toRead = (sampleCount > SAMPLES_PER_SEGMENT) ? SAMPLES_PER_SEGMENT : sampleCount;
  size_t bytesRead = f.read((uint8_t*)samples, toRead * sizeof(BufferedSample));
  f.close();

  size_t actualSamples = bytesRead / sizeof(BufferedSample);
  if (actualSamples == 0) return false;

  String payload = "{\"request\":\"sender data\",\"data\":[";
  payload.reserve(3500);
  bool firstItem = true;
  size_t validFormatted = 0;

  for (size_t i = 0; i < actualSamples; i++) {
    const char* suffix = lookup(samples[i].pid);
    if (!suffix) continue;

    uint32_t epoch = calculateEpoch(samples[i]);

    if (!firstItem) payload += ",";
    payload += "{\"host\":\"" + String(host) + "\",";
    payload += "\"key\":\"obd[" + String(suffix) + "]\",";
    payload += "\"value\":\"" + String(samples[i].value, 2) + "\",";
    payload += "\"clock\":" + String(epoch) + "}";
    firstItem = false;
    validFormatted++;
  }
  payload += "]}";

  if (validFormatted == 0) {
    LittleFS.remove(path);
    if (totalSamplesCount >= actualSamples) {
      totalSamplesCount -= actualSamples;
    } else {
      totalSamplesCount = 0;
    }
    if (headSeq == tailSeq) {
      headSeq++;
      tailSeq++;
      currentSegmentSamples = 0;
    } else {
      headSeq++;
    }
    saveState();
    return true;
  }

  Serial.printf("[BUFFER] Draining segment %s (%u samples, %u bytes)...\n",
                path.c_str(), validFormatted, payload.length());

  bool success = sender(payload, host);
  if (success) {
    LittleFS.remove(path);
    if (totalSamplesCount >= actualSamples) {
      totalSamplesCount -= actualSamples;
    } else {
      totalSamplesCount = 0;
    }

    if (headSeq == tailSeq) {
      headSeq++;
      tailSeq++;
      currentSegmentSamples = 0;
    } else {
      headSeq++;
    }
    saveState();
    Serial.printf("[BUFFER] [OK] Drained & removed segment %s. Remaining queued: %u\n",
                  path.c_str(), totalSamplesCount);
    return true;
  } else {
    Serial.printf("[BUFFER] [WARN] Transmission failed for segment %s, keeping for retry.\n",
                  path.c_str());
    return false;
  }
}
