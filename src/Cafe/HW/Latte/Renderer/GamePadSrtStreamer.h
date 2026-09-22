#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
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
	static GamePadSrtStreamer& Instance();

	bool Start(const std::string& uri, Encoder encoder, std::string& error);
	void Stop();
	bool IsCaptureRequested() const { return m_captureRequested.load(std::memory_order_acquire); }
	bool IsRunning() const { return m_running.load(std::memory_order_acquire); }
	uint64_t Generation() const { return m_generation.load(std::memory_order_acquire); }
	std::string TakeError();
	void SubmitFrame(const uint8_t* rgba, size_t size, uint64_t captureTimestampNs);
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
	std::atomic_bool m_captureRequested{false};
	std::atomic_bool m_running{false};
	std::atomic_bool m_stop{false};
	std::atomic_uint64_t m_generation{0};
	std::mutex m_mutex;
	std::condition_variable m_wake;
	std::deque<Frame> m_frames;
	std::string m_error;
	std::thread m_worker;
	std::atomic_uint64_t m_captureDrops{0};
	std::atomic_uint64_t m_captureFrames{0};
	std::atomic_uint64_t m_queueDrops{0};
	std::atomic_uint64_t m_readbackFrames{0};
	std::atomic_uint64_t m_readbackTotalNs{0};
	std::atomic_uint64_t m_readbackMaxNs{0};
};
