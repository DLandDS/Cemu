#include "Cafe/HW/Latte/Renderer/GamePadSrtStreamer.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <iterator>
#include <string_view>
#include <system_error>
#include <unordered_map>

#ifdef ENABLE_GSTREAMER_SRT
#include "audio/IAudioAPI.h"
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#ifdef CEMU_GST_DYNAMIC_PLUGINS
#include "config/ActiveSettings.h"
#include <filesystem>
#endif
#endif

namespace
{
	uint64_t SteadyNowNs()
	{
		return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count());
	}

	void UpdateMax(std::atomic_uint64_t& maximum, uint64_t value)
	{
		auto previous = maximum.load(std::memory_order_relaxed);
		while (previous < value && !maximum.compare_exchange_weak(previous, value, std::memory_order_relaxed)) {}
	}
#ifdef ENABLE_GSTREAMER_SRT
	void SetLocalGamePadVolume(bool mute)
	{
		std::unique_lock lock(g_audioMutex);
		if (g_padAudio)
			g_padAudio->SetVolume(mute ? 0 : g_padVolume.load(std::memory_order_relaxed));
	}
#endif
}

GamePadSrtStreamer& GamePadSrtStreamer::Instance()
{
	static GamePadSrtStreamer instance;
	return instance;
}

GamePadSrtStreamer::~GamePadSrtStreamer()
{
	Stop();
}

bool GamePadSrtStreamer::ValidateCallerUri(std::string_view uri, std::string& error)
{
	constexpr std::string_view message = "Video URL must be an SRT caller destination with a host and port.";
	if (!uri.starts_with("srt://") || uri.find_first_of(" \t\r\n") != std::string_view::npos)
	{
		error = message;
		return false;
	}
	const auto authorityEnd = uri.find_first_of("/?", 6);
	const auto authority = uri.substr(6, authorityEnd == std::string_view::npos ?
		std::string_view::npos : authorityEnd - 6);
	const auto colon = authority.rfind(':');
	if (colon == std::string_view::npos || colon == 0 || authority.find('@') != std::string_view::npos)
	{
		error = message;
		return false;
	}
	const auto host = authority.substr(0, colon);
	if (host == "0.0.0.0" || host == "[::]" || host.empty() ||
		(host.starts_with('[') != host.ends_with(']')))
	{
		error = message;
		return false;
	}
	uint32_t port = 0;
	const auto portText = authority.substr(colon + 1);
	if (portText.empty() || portText.size() > 5)
	{
		error = message;
		return false;
	}
	for (char digit : portText)
	{
		if (digit < '0' || digit > '9')
		{
			error = message;
			return false;
		}
		port = port * 10 + uint32_t(digit - '0');
	}
	if (!port || port > 65535 || authorityEnd == std::string_view::npos || uri[authorityEnd] != '?')
	{
		error = message;
		return false;
	}
	bool caller = false;
	for (size_t start = authorityEnd + 1; start < uri.size(); )
	{
		const auto end = uri.find('&', start);
		const auto option = uri.substr(start, end == std::string_view::npos ? end : end - start);
		if (option.starts_with("mode="))
		{
			if (caller || option != "mode=caller")
			{
				error = message;
				return false;
			}
			caller = true;
		}
		if (end == std::string_view::npos) break;
		start = end + 1;
	}
	if (!caller) error = message;
	return caller;
}

