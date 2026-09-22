// Optional build and runtime probe for the vcpkg GStreamer feature. With no
// arguments it encodes synthetic frames to fakesink; pass an SRT URI to send.
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string_view>
#include <thread>
#include <vector>

int main(int argc, char** argv)
{
	#ifdef CEMU_GST_FULL_STATIC
	g_setenv("GST_PLUGIN_SYSTEM_PATH", "", TRUE);
	g_setenv("GST_PLUGIN_SYSTEM_PATH_1_0", "", TRUE);
	g_setenv("GST_PLUGIN_PATH", "", TRUE);
	g_setenv("GST_PLUGIN_PATH_1_0", "", TRUE);
	#endif
	#ifdef CEMU_GST_DYNAMIC_PLUGINS
	g_setenv("GST_PLUGIN_SYSTEM_PATH", "", TRUE);
	g_setenv("GST_PLUGIN_SYSTEM_PATH_1_0", "", TRUE);
	g_setenv("GST_PLUGIN_PATH", CEMU_GST_SMOKE_PLUGIN_DIR, TRUE);
	g_setenv("GST_PLUGIN_PATH_1_0", CEMU_GST_SMOKE_PLUGIN_DIR, TRUE);
	#endif
	if (!gst_init_check(nullptr, nullptr, nullptr))
		return 1;
	bool quickSync = false;
	const char* uri = nullptr;
	for (int i = 1; i < argc; ++i)
	{
		const std::string_view arg(argv[i]);
		if (arg == "--encoder=qsv") quickSync = true;
		else if (arg == "--encoder=openh264") quickSync = false;
		else if (arg.starts_with("srt://") && !uri) uri = argv[i];
		else
		{
			std::cerr << "Usage: GamePadSrtSmoke [--encoder=qsv|--encoder=openh264] [srt://host:port?mode=...]\n";
			return 1;
		}
	}
	const char* encoderName = quickSync ? "qsvh264enc" : "openh264enc";
	for (const char* name : {"appsrc", "videoconvert", "capsfilter", encoderName, "h264parse", "mpegtsmux", "srtsink"})
	{
		GstElementFactory* factory = gst_element_factory_find(name);
		if (!factory)
		{
			std::cerr << "Missing GStreamer element: " << name << '\n';
			return 2;
		}
		gst_object_unref(factory);
	}

	constexpr int width = 854;
	constexpr int height = 480;
	const bool sendToSrt = uri != nullptr;
	const std::array<const char*, 7> names{"appsrc", "videoconvert", "capsfilter", encoderName,
		"h264parse", "mpegtsmux", sendToSrt ? "srtsink" : "fakesink"};
	GstElement* pipeline = gst_pipeline_new("srt-smoke");
	std::array<GstElement*, names.size()> elements{};
	if (!pipeline)
		return 3;
	for (size_t i = 0; i < names.size(); ++i)
	{
		elements[i] = gst_element_factory_make(names[i], nullptr);
		if (!elements[i])
			return 3;
		gst_bin_add(GST_BIN(pipeline), elements[i]);
		if (i && !gst_element_link(elements[i - 1], elements[i]))
			return 4;
	}
	GstCaps* caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "RGBA",
		"width", G_TYPE_INT, width, "height", G_TYPE_INT, height,
		"framerate", GST_TYPE_FRACTION, 30, 1, nullptr);
	gst_app_src_set_caps(GST_APP_SRC(elements[0]), caps);
	gst_caps_unref(caps);
	g_object_set(elements[0], "is-live", TRUE, "format", GST_FORMAT_TIME, nullptr);
	GstCaps* encoderCaps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING,
		quickSync ? "NV12" : "I420", nullptr);
	g_object_set(elements[2], "caps", encoderCaps, nullptr);
	gst_caps_unref(encoderCaps);
	if (quickSync)
		g_object_set(elements[3], "bitrate", guint(4000), "gop-size", guint(60),
			"b-frames", guint(0), "rate-control", 1, "low-latency", TRUE,
			"target-usage", guint(7), nullptr);
	else
		g_object_set(elements[3], "bitrate", guint(4000000), "gop-size", guint(60),
			"complexity", 0, "usage-type", 1, "rate-control", 1, nullptr);
	g_object_set(elements[4], "config-interval", -1, nullptr);
	g_object_set(elements[5], "alignment", 7, nullptr);
	if (sendToSrt)
		g_object_set(elements.back(), "uri", uri, nullptr);
	else
		g_object_set(elements.back(), "sync", FALSE, nullptr);
	std::atomic_uint64_t encodedFrames{0};
	GstPad* encodedPad = gst_element_get_static_pad(elements[3], "src");
	const gulong probe = gst_pad_add_probe(encodedPad, GST_PAD_PROBE_TYPE_BUFFER,
		[](GstPad*, GstPadProbeInfo*, gpointer data) -> GstPadProbeReturn {
			static_cast<std::atomic_uint64_t*>(data)->fetch_add(1, std::memory_order_relaxed);
			return GST_PAD_PROBE_OK;
		}, &encodedFrames, nullptr);
	if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
		return 5;

	std::vector<uint8_t> pixels(size_t(width) * height * 4);
	const auto start = std::chrono::steady_clock::now();
	for (int frame = 0; frame < 60; ++frame)
	{
		for (int y = 0; y < height; ++y)
			for (int x = 0; x < width; ++x)
			{
				const size_t i = (size_t(y) * width + x) * 4;
				pixels[i] = uint8_t((x + frame * 4) & 255);
				pixels[i + 1] = uint8_t(y & 255);
				pixels[i + 2] = uint8_t(frame * 4);
				pixels[i + 3] = 255;
			}
		GstBuffer* buffer = gst_buffer_new_allocate(nullptr, pixels.size(), nullptr);
		if (!buffer)
			return 6;
		gst_buffer_fill(buffer, 0, pixels.data(), pixels.size());
		GST_BUFFER_PTS(buffer) = uint64_t(frame) * GST_SECOND / 30;
		GST_BUFFER_DURATION(buffer) = GST_SECOND / 30;
		if (gst_app_src_push_buffer(GST_APP_SRC(elements[0]), buffer) != GST_FLOW_OK)
			return 7;
		std::this_thread::sleep_until(start + std::chrono::nanoseconds(int64_t(frame + 1) * 1000000000 / 30));
	}
	gst_app_src_end_of_stream(GST_APP_SRC(elements[0]));
	GstBus* bus = gst_element_get_bus(pipeline);
	GstMessage* message = gst_bus_timed_pop_filtered(bus, 15 * GST_SECOND,
		GstMessageType(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
	const bool success = message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS &&
		encodedFrames.load(std::memory_order_relaxed) >= 50;
	if (message)
		gst_message_unref(message);
	gst_object_unref(bus);
	gst_element_set_state(pipeline, GST_STATE_NULL);
	gst_pad_remove_probe(encodedPad, probe);
	gst_object_unref(encodedPad);
	gst_object_unref(pipeline);
	std::cout << (success ? "Synthetic H.264 MPEG-TS pipeline passed: " : "Synthetic pipeline failed: ")
		<< encodedFrames.load(std::memory_order_relaxed) << " encoded frames\n";
	return success ? 0 : 8;
}
