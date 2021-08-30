#include "recorder_worker.h"

#include <logger.h>

RecorderWorker::RecorderWorker(int id, const FrequencyRange &frequencyRange, std::mutex &inMutex, std::condition_variable &inCv, std::deque<InputSamples> &inSamples, std::mutex &outMutex,
                               std::condition_variable &outCv, std::deque<OutputSamples> &outSamples)
    : m_id(id),
      m_frequencyRange(frequencyRange),
      m_decimateRate(frequencyRange.sampleRate().value / RESAMPLER_MINIMAL_OUT_SAMPLE_RATE),
      m_spectrogram(frequencyRange.fftSize()),
      m_decimator(m_decimateRate),
      m_inMutex(inMutex),
      m_inCv(inCv),
      m_inSamples(inSamples),
      m_outMutex(outMutex),
      m_outCv(outCv),
      m_outSamples(outSamples),
      m_isWorking(true),
      m_thread([this]() {
        Logger::debug("recorder", "thread: {}, start", m_id);
        while (m_isWorking) {
          {
            std::unique_lock<std::mutex> lock(m_inMutex);
            m_inCv.wait(lock);
            if (m_inSamples.empty()) {
              continue;
            }
          }
          while (true) {
            InputSamples inputSamples;
            {
              std::unique_lock<std::mutex> lock(m_inMutex);
              if (m_inSamples.empty()) {
                break;
              }
              inputSamples = std::move(m_inSamples.front());
              m_inSamples.pop_front();
              Logger::debug("recorder", "thread: {}, pop input samples, size: {}", m_id, m_inSamples.size());
            }
            auto outputSamples = processSamples(inputSamples);
            std::unique_lock<std::mutex> lock(m_outMutex);
            m_outSamples.push_back(std::move(outputSamples));
            Logger::debug("recorder", "thread: {}, push output samples, size: {}", m_id, m_outSamples.size());
            lock.unlock();
            m_outCv.notify_one();
          }
        }
        Logger::debug("recorder", "thread: {}, stop", m_id);
      }) {
  Logger::debug("recorder", "thread: {}, init", m_id);
}

RecorderWorker::~RecorderWorker() {
  Logger::debug("recorder", "thread: {}, deinit", m_id);
  m_isWorking = false;
  m_inCv.notify_all();
  m_thread.join();
}

OutputSamples RecorderWorker::processSamples(const InputSamples &inputSamples) {
  const auto bestFrequency = inputSamples.frequency;
  const auto rawBufferSamples = inputSamples.samples.size() / 2;
  const auto downSamples = rawBufferSamples / m_decimateRate;
  const auto fmSamples = downSamples;
  const auto center = m_frequencyRange.center();
  const auto bandwidth = m_frequencyRange.bandwidth();
  const auto sampleRate = m_frequencyRange.sampleRate();

  Logger::trace("recorder", "thread: {}, processing started, samples: {}, {}", m_id, inputSamples.samples.size(), bestFrequency.toString());

  if (m_rawBuffer.size() < rawBufferSamples) {
    m_rawBuffer.resize(rawBufferSamples);
    Logger::debug("recorder", "thread: {}, raw buffer resized, size: {}", m_id, rawBufferSamples);
  }
  if (m_decimatorBuffer.size() < downSamples) {
    m_decimatorBuffer.resize(downSamples);
    Logger::debug("recorder", "thread: {}, decimator buffer resized, size: {}", m_id, downSamples);
  }
  if (m_fmBuffer.size() < fmSamples) {
    m_fmBuffer.resize(fmSamples);
    Logger::debug("recorder", "thread: {}, fm buffer resized, size: {}", m_id, fmSamples);
  }

  toComplex(inputSamples.samples.data(), m_rawBuffer, rawBufferSamples);
  Logger::trace("recorder", "thread: {}, uint8 to complex finished", m_id);

  const auto allSignals = m_spectrogram.psd(center, bandwidth, m_rawBuffer, rawBufferSamples);
  Logger::trace("recorder", "thread: {}, psd finished", m_id);

  const auto signals = filterSignals(allSignals, m_frequencyRange);
  const auto bestSignal = detectbestSignal(signals);
  Logger::trace("recorder", "thread: {}, best signal finished", m_id);

  shift(m_rawBuffer, center.value - bestFrequency.value, sampleRate, rawBufferSamples);
  Logger::trace("recorder", "thread: {}, shift finished", m_id);

  m_decimator.decimate(m_rawBuffer.data(), rawBufferSamples / m_decimateRate, m_decimatorBuffer.data());
  Logger::trace("recorder", "thread: {}, resampling finished , in rate/samples: {}/{}, out rate/samples: {}/{}", m_id, sampleRate.value, rawBufferSamples, sampleRate.value / m_decimateRate,
                downSamples);

  m_demodulator.demodulate(m_decimatorBuffer.data(), downSamples, m_fmBuffer.data());
  Logger::trace("recorder", "thread: {}, fm demodulation finished, in {}, out: {}", m_id, downSamples, fmSamples);

  Logger::trace("recorder", "thread: {}, processing finished", m_id);
  return {inputSamples.time, m_fmBuffer, bestSignal.first, bestSignal.second};
}