bool GamePadSrtStreamer::Start(const std::string& uri, Encoder encoder, std::string& error)
{
	if (!ValidateCallerUri(uri, error))
		return false;
#ifndef ENABLE_GSTREAMER_SRT
	(void)encoder;
	error = "This Cemu build does not include GStreamer SRT support.";
	return false;
#else
	Stop();
	// Keep the full URI out of logs and error messages: its query can contain a passphrase.
	if (!uri.starts_with("srt://"))
	{
		error = "Enter an SRT URI with a host and port in SRT stream settings.";
		return false;
	}
	const auto authorityEnd = uri.find_first_of("/?", 6);
	const std::string authority = uri.substr(6, authorityEnd == std::string::npos ? std::string::npos : authorityEnd - 6);
	const auto colon = authority.rfind(':');
	bool validPort = colon != std::string::npos && colon + 1 < authority.size() &&
		authority.size() - colon - 1 <= 5;
	unsigned port = 0;
	if (validPort)
	{
		for (size_t i = colon + 1; i < authority.size(); ++i)
		{
			if (authority[i] < '0' || authority[i] > '9')
			{
				validPort = false;
				break;
			}
			port = port * 10 + unsigned(authority[i] - '0');
			if (port > 65535)
			{
				validPort = false;
				break;
			}
		}
	}
	std::string_view mode = "caller";
	bool validMode = true;
	if (const auto queryStart = uri.find('?'); queryStart != std::string::npos)
	{
		for (size_t paramStart = queryStart + 1; paramStart < uri.size(); )
		{
			const auto paramEnd = uri.find('&', paramStart);
			const std::string_view param(uri.data() + paramStart,
				(paramEnd == std::string::npos ? uri.size() : paramEnd) - paramStart);
			if (param == "mode=caller") mode = "caller";
			else if (param == "mode=listener") mode = "listener";
			else if (param == "mode=rendezvous") mode = "rendezvous";
			else if (param.starts_with("mode=")) validMode = false;
			if (paramEnd == std::string::npos)
				break;
			paramStart = paramEnd + 1;
		}
	}
	if (authority.empty() || (colon == 0 && mode != "listener") || authority.find('@') != std::string::npos ||
		!validPort || !validMode || port == 0 ||
		uri.find_first_of(" \t\r\n") != std::string::npos)
	{
		error = "Enter an SRT URI with a host and port in SRT stream settings.";
		return false;
	}
	{
		std::lock_guard lock(m_mutex);
		m_error.clear();
		m_frames.clear();
		m_audioBlocks.clear();
		m_audioDiscontinuity = true;
	}
	m_stop.store(false, std::memory_order_release);
	m_connected.store(false, std::memory_order_release);
	m_captureDrops.store(0, std::memory_order_relaxed);
	m_captureFrames.store(0, std::memory_order_relaxed);
	m_queueDrops.store(0, std::memory_order_relaxed);
	m_audioQueueDrops.store(0, std::memory_order_relaxed);
	m_readbackFrames.store(0, std::memory_order_relaxed);
	m_readbackTotalNs.store(0, std::memory_order_relaxed);
	m_readbackMaxNs.store(0, std::memory_order_relaxed);
	m_generation.fetch_add(1, std::memory_order_acq_rel);
	m_captureRequested.store(true, std::memory_order_release);
	cemuLog_log(LogType::Force, "GamePad SRT: starting {}x{} RGBA + GamePad PCM -> H.264/AAC MPEG-TS at srt://{}:{}, mode {}",
		kWidth, kHeight, authority.substr(0, colon), port,
		mode);
	try
	{
		m_worker = std::thread(&GamePadSrtStreamer::Worker, this, uri, encoder);
	}
	catch (const std::system_error&)
	{
		m_captureRequested.store(false, std::memory_order_release);
		m_stop.store(true, std::memory_order_release);
		error = "Could not start the GamePad SRT worker thread.";
		return false;
	}
	return true;
#endif
}

void GamePadSrtStreamer::Stop()
{
	m_generation.fetch_add(1, std::memory_order_acq_rel);
	m_captureRequested.store(false, std::memory_order_release);
	m_stop.store(true, std::memory_order_release);
	[[maybe_unused]] const bool wasConnected = m_connected.exchange(false, std::memory_order_acq_rel);
	m_wake.notify_all();
	if (m_worker.joinable())
		m_worker.join();
#ifdef ENABLE_GSTREAMER_SRT
	if (wasConnected)
		SetLocalGamePadVolume(false);
#endif
	m_running.store(false, std::memory_order_release);
	std::lock_guard lock(m_mutex);
	m_frames.clear();
	m_audioBlocks.clear();
	m_audioDiscontinuity = true;
	m_error.clear();
}

std::string GamePadSrtStreamer::TakeError()
{
	std::lock_guard lock(m_mutex);
	std::string error;
	error.swap(m_error);
	return error;
}

void GamePadSrtStreamer::Fail(std::string error)
{
	cemuLog_log(LogType::Force, "GamePad SRT: {}", error);
	{
		std::lock_guard lock(m_mutex);
		m_error = std::move(error);
		m_frames.clear();
		m_audioBlocks.clear();
		m_audioDiscontinuity = true;
	}
	m_captureRequested.store(false, std::memory_order_release);
	m_running.store(false, std::memory_order_release);
	m_stop.store(true, std::memory_order_release);
	[[maybe_unused]] const bool wasConnected = m_connected.exchange(false, std::memory_order_acq_rel);
#ifdef ENABLE_GSTREAMER_SRT
	if (wasConnected)
		SetLocalGamePadVolume(false);
#endif
	m_wake.notify_all();
}

void GamePadSrtStreamer::SubmitFrame(const uint8_t* rgba, size_t size, uint64_t captureTimestampNs)
{
	constexpr size_t frameSize = size_t(kWidth) * kHeight * 4;
	if (!IsCaptureRequested() || !rgba || size != frameSize)
		return;
	Frame frame;
	frame.timestampNs = captureTimestampNs;
	frame.pixels.assign(rgba, rgba + size);
	{
		std::lock_guard lock(m_mutex);
		if (m_stop.load(std::memory_order_acquire))
			return;
		// A late frame cannot be useful to an interactive receiver.
		m_queueDrops.fetch_add(m_frames.size(), std::memory_order_relaxed);
		m_frames.clear();
		m_frames.emplace_back(std::move(frame));
	}
	m_wake.notify_one();
}

