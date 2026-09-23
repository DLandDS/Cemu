#include "input/api/RemoteGamePad/RemoteGamePadProvider.h"
#include "input/api/RemoteGamePad/RemoteGamePadController.h"
#include "input/motion/MotionHandler.h"

#include <boost/asio.hpp>
#include <algorithm>
#include <random>

using namespace std::chrono_literals;

std::vector<std::shared_ptr<ControllerBase>> RemoteGamePadProvider::get_controllers()
{
	return {std::make_shared<RemoteGamePadController>()};
}

bool RemoteGamePadProvider::ParseEndpoint(std::string_view text, std::string& host, std::string& port)
{
	std::string_view name, service;
	const bool bracketed = text.starts_with('[');
	if (bracketed)
	{
		const auto close = text.find(']');
		if (close == std::string_view::npos || close + 1 >= text.size() || text[close + 1] != ':')
			return false;
		name = text.substr(1, close - 1);
		service = text.substr(close + 2);
	}
	else
	{
		const auto colon = text.rfind(':');
		if (colon == std::string_view::npos)
			return false;
		name = text.substr(0, colon);
		service = text.substr(colon + 1);
		if (name.find(':') != std::string_view::npos)
			return false;
	}
	if (name.empty() || name == "0.0.0.0" || name == "::" || name == "[::]" ||
		service.empty() || service.size() > 5 ||
		name.find_first_of(" /\\?@\t\r\n") != std::string_view::npos)
		return false;
	if (bracketed)
	{
		boost::system::error_code addressError;
		boost::asio::ip::make_address_v6(std::string(name), addressError);
		if (addressError) return false;
	}
	else if (name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-._") !=
		std::string_view::npos)
		return false;
	uint32_t number = 0;
	for (char digit : service)
	{
		if (digit < '0' || digit > '9')
			return false;
		number = number * 10 + uint32_t(digit - '0');
	}
	if (!number || number > 65535)
		return false;
	host = name;
	port = service;
	return true;
}

bool RemoteGamePadProvider::Start(std::string_view endpoint, std::string& error)
{
	std::string host, port;
	if (!ParseEndpoint(endpoint, host, port))
	{
		error = "Input server must be a destination host and port, for example 192.168.1.42:9701.";
		return false;
	}
	Stop();
	{
		std::scoped_lock lock(m_mutex);
		m_error.clear();
		m_errorReported = false;
	}
	m_stop.store(false, std::memory_order_release);
	try
	{
		m_thread = std::thread(&RemoteGamePadProvider::Run, this, std::move(host), std::move(port));
	}
	catch (const std::system_error&)
	{
		error = "Could not start the Remote GamePad input worker.";
		return false;
	}
	return true;
}

void RemoteGamePadProvider::Stop()
{
	m_stop.store(true, std::memory_order_release);
	if (m_thread.joinable())
		m_thread.join();
	m_rumble.store(false, std::memory_order_release);
	std::scoped_lock lock(m_mutex);
	m_snapshot = {};
	m_lastState = {};
	m_error.clear();
	m_errorReported = false;
}

RemoteGamePadProvider::Snapshot RemoteGamePadProvider::GetSnapshot() const
{
	std::scoped_lock lock(m_mutex);
	auto result = m_snapshot;
	if (result.connected && std::chrono::steady_clock::now() - m_lastState >= 250ms)
	{
		result.connected = false;
		result.state = {};
		result.motion = {};
	}
	return result;
}

std::string RemoteGamePadProvider::TakeError()
{
	std::scoped_lock lock(m_mutex);
	std::string result;
	result.swap(m_error);
	return result;
}

void RemoteGamePadProvider::ReportError(std::string error)
{
	std::scoped_lock lock(m_mutex);
	if (!m_errorReported)
	{
		m_error = std::move(error);
		m_errorReported = true;
	}
}

