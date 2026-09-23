#pragma once

#include "input/api/ControllerProvider.h"
#include "input/api/RemoteGamePad/RemoteGamePadProtocol.h"
#include "input/motion/MotionSample.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

class RemoteGamePadProvider final : public ControllerProviderBase
{
public:
	inline static InputAPI::Type kAPIType = InputAPI::RemoteGamePad;
	InputAPI::Type api() const override { return kAPIType; }
	std::vector<std::shared_ptr<ControllerBase>> get_controllers() override;
	~RemoteGamePadProvider() override { Stop(); }

	struct Snapshot
	{
		RemoteGamePadProtocol::State state{};
		MotionSample motion{};
		uint32_t features{};
		bool connected{};
	};

	static bool ParseEndpoint(std::string_view text, std::string& host, std::string& port);
	bool Start(std::string_view endpoint, std::string& error);
	void Stop();
	Snapshot GetSnapshot() const;
	void SetRumble(bool on) { m_rumble.store(on, std::memory_order_release); }
	std::string TakeError();

private:
	void Run(std::string host, std::string port);
	void ReportError(std::string error);

	mutable std::mutex m_mutex;
	Snapshot m_snapshot{};
	std::chrono::steady_clock::time_point m_lastState{};
	std::string m_error;
	bool m_errorReported{};
	std::thread m_thread;
	std::atomic_bool m_stop{false};
	std::atomic_bool m_rumble{false};
};