void GamePadSrtStreamer::SubmitAudio(const int16_t* stereo, size_t frames, uint64_t firstSampleTimestampNs)
{
	if (!IsCaptureRequested() || !stereo || frames != kAudioFramesPerBlock)
		return;
	AudioBlock block;
	std::copy_n(stereo, block.samples.size(), block.samples.begin());
	block.timestampNs = firstSampleTimestampNs;
	{
		std::unique_lock lock(m_mutex, std::try_to_lock);
		if (!lock.owns_lock())
		{
			m_audioQueueDrops.fetch_add(1, std::memory_order_relaxed);
			m_audioDiscontinuity.store(true, std::memory_order_release);
			return;
		}
		if (m_stop.load(std::memory_order_acquire))
			return;
		constexpr size_t maxBlocks = 4; // 48 ms of PCM, even if the encoder stalls.
		if (m_audioBlocks.size() >= maxBlocks)
		{
			m_audioBlocks.pop_front();
			m_audioQueueDrops.fetch_add(1, std::memory_order_relaxed);
			if (!m_audioBlocks.empty())
				m_audioBlocks.front().discontinuity = true;
			else
				m_audioDiscontinuity = true;
		}
		block.discontinuity = m_audioDiscontinuity.exchange(false, std::memory_order_acq_rel);
		m_audioBlocks.emplace_back(std::move(block));
	}
	m_wake.notify_one();
}

void GamePadSrtStreamer::RecordReadbackCompletion(uint64_t captureTimestampNs)
{
	const auto readbackNs = SteadyNowNs() - captureTimestampNs;
	m_readbackFrames.fetch_add(1, std::memory_order_relaxed);
	m_readbackTotalNs.fetch_add(readbackNs, std::memory_order_relaxed);
	UpdateMax(m_readbackMaxNs, readbackNs);
}

#ifdef ENABLE_GSTREAMER_SRT
namespace
{
	struct PipelineTiming
	{
		std::atomic_uint64_t firstCaptureNs{0};
		std::mutex mutex;
		std::unordered_map<GstClockTime, uint64_t> encoderInputs;
		uint64_t encodedFrames = 0;
		uint64_t encodeSamples = 0;
		uint64_t encodeTotalNs = 0;
		uint64_t encodeMaxNs = 0;
		uint64_t outputAgeSamples = 0;
		uint64_t outputAgeTotalNs = 0;
		uint64_t outputAgeMaxNs = 0;
		uint64_t lastEncodedNs = 0;
		uint64_t lastSrtInputEncodedNs = 0;
		uint64_t srtInputBuffers = 0;
		uint64_t srtInputSamples = 0;
		uint64_t srtInputTotalNs = 0;
		uint64_t srtInputMaxNs = 0;
	};

	GstPadProbeReturn OnEncoderInput(GstPad*, GstPadProbeInfo* info, gpointer data)
	{
		auto* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
		if (buffer && GST_CLOCK_TIME_IS_VALID(GST_BUFFER_PTS(buffer)))
		{
			auto& timing = *static_cast<PipelineTiming*>(data);
			std::lock_guard lock(timing.mutex);
			if (timing.encoderInputs.size() >= 64)
				timing.encoderInputs.clear();
			timing.encoderInputs[GST_BUFFER_PTS(buffer)] = SteadyNowNs();
		}
		return GST_PAD_PROBE_OK;
	}

	GstPadProbeReturn OnEncoderOutput(GstPad*, GstPadProbeInfo* info, gpointer data)
	{
		auto* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
		if (!buffer)
			return GST_PAD_PROBE_OK;
		auto& timing = *static_cast<PipelineTiming*>(data);
		const auto now = SteadyNowNs();
		const auto pts = GST_BUFFER_PTS(buffer);
		std::lock_guard lock(timing.mutex);
		++timing.encodedFrames;
		timing.lastEncodedNs = now;
		if (GST_CLOCK_TIME_IS_VALID(pts))
		{
			if (auto it = timing.encoderInputs.find(pts); it != timing.encoderInputs.end())
			{
				const auto encodeNs = now - it->second;
				++timing.encodeSamples;
				timing.encodeTotalNs += encodeNs;
				timing.encodeMaxNs = std::max(timing.encodeMaxNs, encodeNs);
				timing.encoderInputs.erase(it);
			}
			const auto captured = timing.firstCaptureNs.load(std::memory_order_acquire) + pts;
			if (captured <= now)
			{
				const auto ageNs = now - captured;
				++timing.outputAgeSamples;
				timing.outputAgeTotalNs += ageNs;
				timing.outputAgeMaxNs = std::max(timing.outputAgeMaxNs, ageNs);
			}
		}
		return GST_PAD_PROBE_OK;
	}

