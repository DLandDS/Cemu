#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

// The renderer submits completed RGBA frames. All GStreamer work happens on the
// worker thread; neither the UI nor the Latte thread calls into GStreamer.
class GamePadSrtStreamer
{
public:
	enum class Encoder { Auto, QuickSync, OpenH264 };
	static constexpr uint32_t kWidth = 854;
	static constexpr uint32_t kHeight = 480;
	static constexpr size_t kAudioFramesPerBlock = 576;
	static GamePadSrtStreamer& Instance();

	static bool ValidateCallerUri(std::string_view uri, std::string& error);
	bool Start(const std::string& uri, Encoder encoder, std::string& error);
	void Stop();
	bool IsCaptureRequested() const { return m_captureRequested.load(std::memory_order_acquire); }
	bool IsRunning() const { return m_running.load(std::memory_order_acquire); }
	bool IsConnected() const { return m_connected.load(std::memory_order_acquire); }
	uint64_t Generation() const { return m_generation.load(std::memory_order_acquire); }
	std::string TakeError();
	void SubmitFrame(const uint8_t* rgba, size_t size, uint64_t captureTimestampNs);
	void SubmitAudio(const int16_t* stereo, size_t frames, uint64_t firstSampleTimestampNs);
	void RecordCapture() { m_captureFrames.fetch_add(1, std::memory_order_relaxed); }
	void RecordCaptureDrop() { m_captureDrops.fetch_add(1, std::memory_order_relaxed); }
	void RecordReadbackCompletion(uint64_t captureTimestampNs);
	void ReportCaptureError(std::string error) { Fail(std::move(error)); }

private:
	GamePadSrtStreamer() = default;
	~GamePadSrtStreamer();
	GamePadSrtStreamer(const GamePadSrtStreamer&) = delete;
	GamePadSrtStreamer& operator=(const GamePadSrtStreamer&) = delete;
	void Worker(std::string uri, Encoder encoder);
	void Fail(std::string error);

	struct Frame
	{
		std::vector<uint8_t> pixels;
		uint64_t timestampNs{};
	};
	struct AudioBlock
	{
		std::array<int16_t, kAudioFramesPerBlock * 2> samples{};
		uint64_t timestampNs{};
		bool discontinuity{};
	};
	std::atomic_bool m_captureRequested{false};
	std::atomic_bool m_running{false};
	std::atomic_bool m_connected{false};
	std::atomic_bool m_stop{false};
	std::atomic_uint64_t m_generation{0};
	std::mutex m_mutex;
	std::condition_variable m_wake;
	std::deque<Frame> m_frames;
	std::deque<AudioBlock> m_audioBlocks;
	std::atomic_bool m_audioDiscontinuity{false};
	std::string m_error;
	std::thread m_worker;
	std::atomic_uint64_t m_captureDrops{0};
	std::atomic_uint64_t m_captureFrames{0};
	std::atomic_uint64_t m_queueDrops{0};
	std::atomic_uint64_t m_audioQueueDrops{0};
	std::atomic_uint64_t m_readbackFrames{0};
	std::atomic_uint64_t m_readbackTotalNs{0};
	std::atomic_uint64_t m_readbackMaxNs{0};
};
