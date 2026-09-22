#include "Cafe/HW/Latte/Renderer/GamePadSrtStreamer.h"

#include <algorithm>
#include <chrono>
#include <iterator>
#include <string_view>
#include <system_error>

#ifdef ENABLE_GSTREAMER_SRT
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#ifdef CEMU_GST_DYNAMIC_PLUGINS
#include "config/ActiveSettings.h"
#include <filesystem>
#endif
#endif

GamePadSrtStreamer& GamePadSrtStreamer::Instance()
{
	static GamePadSrtStreamer instance;
	return instance;
}

GamePadSrtStreamer::~GamePadSrtStreamer()
{
	Stop();
}

bool GamePadSrtStreamer::Start(const std::string& uri, std::string& error)
{
#ifndef ENABLE_GSTREAMER_SRT
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
	}
	m_stop.store(false, std::memory_order_release);
	m_generation.fetch_add(1, std::memory_order_acq_rel);
	m_captureRequested.store(true, std::memory_order_release);
	cemuLog_log(LogType::Force, "GamePad SRT: starting {}x{} RGBA -> H.264/MPEG-TS at srt://{}:{}, mode {}",
		kWidth, kHeight, authority.substr(0, colon), port,
		mode);
	try
	{
		m_worker = std::thread(&GamePadSrtStreamer::Worker, this, uri);
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
	m_wake.notify_all();
	if (m_worker.joinable())
		m_worker.join();
	m_running.store(false, std::memory_order_release);
	std::lock_guard lock(m_mutex);
	m_frames.clear();
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
	}
	m_captureRequested.store(false, std::memory_order_release);
	m_running.store(false, std::memory_order_release);
	m_stop.store(true, std::memory_order_release);
	m_wake.notify_all();
}

void GamePadSrtStreamer::SubmitFrame(const uint8_t* rgba, size_t size)
{
	constexpr size_t frameSize = size_t(kWidth) * kHeight * 4;
	if (!IsCaptureRequested() || !rgba || size != frameSize)
		return;
	Frame frame;
	frame.timestampNs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count());
	frame.pixels.assign(rgba, rgba + size);
	{
		std::lock_guard lock(m_mutex);
		if (m_stop.load(std::memory_order_acquire))
			return;
		// Retain only a few recent frames if encoding or the network falls behind.
		while (m_frames.size() >= 3)
			m_frames.pop_front();
		m_frames.emplace_back(std::move(frame));
	}
	m_wake.notify_one();
}

void GamePadSrtStreamer::Worker(std::string uri)
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
	constexpr const char* names[] = {"appsrc", "videoconvert", "openh264enc", "h264parse", "mpegtsmux", "srtsink"};
	for (const char* name : names)
	{
		GstElementFactory* factory = gst_element_factory_find(name);
		if (!factory)
		{
			Fail(std::string("Missing GStreamer element: ") + name);
			return;
		}
		gst_object_unref(factory);
	}

	GstElement* pipeline = gst_pipeline_new("gamepad-srt");
	GstElement* elements[std::size(names)]{};
	bool created = pipeline != nullptr;
	for (size_t i = 0; created && i < std::size(names); ++i)
	{
		elements[i] = gst_element_factory_make(names[i], nullptr);
		created = elements[i] != nullptr;
	}
	if (!created)
	{
		for (GstElement* element : elements)
			if (element) gst_object_unref(element);
		if (pipeline) gst_object_unref(pipeline);
		Fail("Could not create the GStreamer SRT pipeline.");
		return;
	}

	GstElement* source = elements[0];
	GstElement* sink = elements[5];
	g_object_set(source, "is-live", TRUE, "format", GST_FORMAT_TIME, "block", FALSE,
		"emit-signals", FALSE, "max-buffers", guint64(3),
		"max-bytes", guint64(kWidth) * kHeight * 4 * 3, nullptr);
	gst_app_src_set_leaky_type(GST_APP_SRC(source), GST_APP_LEAKY_TYPE_DOWNSTREAM);
	GstCaps* caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "RGBA",
		"width", G_TYPE_INT, int(kWidth), "height", G_TYPE_INT, int(kHeight),
		"framerate", GST_TYPE_FRACTION, 60, 1, nullptr);
	gst_app_src_set_caps(GST_APP_SRC(source), caps);
	gst_caps_unref(caps);
	g_object_set(elements[2], "bitrate", guint(4000000), "gop-size", guint(60),
		"complexity", 0, "usage-type", 1, "rate-control", 1, nullptr);
	g_object_set(elements[3], "config-interval", -1, nullptr);
	g_object_set(elements[4], "alignment", 7, nullptr);
	g_object_set(sink, "uri", uri.c_str(), "auto-reconnect", FALSE, "poll-timeout", 100, nullptr);

	for (GstElement* element : elements)
		gst_bin_add(GST_BIN(pipeline), element);
	bool linked = true;
	for (size_t i = 1; i < std::size(elements); ++i)
		linked = gst_element_link(elements[i - 1], elements[i]) && linked;
	if (!linked || gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
	{
		gst_element_set_state(pipeline, GST_STATE_NULL);
		gst_object_unref(pipeline);
		Fail("Could not start the GStreamer SRT pipeline.");
		return;
	}

	m_running.store(true, std::memory_order_release);
	GstBus* bus = gst_element_get_bus(pipeline);
	uint64_t firstTimestamp = 0;
	uint64_t lastPts = 0;
	while (!m_stop.load(std::memory_order_acquire))
	{
		if (GstMessage* message = gst_bus_pop_filtered(bus, GstMessageType(GST_MESSAGE_ERROR | GST_MESSAGE_EOS)))
		{
			// GStreamer errors can include the URI; never forward their text to UI/logs.
			gst_message_unref(message);
			Fail("The SRT pipeline or connection stopped. Check the receiver and GStreamer plugins.");
			break;
		}

		Frame frame;
		{
			std::unique_lock lock(m_mutex);
			m_wake.wait_for(lock, std::chrono::milliseconds(20), [this] { return m_stop.load() || !m_frames.empty(); });
			if (m_stop.load())
				break;
			if (m_frames.empty())
				continue;
			frame = std::move(m_frames.back());
			m_frames.clear();
		}
		if (!firstTimestamp)
			firstTimestamp = frame.timestampNs;
		uint64_t pts = frame.timestampNs - firstTimestamp;
		pts = std::max(pts, lastPts + 1);
		lastPts = pts;
		GstBuffer* buffer = gst_buffer_new_allocate(nullptr, frame.pixels.size(), nullptr);
		if (!buffer)
		{
			Fail("GStreamer could not allocate a video buffer.");
			break;
		}
		gst_buffer_fill(buffer, 0, frame.pixels.data(), frame.pixels.size());
		GST_BUFFER_PTS(buffer) = pts;
		GST_BUFFER_DTS(buffer) = GST_CLOCK_TIME_NONE;
		GST_BUFFER_DURATION(buffer) = GST_SECOND / 60;
		if (gst_app_src_push_buffer(GST_APP_SRC(source), buffer) != GST_FLOW_OK)
		{
			Fail("GStreamer could not accept a GamePad frame.");
			break;
		}
	}
	gst_object_unref(bus);
	gst_element_set_state(pipeline, GST_STATE_NULL);
	gst_object_unref(pipeline);
	m_running.store(false, std::memory_order_release);
#endif
}