	GstPadProbeReturn OnSrtInput(GstPad*, GstPadProbeInfo* info, gpointer data)
	{
		if (!GST_PAD_PROBE_INFO_BUFFER(info))
			return GST_PAD_PROBE_OK;
		auto& timing = *static_cast<PipelineTiming*>(data);
		const auto now = SteadyNowNs();
		std::lock_guard lock(timing.mutex);
		++timing.srtInputBuffers;
		if (timing.lastEncodedNs && timing.lastSrtInputEncodedNs != timing.lastEncodedNs &&
			timing.lastEncodedNs <= now)
		{
			const auto elapsedNs = now - timing.lastEncodedNs;
			timing.lastSrtInputEncodedNs = timing.lastEncodedNs;
			++timing.srtInputSamples;
			timing.srtInputTotalNs += elapsedNs;
			timing.srtInputMaxNs = std::max(timing.srtInputMaxNs, elapsedNs);
		}
		return GST_PAD_PROBE_OK;
	}
}
#endif

void GamePadSrtStreamer::Worker(std::string uri, Encoder requestedEncoder)
{
#ifdef ENABLE_GSTREAMER_SRT
#ifdef CEMU_GST_FULL_STATIC
	g_setenv("GST_PLUGIN_SYSTEM_PATH", "", TRUE);
	g_setenv("GST_PLUGIN_SYSTEM_PATH_1_0", "", TRUE);
	g_setenv("GST_PLUGIN_PATH", "", TRUE);
	g_setenv("GST_PLUGIN_PATH_1_0", "", TRUE);
#endif
#ifdef CEMU_GST_DYNAMIC_PLUGINS
	const auto pluginDir = ActiveSettings::GetExecutablePath().parent_path() / "plugins" / "gstreamer";
	if (!std::filesystem::exists(pluginDir))
	{
		Fail("Packaged vcpkg GStreamer plugins are missing.");
		return;
	}
	const auto pluginDirUtf8 = pluginDir.u8string();
	const std::string pluginPath(pluginDirUtf8.begin(), pluginDirUtf8.end());
	g_setenv("GST_PLUGIN_SYSTEM_PATH", "", TRUE);
	g_setenv("GST_PLUGIN_SYSTEM_PATH_1_0", "", TRUE);
	g_setenv("GST_PLUGIN_PATH", pluginPath.c_str(), TRUE);
	g_setenv("GST_PLUGIN_PATH_1_0", pluginPath.c_str(), TRUE);
#endif
	GError* initError = nullptr;
	if (!gst_init_check(nullptr, nullptr, &initError))
	{
		if (initError)
			g_error_free(initError);
		Fail("GStreamer could not initialize.");
		return;
	}
	for (const char* name : {"appsrc", "videoconvert", "audioconvert", "capsfilter", "h264parse",
			"avenc_aac", "aacparse", "mpegtsmux", "srtsink"})
	{
		GstElementFactory* factory = gst_element_factory_find(name);
		if (!factory)
		{
			Fail(std::string("Missing GStreamer element: ") + name);
			return;
		}
		gst_object_unref(factory);
	}

	const std::array<Encoder, 2> attempts{
		requestedEncoder == Encoder::Auto ? Encoder::QuickSync : requestedEncoder, Encoder::OpenH264};
	const size_t attemptCount = requestedEncoder == Encoder::Auto ? 2 : 1;
	for (size_t attempt = 0; attempt < attemptCount && !m_stop.load(std::memory_order_acquire); ++attempt)
	{
		const bool quickSync = attempts[attempt] == Encoder::QuickSync;
		const char* encoderName = quickSync ? "qsvh264enc" : "openh264enc";
		const char* displayName = quickSync ? "Intel Quick Sync" : "OpenH264";
		GstElementFactory* encoderFactory = gst_element_factory_find(encoderName);
		if (!encoderFactory)
		{
			if (quickSync && requestedEncoder == Encoder::Auto)
			{
				cemuLog_log(LogType::Force, "GamePad SRT: Intel Quick Sync unavailable; using OpenH264");
				continue;
			}
			Fail(quickSync ? "Intel Quick Sync H.264 is unavailable on this device or build." :
				"Missing GStreamer element: openh264enc");
			return;
		}
		gst_object_unref(encoderFactory);

		const std::array<const char*, 12> names{
			"appsrc", "videoconvert", "capsfilter", encoderName, "h264parse", "mpegtsmux", "srtsink",
			"appsrc", "audioconvert", "avenc_aac", "aacparse", "capsfilter"};
		GstElement* pipeline = gst_pipeline_new("gamepad-srt");
		std::array<GstElement*, names.size()> elements{};
		bool created = pipeline != nullptr;
		for (size_t i = 0; created && i < names.size(); ++i)
		{
			elements[i] = gst_element_factory_make(names[i], nullptr);
			created = elements[i] != nullptr;
		}
		if (!created)
		{
			for (GstElement* element : elements)
				if (element) gst_object_unref(element);
			if (pipeline) gst_object_unref(pipeline);
			if (quickSync && requestedEncoder == Encoder::Auto)
			{
				cemuLog_log(LogType::Force, "GamePad SRT: Intel Quick Sync could not initialize; using OpenH264");
				continue;
			}
			Fail(quickSync ? "Intel Quick Sync H.264 could not initialize." :
				"Could not create the GStreamer SRT pipeline.");
			return;
		}

		GstElement* source = elements[0];
		GstElement* videoEncoder = elements[3];
		GstElement* sink = elements[6];
		GstElement* audioSource = elements[7];
		GstElement* audioEncoder = elements[9];
		g_object_set(source, "is-live", TRUE, "format", GST_FORMAT_TIME, "block", FALSE,
			"emit-signals", FALSE, "max-buffers", guint64(1),
			"max-bytes", guint64(kWidth) * kHeight * 4, nullptr);
		gst_app_src_set_leaky_type(GST_APP_SRC(source), GST_APP_LEAKY_TYPE_DOWNSTREAM);
		GstCaps* sourceCaps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "RGBA",
			"width", G_TYPE_INT, int(kWidth), "height", G_TYPE_INT, int(kHeight),
			"framerate", GST_TYPE_FRACTION, 60, 1, nullptr);
		gst_app_src_set_caps(GST_APP_SRC(source), sourceCaps);
		gst_caps_unref(sourceCaps);
		GstCaps* encoderCaps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING,
			quickSync ? "NV12" : "I420", nullptr);
		g_object_set(elements[2], "caps", encoderCaps, nullptr);
		gst_caps_unref(encoderCaps);
		if (quickSync)
		{
			g_object_set(videoEncoder, "bitrate", guint(4000), "gop-size", guint(60),
				"b-frames", guint(0), "rate-control", 1, "low-latency", TRUE,
				"target-usage", guint(7), nullptr);
		}
		else
		{
			g_object_set(videoEncoder, "bitrate", guint(4000000), "gop-size", guint(60),
				"complexity", 0, "usage-type", 1, "rate-control", 1, nullptr);
		}
		g_object_set(elements[4], "config-interval", -1, nullptr);
		g_object_set(elements[5], "alignment", 7, nullptr);
		g_object_set(sink, "uri", uri.c_str(), "auto-reconnect", FALSE, "poll-timeout", 100, nullptr);
		g_object_set(audioSource, "is-live", TRUE, "format", GST_FORMAT_TIME, "block", FALSE,
			"emit-signals", FALSE, "max-buffers", guint64(4),
			"max-time", guint64(48 * GST_MSECOND), nullptr);
		gst_app_src_set_leaky_type(GST_APP_SRC(audioSource), GST_APP_LEAKY_TYPE_DOWNSTREAM);
		GstCaps* audioSourceCaps = gst_caps_new_simple("audio/x-raw", "format", G_TYPE_STRING, "S16LE",
			"layout", G_TYPE_STRING, "interleaved", "rate", G_TYPE_INT, 48000,
			"channels", G_TYPE_INT, 2, nullptr);
		gst_app_src_set_caps(GST_APP_SRC(audioSource), audioSourceCaps);
		gst_caps_unref(audioSourceCaps);
		g_object_set(audioEncoder, "bitrate", 128000, "aac-coder", 2, "threads", 1, nullptr);
		GstCaps* adtsCaps = gst_caps_new_simple("audio/mpeg", "mpegversion", G_TYPE_INT, 4,
			"stream-format", G_TYPE_STRING, "adts", nullptr);
		g_object_set(elements[11], "caps", adtsCaps, nullptr);
		gst_caps_unref(adtsCaps);

		for (GstElement* element : elements)
			gst_bin_add(GST_BIN(pipeline), element);
		bool linked = true;
		for (size_t i = 1; i <= 6; ++i)
			linked = gst_element_link(elements[i - 1], elements[i]) && linked;
		for (size_t i = 8; i < elements.size(); ++i)
			linked = gst_element_link(elements[i - 1], elements[i]) && linked;
		linked = gst_element_link(elements[11], elements[5]) && linked;
		if (!linked)
		{
			gst_element_set_state(pipeline, GST_STATE_NULL);
			gst_object_unref(pipeline);
			if (quickSync && requestedEncoder == Encoder::Auto)
			{
				cemuLog_log(LogType::Force, "GamePad SRT: Intel Quick Sync could not link; using OpenH264");
				continue;
			}
			Fail(quickSync ? "Intel Quick Sync H.264 could not link to the SRT pipeline." :
				"Could not link the GStreamer SRT pipeline.");
			return;
		}

		PipelineTiming timing;
		GstBus* bus = gst_element_get_bus(pipeline);
		GstPad* encoderInputPad = gst_element_get_static_pad(videoEncoder, "sink");
		GstPad* encoderOutputPad = gst_element_get_static_pad(videoEncoder, "src");
		GstPad* srtInputPad = gst_element_get_static_pad(sink, "sink");
		const gulong inputProbe = encoderInputPad ?
			gst_pad_add_probe(encoderInputPad, GST_PAD_PROBE_TYPE_BUFFER, OnEncoderInput, &timing, nullptr) : 0;
		const gulong outputProbe = encoderOutputPad ?
			gst_pad_add_probe(encoderOutputPad, GST_PAD_PROBE_TYPE_BUFFER, OnEncoderOutput, &timing, nullptr) : 0;
		const gulong srtProbe = srtInputPad ?
			gst_pad_add_probe(srtInputPad, GST_PAD_PROBE_TYPE_BUFFER, OnSrtInput, &timing, nullptr) : 0;
		auto releasePipeline = [&] {
			gst_element_set_state(pipeline, GST_STATE_NULL);
			if (inputProbe) gst_pad_remove_probe(encoderInputPad, inputProbe);
			if (outputProbe) gst_pad_remove_probe(encoderOutputPad, outputProbe);
			if (srtProbe) gst_pad_remove_probe(srtInputPad, srtProbe);
			if (encoderInputPad) gst_object_unref(encoderInputPad);
			if (encoderOutputPad) gst_object_unref(encoderOutputPad);
			if (srtInputPad) gst_object_unref(srtInputPad);
			if (bus) gst_object_unref(bus);
			gst_object_unref(pipeline);
		};
		if (!bus || !inputProbe || !outputProbe || !srtProbe ||
			gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
		{
			releasePipeline();
			if (quickSync && requestedEncoder == Encoder::Auto)
			{
				cemuLog_log(LogType::Force, "GamePad SRT: Intel Quick Sync could not start; using OpenH264");
				continue;
			}
			Fail(quickSync ? "Intel Quick Sync H.264 could not start." :
				"Could not start the GStreamer SRT pipeline.");
			return;
		}

		cemuLog_log(LogType::Force, "GamePad SRT: using {}", displayName);
		constexpr uint64_t audioDurationNs = kAudioFramesPerBlock * GST_SECOND / 48000;
		const uint64_t epochNs = SteadyNowNs();
		timing.firstCaptureNs.store(epochNs, std::memory_order_release);
		{
			std::lock_guard lock(m_mutex);
			m_audioBlocks.clear();
			m_audioDiscontinuity = true;
		}
		m_running.store(true, std::memory_order_release);
		uint64_t lastPts = 0;
		uint64_t nextAudioPts = 0;
		bool haveAudioPts = false;
		uint64_t staleDrops = 0;
		uint64_t staleAudioDrops = 0;
		uint64_t appsrcFullEvents = 0;
		uint64_t lastReportNs = SteadyNowNs();
		uint64_t lastCaptures = m_captureFrames.load(std::memory_order_relaxed);
		uint64_t lastEncodedFrames = 0;
		uint64_t lastSentBytes = 0;
		bool retrySoftware = false;
		std::string failure;
		while (!m_stop.load(std::memory_order_acquire))
		{
			if (GstMessage* message = gst_bus_pop_filtered(bus, GstMessageType(GST_MESSAGE_ERROR | GST_MESSAGE_EOS)))
			{
				bool mediaFailure = false;
				if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR)
				{
					GstObject* messageSource = GST_MESSAGE_SRC(message);
					mediaFailure = messageSource &&
						(messageSource == GST_OBJECT(source) || messageSource == GST_OBJECT(elements[1]) ||
							messageSource == GST_OBJECT(elements[2]) || messageSource == GST_OBJECT(videoEncoder) ||
							messageSource == GST_OBJECT(elements[4]) ||
							gst_object_has_as_ancestor(messageSource, GST_OBJECT(videoEncoder)));
				}
				retrySoftware = quickSync && requestedEncoder == Encoder::Auto && mediaFailure;
				failure = quickSync && mediaFailure ?
					"Intel Quick Sync H.264 stopped before it could encode a frame." :
					"The SRT pipeline or connection stopped. Check the receiver and GStreamer plugins.";
				// GStreamer errors can include the URI; never forward their text to UI/logs.
				gst_message_unref(message);
				break;
			}

			const auto now = SteadyNowNs();
			GstStructure* currentSrtStats = nullptr;
			g_object_get(sink, "stats", &currentSrtStats, nullptr);
			guint64 sentBytes = 0;
			gint srtLatencyMs = 0;
			gdouble srtRttMs = 0;
			if (currentSrtStats)
			{
				gst_structure_get_uint64(currentSrtStats, "bytes-sent-total", &sentBytes);
				gst_structure_get_int(currentSrtStats, "negotiated-latency-ms", &srtLatencyMs);
				gst_structure_get_double(currentSrtStats, "rtt-ms", &srtRttMs);
				gst_structure_free(currentSrtStats);
			}
			if (sentBytes > 0 && !m_stop.load(std::memory_order_acquire) &&
				!m_connected.exchange(true, std::memory_order_acq_rel))
				SetLocalGamePadVolume(true);
			if (now - lastReportNs >= 5 * GST_SECOND)
			{
				uint64_t encodedFrames, encodeSamples, encodeTotalNs, encodeMaxNs;
				uint64_t outputAgeSamples, outputAgeTotalNs, outputAgeMaxNs;
				uint64_t srtInputBuffers, srtInputSamples, srtInputTotalNs, srtInputMaxNs;
				{
					std::lock_guard lock(timing.mutex);
					encodedFrames = timing.encodedFrames;
					encodeSamples = timing.encodeSamples;
					encodeTotalNs = timing.encodeTotalNs;
					encodeMaxNs = timing.encodeMaxNs;
					outputAgeSamples = timing.outputAgeSamples;
					outputAgeTotalNs = timing.outputAgeTotalNs;
					outputAgeMaxNs = timing.outputAgeMaxNs;
					srtInputBuffers = timing.srtInputBuffers;
					srtInputSamples = timing.srtInputSamples;
					srtInputTotalNs = timing.srtInputTotalNs;
					srtInputMaxNs = timing.srtInputMaxNs;
				}
				const auto readbacks = m_readbackFrames.load(std::memory_order_relaxed);
				const auto captures = m_captureFrames.load(std::memory_order_relaxed);
				const double seconds = double(now - lastReportNs) / double(GST_SECOND);
				const auto readbackTotalNs = m_readbackTotalNs.load(std::memory_order_relaxed);
				cemuLog_log(LogType::Force,
					"GamePad SRT stats ({}): capture {:.1f} fps, encoded {:.1f} fps, readback avg {:.1f}/max {:.1f} ms, encode avg {:.1f}/max {:.1f} ms, encoded age avg {:.1f}/max {:.1f} ms, SRT input avg {:.1f}/max {:.1f} ms ({} buffers), SRT latency {} ms RTT {:.1f} ms, sent {:.1f} Mbit/s, dropped GPU {} queue {} stale {}, appsrc full {}, audio queue {} stale {}",
					displayName, double(captures - lastCaptures) / seconds,
					double(encodedFrames - lastEncodedFrames) / seconds,
					readbacks ? double(readbackTotalNs) / readbacks / 1e6 : 0.0,
					double(m_readbackMaxNs.load(std::memory_order_relaxed)) / 1e6,
					encodeSamples ? double(encodeTotalNs) / encodeSamples / 1e6 : 0.0,
					double(encodeMaxNs) / 1e6,
					outputAgeSamples ? double(outputAgeTotalNs) / outputAgeSamples / 1e6 : 0.0,
					double(outputAgeMaxNs) / 1e6,
					srtInputSamples ? double(srtInputTotalNs) / srtInputSamples / 1e6 : 0.0,
					double(srtInputMaxNs) / 1e6, srtInputBuffers, srtLatencyMs, srtRttMs,
					double(sentBytes - lastSentBytes) * 8.0 / seconds / 1e6,
					m_captureDrops.load(std::memory_order_relaxed),
					m_queueDrops.load(std::memory_order_relaxed), staleDrops, appsrcFullEvents,
					m_audioQueueDrops.load(std::memory_order_relaxed), staleAudioDrops);
				lastReportNs = now;
				lastCaptures = captures;
				lastEncodedFrames = encodedFrames;
				lastSentBytes = sentBytes;
			}

			Frame frame;
			AudioBlock audio;
			bool haveFrame = false;
			bool haveAudio = false;
			{
				std::unique_lock lock(m_mutex);
				const auto audioDeadline = std::chrono::steady_clock::time_point(
					std::chrono::duration_cast<std::chrono::steady_clock::duration>(
						std::chrono::nanoseconds(epochNs + nextAudioPts + audioDurationNs + 2 * GST_MSECOND)));
				m_wake.wait_until(lock, audioDeadline,
					[this] { return m_stop.load() || !m_frames.empty() || !m_audioBlocks.empty(); });
				if (m_stop.load())
					break;
				if (!m_audioBlocks.empty())
				{
					audio = std::move(m_audioBlocks.front());
					m_audioBlocks.pop_front();
					haveAudio = true;
				}
				if (!m_frames.empty())
				{
					frame = std::move(m_frames.back());
					m_frames.clear();
					haveFrame = true;
				}
			}
			if (!haveAudio && SteadyNowNs() >= epochNs + nextAudioPts + audioDurationNs + 2 * GST_MSECOND)
			{
				const uint64_t nowNs = SteadyNowNs();
				if (nowNs > epochNs + nextAudioPts + 50 * GST_MSECOND)
				{
					nextAudioPts = nowNs - epochNs - audioDurationNs;
					haveAudioPts = false;
				}
				audio.timestampNs = epochNs + nextAudioPts;
				audio.discontinuity = !haveAudioPts;
				haveAudio = true; // Keep MPEG-TS video moving when no GamePad PCM is produced.
			}
			if (haveAudio)
			{
				if (audio.timestampNs < epochNs || SteadyNowNs() - audio.timestampNs > 100 * GST_MSECOND)
				{
					++staleAudioDrops;
					haveAudioPts = false;
				}
				else
				{
					uint64_t pts = audio.timestampNs - epochNs;
					bool discontinuity = audio.discontinuity || !haveAudioPts;
					if (haveAudioPts)
					{
						const uint64_t jitterNs = pts >= nextAudioPts ? pts - nextAudioPts : nextAudioPts - pts;
						if (jitterNs <= 4 * GST_MSECOND)
							pts = nextAudioPts;
						else
						{
							discontinuity = true;
							pts = std::max(pts, nextAudioPts);
						}
					}
					GstBuffer* buffer = gst_buffer_new_allocate(nullptr, sizeof(audio.samples), nullptr);
					if (!buffer)
					{
						failure = "GStreamer could not allocate an audio buffer.";
						break;
					}
					gst_buffer_fill(buffer, 0, audio.samples.data(), sizeof(audio.samples));
					GST_BUFFER_PTS(buffer) = pts;
					GST_BUFFER_DTS(buffer) = GST_CLOCK_TIME_NONE;
					GST_BUFFER_DURATION(buffer) = audioDurationNs;
					if (gst_app_src_get_current_level_buffers(GST_APP_SRC(audioSource)) >= 4)
						discontinuity = true;
					if (discontinuity)
						GST_BUFFER_FLAG_SET(buffer, GST_BUFFER_FLAG_DISCONT);
					if (gst_app_src_push_buffer(GST_APP_SRC(audioSource), buffer) != GST_FLOW_OK)
					{
						failure = "GStreamer could not accept GamePad audio.";
						break;
					}
					nextAudioPts = pts + audioDurationNs;
					haveAudioPts = true;
				}
			}
			if (!haveFrame)
				continue;
			if (frame.timestampNs < epochNs || SteadyNowNs() - frame.timestampNs > 150 * GST_MSECOND)
			{
				++staleDrops;
				continue;
			}
			uint64_t pts = frame.timestampNs - epochNs;
			pts = std::max(pts, lastPts + 1);
			lastPts = pts;
			GstBuffer* buffer = gst_buffer_new_allocate(nullptr, frame.pixels.size(), nullptr);
			if (!buffer)
			{
				failure = "GStreamer could not allocate a video buffer.";
				break;
			}
			gst_buffer_fill(buffer, 0, frame.pixels.data(), frame.pixels.size());
			GST_BUFFER_PTS(buffer) = pts;
			GST_BUFFER_DTS(buffer) = GST_CLOCK_TIME_NONE;
			GST_BUFFER_DURATION(buffer) = GST_SECOND / 60;
			if (gst_app_src_get_current_level_buffers(GST_APP_SRC(source)) >= 1)
				++appsrcFullEvents;
			if (gst_app_src_push_buffer(GST_APP_SRC(source), buffer) != GST_FLOW_OK)
			{
				bool encodedAny = false;
				{
					std::lock_guard lock(timing.mutex);
					encodedAny = timing.encodedFrames != 0;
				}
				retrySoftware = quickSync && requestedEncoder == Encoder::Auto && !encodedAny;
				failure = quickSync && !encodedAny ?
					"Intel Quick Sync H.264 could not accept a GamePad frame." :
					"GStreamer could not accept a GamePad frame.";
				break;
			}
		}
		if (m_connected.exchange(false, std::memory_order_acq_rel))
			SetLocalGamePadVolume(false);
		m_running.store(false, std::memory_order_release);
		releasePipeline();
		if (m_stop.load(std::memory_order_acquire))
			return;
		if (retrySoftware)
		{
			cemuLog_log(LogType::Force, "GamePad SRT: Intel Quick Sync failed; using OpenH264");
			continue;
		}
		Fail(failure.empty() ? "The SRT pipeline stopped." : failure);
		return;
	}
	if (!m_stop.load(std::memory_order_acquire))
		Fail("No GamePad SRT encoder is available.");
#endif
}
