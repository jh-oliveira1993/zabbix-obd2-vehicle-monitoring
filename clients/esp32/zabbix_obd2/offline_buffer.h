#ifndef OFFLINE_BUFFER_H
#define OFFLINE_BUFFER_H

#include <Arduino.h>
#include <LittleFS.h>
#include <time.h>

#pragma pack(push, 1)
struct BufferedSample {
  uint32_t timestamp;  // Unix timestamp (or relative uptime seconds)
  uint8_t flags;       // Bit 0: 0 = relative uptime, 1 = absolute epoch
  uint8_t pid;         // OBD2 PID byte
  float value;         // Sensor reading
};
#pragma pack(pop)

typedef const char* (*PidSuffixLookupFn)(uint8_t pid);
typedef bool (*BatchSenderFn)(const String& json, const char* host);

class OfflineBuffer {
 public:
  OfflineBuffer();

  bool begin();
  void syncTime();
  bool isTimeSynced();

  // Enqueue a sample
  bool pushSample(uint8_t pid, float value);

  // Buffer state
  bool hasBacklog();
  uint32_t getQueuedSamplesCount();
  size_t getUsedBytes();

  // Drain one batch of up to SAMPLES_PER_SEGMENT to Zabbix
  bool drainSingleBatch(BatchSenderFn sender, const char* host, PidSuffixLookupFn lookup);

 private:
  uint32_t headSeq;                // Oldest segment to read
  uint32_t tailSeq;                // Active segment being written
  uint16_t currentSegmentSamples;  // Samples in tail segment
  uint32_t totalSamplesCount;
  bool isFsMounted;

  void saveState();
  void loadState();
  void recoverStateFromDir();
  String getSegmentPath(uint32_t seq);
  uint32_t calculateEpoch(const BufferedSample& sample);
};

extern OfflineBuffer offlineBuffer;

#endif  // OFFLINE_BUFFER_H