void RemoteGamePadProvider::Run(std::string host, std::string port)
{
	using boost::asio::ip::udp;
	auto pause = [this](std::chrono::milliseconds duration)
	{
		for (auto elapsed = 0ms; elapsed < duration && !m_stop.load(std::memory_order_acquire); elapsed += 10ms)
			std::this_thread::sleep_for(10ms);
	};
	while (!m_stop.load(std::memory_order_acquire))
	{
		boost::asio::io_context context;
		boost::system::error_code error;
		udp::resolver resolver(context);
		auto resolved = resolver.resolve(host, port, error);
		if (error || resolved.empty())
		{
			ReportError("Could not resolve the Input server.");
			pause(2s);
			continue;
		}
		udp::endpoint peer = *resolved.begin();
		for (const auto& candidate : resolved)
		{
			if (candidate.endpoint().address().is_v4())
			{
				peer = candidate.endpoint();
				break;
			}
		}
		if (peer.address().is_unspecified())
		{
			ReportError("Input server is a bind address, not a destination.");
			pause(2s);
			continue;
		}
		udp::socket socket(context);
		socket.open(peer.protocol(), error);
		if (!error)
			socket.bind(udp::endpoint(peer.protocol(), 0), error);
		if (!error)
			socket.non_blocking(true, error);
		if (error)
		{
			ReportError("Could not open a UDP socket.");
			pause(2s);
			continue;
		}

		std::random_device random;
		auto newNonce = [&random]() -> uint64_t
		{
			return (uint64_t(random()) << 32) | random();
		};
		uint64_t nonce = newNonce();
		uint32_t session = 0, features = 0, txSequence = 0, rxSequence = 0;
		uint64_t motionTimestamp = 0;
		bool haveState = false, rumbleSent = false;
		WiiUMotionHandler motion;
		auto started = std::chrono::steady_clock::now();
		auto lastHello = started - 500ms;
		auto lastKeepalive = started;
		auto lastState = started;
		auto lastRumble = started;
		auto send = [&](const auto& packet)
		{
			socket.send_to(boost::asio::buffer(packet), peer, 0, error);
			return !error;
		};
		auto sendRumble = [&](bool on)
		{
			if (session && (features & RemoteGamePadProtocol::kRumble))
				send(RemoteGamePadProtocol::MakeRumble(session, ++txSequence, on));
			rumbleSent = on;
			lastRumble = std::chrono::steady_clock::now();
		};
		while (!m_stop.load(std::memory_order_acquire))
		{
			const auto now = std::chrono::steady_clock::now();
			if (!session && now - lastHello >= 500ms)
			{
				if (!send(RemoteGamePadProtocol::MakeHello(nonce))) break;
				lastHello = now;
				if (now - started >= 3s)
					ReportError("No WELCOME from the Input server.");
			}
			if (session && now - lastKeepalive >= 1s)
			{
				if (!send(RemoteGamePadProtocol::MakeKeepalive(session, ++txSequence))) break;
				lastKeepalive = now;
			}
			if (session && now - lastState >= 250ms)
			{
				if (rumbleSent) sendRumble(false);
				m_rumble.store(false, std::memory_order_release);
				std::scoped_lock lock(m_mutex);
				m_snapshot.state = {};
				m_snapshot.motion = {};
				m_snapshot.connected = false;
			}
			if (session && now - lastState >= 1s)
			{
				ReportError("STATE timed out; reconnecting.");
				session = 0;
				features = 0;
				motionTimestamp = 0;
				haveState = false;
				motion = {};
				nonce = newNonce();
				started = now;
				lastHello = now - 500ms;
				std::scoped_lock lock(m_mutex);
				m_snapshot = {};
			}
			if (session && (features & RemoteGamePadProtocol::kRumble) &&
				now - lastState < 250ms)
			{
				const bool wanted = m_rumble.load(std::memory_order_acquire);
				if (wanted != rumbleSent || (wanted && now - lastRumble >= 100ms))
				{
					sendRumble(wanted);
					if (error) break;
				}
			}

			std::array<uint8_t, 257> bytes{};
			udp::endpoint source;
			const auto size = socket.receive_from(boost::asio::buffer(bytes), source, 0, error);
			if (error == boost::asio::error::message_size)
			{
				// Oversized datagrams are malformed, not a lost connection.
				error.clear();
				continue;
			}
			if (error == boost::asio::error::would_block || error == boost::asio::error::try_again)
			{
				error.clear();
				std::this_thread::sleep_for(2ms);
				continue;
			}
			if (error) break;
			if (source != peer) continue;
			const std::span<const uint8_t> packet(bytes.data(), size);
			RemoteGamePadProtocol::Header header;
			if (!RemoteGamePadProtocol::DecodeHeader(packet, header)) continue;
			if (!session && header.type == 2)
			{
				RemoteGamePadProtocol::Welcome welcome;
				if (!RemoteGamePadProtocol::DecodeWelcome(packet, header, welcome) ||
					welcome.nonce != nonce || (welcome.features & ~RemoteGamePadProtocol::kFeatures))
					continue;
				session = welcome.session;
				features = welcome.features;
				txSequence = 0;
				rxSequence = 0;
				haveState = false;
				motionTimestamp = 0;
				motion = {};
				lastKeepalive = lastState = std::chrono::steady_clock::now();
				std::scoped_lock lock(m_mutex);
				m_snapshot = {};
				m_snapshot.features = features;
			}
			else if (session && header.type == 4 && header.session == session)
			{
				RemoteGamePadProtocol::State state;
				if (!RemoteGamePadProtocol::DecodeState(packet, header, state) ||
					(haveState && !RemoteGamePadProtocol::NewerSequence(state.sequence, rxSequence)) ||
					(!(features & RemoteGamePadProtocol::kTouch) && state.touchActive) ||
					(!(features & RemoteGamePadProtocol::kMotion) &&
						(state.motionTimestamp || state.acceleration != std::array<float, 3>{} ||
							state.gyro != std::array<float, 3>{})) ||
					((features & RemoteGamePadProtocol::kMotion) &&
						(!state.motionTimestamp || (motionTimestamp && state.motionTimestamp <= motionTimestamp))))
					continue;
				if (features & RemoteGamePadProtocol::kMotion)
				{
					const float dt = motionTimestamp ?
						std::min(float(state.motionTimestamp - motionTimestamp) / 1000000.0f, 0.25f) : 1.0f / 180.0f;
					constexpr float radians = 0.017453292519943295f;
					// Match the existing VPAD motion path; verify physical axes on Android hardware.
					motion.processMotionSample(dt, state.gyro[0] * radians,
						state.gyro[1] * radians, state.gyro[2] * radians,
						state.acceleration[0], -state.acceleration[1], -state.acceleration[2]);
					motionTimestamp = state.motionTimestamp;
				}
				else
					state.motionTimestamp = 0;
				if (!(features & RemoteGamePadProtocol::kTouch))
					state.touchActive = false;
				rxSequence = state.sequence;
				haveState = true;
				lastState = std::chrono::steady_clock::now();
				std::scoped_lock lock(m_mutex);
				m_snapshot.state = state;
				m_snapshot.motion = features & RemoteGamePadProtocol::kMotion ? motion.getMotionSample() : MotionSample{};
				m_snapshot.features = features;
				m_snapshot.connected = true;
				m_lastState = lastState;
				m_error.clear();
				m_errorReported = false;
			}
		}
		if (rumbleSent) sendRumble(false);
		if (!m_stop.load(std::memory_order_acquire))
		{
			ReportError("UDP connection failed.");
			pause(2s);
		}
		std::scoped_lock lock(m_mutex);
		m_snapshot = {};
	}
}
