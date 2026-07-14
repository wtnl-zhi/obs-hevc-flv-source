/*
 * OBS HEVC-FLV Source
 *
 * A Windows OBS input source for FLV streams whose video tag uses codec id 12
 * (HEVC). OBS's normal media source currently does not identify this variant.
 */

#include <obs-module.h>
#include <util/platform.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#if defined(_WIN32)
#include <windows.h>
#include <winhttp.h>
#elif defined(__APPLE__)
#include <CFNetwork/CFNetwork.h>
#include <CoreFoundation/CoreFoundation.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-hevc-flv-source", "en-US")

namespace {

constexpr uint8_t kFlvTagAudio = 8;
constexpr uint8_t kFlvTagVideo = 9;
constexpr uint8_t kFlvAudioAac = 10;
constexpr uint8_t kFlvVideoHevc = 12;

uint32_t read_be24(const uint8_t *data)
{
	return (uint32_t(data[0]) << 16U) | (uint32_t(data[1]) << 8U) | uint32_t(data[2]);
}

uint32_t read_be32(const uint8_t *data)
{
	return (uint32_t(data[0]) << 24U) | (uint32_t(data[1]) << 16U) |
	       (uint32_t(data[2]) << 8U) | uint32_t(data[3]);
}

int32_t read_signed_be24(const uint8_t *data)
{
	uint32_t value = read_be24(data);
	return (value & 0x800000U) ? int32_t(value | 0xFF000000U) : int32_t(value);
}

int64_t milliseconds_to_ns(int64_t value)
{
	return value * 1000000LL;
}

#if defined(_WIN32)
std::wstring utf8_to_wide(const std::string &value)
{
	if (value.empty())
		return {};
	int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), int(value.size()), nullptr, 0);
	if (size == 0)
		return {};
	std::wstring result(size, L'\0');
	MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), int(value.size()), result.data(), size);
	return result;
}
#endif

std::string ffmpeg_error(int error)
{
	std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
	av_strerror(error, buffer.data(), buffer.size());
	return buffer.data();
}

class HevcFlvSource {
public:
	explicit HevcFlvSource(obs_source_t *source) : source_(source) {}
	~HevcFlvSource()
	{
		stop();
		free_decoders();
	}

	void update(obs_data_t *settings)
	{
		const char *configured_url = obs_data_get_string(settings, "url");
		const int configured_reconnect = int(obs_data_get_int(settings, "reconnect_delay"));
		const std::string next_url = configured_url ? configured_url : "";

		{
			std::lock_guard lock(settings_mutex_);
			reconnect_delay_ms_ = std::clamp(configured_reconnect, 250, 30000);
			if (next_url == url_ && worker_.joinable())
				return;
		}

		stop();
		if (next_url.empty())
			return;

		std::lock_guard lock(settings_mutex_);
		url_ = next_url;
		stop_requested_ = false;
		worker_ = std::thread(&HevcFlvSource::run, this, url_);
	}

	void stop()
	{
		stop_requested_ = true;
		std::thread worker;
		{
			std::lock_guard lock(settings_mutex_);
			worker.swap(worker_);
		}
		if (worker.joinable())
			worker.join();
	}

private:
	obs_source_t *source_ = nullptr;
	std::mutex settings_mutex_;
	std::thread worker_;
	std::atomic<bool> stop_requested_{false};
	std::string url_;
	int reconnect_delay_ms_ = 1500;

	AVCodecContext *video_decoder_ = nullptr;
	AVCodecContext *audio_decoder_ = nullptr;
	SwsContext *scaler_ = nullptr;
	SwrContext *resampler_ = nullptr;
	std::vector<uint8_t> video_buffer_;
	std::array<std::vector<float>, 2> audio_buffer_;
	int video_width_ = 0;
	int video_height_ = 0;

	bool wait_for_reconnect()
	{
		int delay = 1500;
		{
			std::lock_guard lock(settings_mutex_);
			delay = reconnect_delay_ms_;
		}
		for (int elapsed = 0; elapsed < delay && !stop_requested_; elapsed += 50)
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		return !stop_requested_;
	}

	void free_decoders()
	{
		avcodec_free_context(&video_decoder_);
		avcodec_free_context(&audio_decoder_);
		sws_freeContext(scaler_);
		scaler_ = nullptr;
		swr_free(&resampler_);
		video_buffer_.clear();
		audio_buffer_[0].clear();
		audio_buffer_[1].clear();
		video_width_ = 0;
		video_height_ = 0;
	}

	bool open_decoder(AVCodecContext *&decoder, AVCodecID codec_id, const uint8_t *config, size_t config_size)
	{
		avcodec_free_context(&decoder);
		const AVCodec *codec = avcodec_find_decoder(codec_id);
		if (!codec) {
			obs_log(LOG_ERROR, "[HEVC FLV] Decoder is unavailable");
			return false;
		}

		decoder = avcodec_alloc_context3(codec);
		if (!decoder)
			return false;
		decoder->pkt_timebase = AVRational{1, 1000};
		decoder->time_base = AVRational{1, 1000};
		decoder->extradata = static_cast<uint8_t *>(av_mallocz(config_size + AV_INPUT_BUFFER_PADDING_SIZE));
		if (!decoder->extradata) {
			avcodec_free_context(&decoder);
			return false;
		}
		std::memcpy(decoder->extradata, config, config_size);
		decoder->extradata_size = int(config_size);

		const int result = avcodec_open2(decoder, codec, nullptr);
		if (result < 0) {
			obs_log(LOG_ERROR, "[HEVC FLV] Cannot open decoder: %s", ffmpeg_error(result).c_str());
			avcodec_free_context(&decoder);
			return false;
		}
		return true;
	}

	void initialize_video(const uint8_t *config, size_t config_size)
	{
		if (config_size < 23 || config[0] != 1) {
			obs_log(LOG_WARNING, "[HEVC FLV] Invalid HEVC configuration record");
			return;
		}
		free_video_decoder();
		if (open_decoder(video_decoder_, AV_CODEC_ID_HEVC, config, config_size))
			obs_log(LOG_INFO, "[HEVC FLV] HEVC video decoder initialized");
	}

	void free_video_decoder()
	{
		avcodec_free_context(&video_decoder_);
		sws_freeContext(scaler_);
		scaler_ = nullptr;
		video_buffer_.clear();
		video_width_ = 0;
		video_height_ = 0;
	}

	void initialize_audio(const uint8_t *config, size_t config_size)
	{
		if (config_size < 2) {
			obs_log(LOG_WARNING, "[HEVC FLV] Invalid AAC configuration record");
			return;
		}
		avcodec_free_context(&audio_decoder_);
		swr_free(&resampler_);
		if (open_decoder(audio_decoder_, AV_CODEC_ID_AAC, config, config_size))
			obs_log(LOG_INFO, "[HEVC FLV] AAC audio decoder initialized");
	}

	void output_video(AVFrame *frame)
	{
		if (frame->width <= 0 || frame->height <= 0)
			return;
		if (frame->width != video_width_ || frame->height != video_height_ || !scaler_) {
			scaler_ = sws_getCachedContext(scaler_, frame->width, frame->height, AVPixelFormat(frame->format), frame->width,
							      frame->height, AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr);
			if (!scaler_)
				return;
			video_width_ = frame->width;
			video_height_ = frame->height;
			video_buffer_.resize(size_t(video_width_) * size_t(video_height_) * 4U);
		}

		uint8_t *planes[] = {video_buffer_.data(), nullptr, nullptr, nullptr};
		int linesizes[] = {video_width_ * 4, 0, 0, 0};
		sws_scale(scaler_, frame->data, frame->linesize, 0, frame->height, planes, linesizes);

		obs_source_frame output{};
		output.data[0] = video_buffer_.data();
		output.linesize[0] = uint32_t(linesizes[0]);
		output.width = uint32_t(video_width_);
		output.height = uint32_t(video_height_);
		output.format = VIDEO_FORMAT_BGRA;
		const int64_t timestamp = frame->best_effort_timestamp == AV_NOPTS_VALUE ? frame->pts : frame->best_effort_timestamp;
		output.timestamp = timestamp == AV_NOPTS_VALUE ? os_gettime_ns() : milliseconds_to_ns(timestamp);
		obs_source_output_video(source_, &output);
	}

	void output_audio(AVFrame *frame)
	{
		if (!audio_decoder_ || frame->nb_samples <= 0)
			return;

		AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
		if (!resampler_) {
			const int result = swr_alloc_set_opts2(&resampler_, &stereo, AV_SAMPLE_FMT_FLTP, 48000,
								      &audio_decoder_->ch_layout, audio_decoder_->sample_fmt,
								      audio_decoder_->sample_rate, 0, nullptr);
			if (result < 0 || !resampler_ || swr_init(resampler_) < 0) {
				obs_log(LOG_ERROR, "[HEVC FLV] Cannot initialize audio resampler");
				swr_free(&resampler_);
				return;
			}
		}

		const int samples = int(av_rescale_rnd(swr_get_delay(resampler_, audio_decoder_->sample_rate) + frame->nb_samples,
									 48000, audio_decoder_->sample_rate, AV_ROUND_UP));
		audio_buffer_[0].resize(samples);
		audio_buffer_[1].resize(samples);
		uint8_t *planes[] = {reinterpret_cast<uint8_t *>(audio_buffer_[0].data()),
					     reinterpret_cast<uint8_t *>(audio_buffer_[1].data())};
		const int converted = swr_convert(resampler_, planes, samples, const_cast<const uint8_t **>(frame->extended_data),
							  frame->nb_samples);
		if (converted <= 0)
			return;

		obs_source_audio output{};
		output.data[0] = reinterpret_cast<uint8_t *>(audio_buffer_[0].data());
		output.data[1] = reinterpret_cast<uint8_t *>(audio_buffer_[1].data());
		output.speakers = SPEAKERS_STEREO;
		output.samples_per_sec = 48000;
		output.format = AUDIO_FORMAT_FLOAT_PLANAR;
		output.frames = uint32_t(converted);
		const int64_t timestamp = frame->best_effort_timestamp == AV_NOPTS_VALUE ? frame->pts : frame->best_effort_timestamp;
		output.timestamp = timestamp == AV_NOPTS_VALUE ? os_gettime_ns() : milliseconds_to_ns(timestamp);
		obs_source_output_audio(source_, &output);
	}

	void decode_video(const uint8_t *data, size_t size, int64_t dts, int64_t pts)
	{
		if (!video_decoder_ || size == 0)
			return;
		AVPacket packet{};
		packet.data = const_cast<uint8_t *>(data);
		packet.size = int(size);
		packet.dts = dts;
		packet.pts = pts;
		if (avcodec_send_packet(video_decoder_, &packet) < 0)
			return;
		AVFrame *frame = av_frame_alloc();
		if (!frame)
			return;
		while (avcodec_receive_frame(video_decoder_, frame) == 0) {
			output_video(frame);
			av_frame_unref(frame);
		}
		av_frame_free(&frame);
	}

	void decode_audio(const uint8_t *data, size_t size, int64_t timestamp)
	{
		if (!audio_decoder_ || size == 0)
			return;
		AVPacket packet{};
		packet.data = const_cast<uint8_t *>(data);
		packet.size = int(size);
		packet.dts = timestamp;
		packet.pts = timestamp;
		if (avcodec_send_packet(audio_decoder_, &packet) < 0)
			return;
		AVFrame *frame = av_frame_alloc();
		if (!frame)
			return;
		while (avcodec_receive_frame(audio_decoder_, frame) == 0) {
			output_audio(frame);
			av_frame_unref(frame);
		}
		av_frame_free(&frame);
	}

	void handle_tag(uint8_t type, const uint8_t *data, size_t size, uint32_t timestamp)
	{
		if (type == kFlvTagVideo) {
			if (size < 5 || (data[0] & 0x0FU) != kFlvVideoHevc)
				return;
			const uint8_t packet_type = data[1];
			if (packet_type == 0)
				initialize_video(data + 5, size - 5);
			else if (packet_type == 1)
				decode_video(data + 5, size - 5, timestamp, int64_t(timestamp) + read_signed_be24(data + 2));
			return;
		}

		if (type == kFlvTagAudio) {
			if (size < 2 || ((data[0] >> 4U) & 0x0FU) != kFlvAudioAac)
				return;
			if (data[1] == 0)
				initialize_audio(data + 2, size - 2);
			else if (data[1] == 1)
				decode_audio(data + 2, size - 2, timestamp);
		}
	}

	void consume_flv(std::vector<uint8_t> &buffer, size_t &offset, bool &header_parsed)
	{
		if (!header_parsed && buffer.size() >= 13) {
			if (std::memcmp(buffer.data(), "FLV", 3) != 0) {
				obs_log(LOG_ERROR, "[HEVC FLV] Upstream response is not FLV");
				stop_requested_ = true;
				return;
			}
			const uint32_t header_size = read_be32(buffer.data() + 5);
			if (buffer.size() < size_t(header_size) + 4)
				return;
			offset = size_t(header_size) + 4;
			header_parsed = true;
		}

		while (!stop_requested_ && buffer.size() - offset >= 11) {
			const uint8_t *tag = buffer.data() + offset;
			const size_t data_size = read_be24(tag + 1);
			if (buffer.size() - offset < 11 + data_size + 4)
				break;
			const uint32_t timestamp = read_be24(tag + 4) | (uint32_t(tag[7]) << 24U);
			handle_tag(tag[0], tag + 11, data_size, timestamp);
			offset += 11 + data_size + 4;
		}

		if (offset > 1024 * 1024) {
			buffer.erase(buffer.begin(), buffer.begin() + ptrdiff_t(offset));
			offset = 0;
		}
	}

	bool stream_once(const std::string &url)
	{
#if defined(_WIN32)
		const std::wstring wide_url = utf8_to_wide(url);
		if (wide_url.empty()) {
			obs_log(LOG_ERROR, "[HEVC FLV] URL must be valid UTF-8");
			return false;
		}

		URL_COMPONENTS components{};
		components.dwStructSize = sizeof(components);
		if (!WinHttpCrackUrl(wide_url.c_str(), 0, 0, &components)) {
			obs_log(LOG_ERROR, "[HEVC FLV] Invalid URL");
			return false;
		}
		const std::wstring host(components.lpszHostName, components.dwHostNameLength);
		std::wstring path(components.lpszUrlPath, components.dwUrlPathLength);
		if (components.dwExtraInfoLength > 0)
			path.append(components.lpszExtraInfo, components.dwExtraInfoLength);

		HINTERNET session = WinHttpOpen(L"OBS HEVC-FLV Source/0.1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
					       WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
		if (!session)
			return false;
		HINTERNET connection = WinHttpConnect(session, host.c_str(), components.nPort, 0);
		HINTERNET request = connection ? WinHttpOpenRequest(connection, L"GET", path.c_str(), nullptr,
							      WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
							      components.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
						     : nullptr;
		if (!request || !WinHttpSendRequest(request, L"User-Agent: OBS HEVC-FLV Source\r\n", -1L,
							       WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
		    !WinHttpReceiveResponse(request, nullptr)) {
			if (request)
				WinHttpCloseHandle(request);
			if (connection)
				WinHttpCloseHandle(connection);
			WinHttpCloseHandle(session);
			return false;
		}

		DWORD status = 0;
		DWORD status_size = sizeof(status);
		WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status,
						    &status_size, nullptr);
		if (status != 200) {
			obs_log(LOG_WARNING, "[HEVC FLV] Upstream returned HTTP %lu", status);
			WinHttpCloseHandle(request);
			WinHttpCloseHandle(connection);
			WinHttpCloseHandle(session);
			return false;
		}

		std::vector<uint8_t> flv_data;
		flv_data.reserve(2 * 1024 * 1024);
		size_t offset = 0;
		bool header_parsed = false;
		std::array<uint8_t, 64 * 1024> chunk{};
		while (!stop_requested_) {
			DWORD received = 0;
			if (!WinHttpReadData(request, chunk.data(), DWORD(chunk.size()), &received) || received == 0)
				break;
			flv_data.insert(flv_data.end(), chunk.data(), chunk.data() + received);
			consume_flv(flv_data, offset, header_parsed);
		}

		WinHttpCloseHandle(request);
		WinHttpCloseHandle(connection);
		WinHttpCloseHandle(session);
		return !stop_requested_;
#elif defined(__APPLE__)
		CFURLRef stream_url = CFURLCreateWithBytes(kCFAllocatorDefault, reinterpret_cast<const UInt8 *>(url.data()),
								   CFIndex(url.size()), kCFStringEncodingUTF8, nullptr);
		if (!stream_url) {
			obs_log(LOG_ERROR, "[HEVC FLV] URL must be valid UTF-8");
			return false;
		}
		CFHTTPMessageRef request = CFHTTPMessageCreateRequest(kCFAllocatorDefault, CFSTR("GET"), stream_url,
									      kCFHTTPVersion1_1);
		CFRelease(stream_url);
		if (!request)
			return false;
		CFHTTPMessageSetHeaderFieldValue(request, CFSTR("User-Agent"), CFSTR("OBS HEVC-FLV Source/0.1"));
		CFReadStreamRef stream = CFReadStreamCreateForHTTPRequest(kCFAllocatorDefault, request);
		CFRelease(request);
		if (!stream)
			return false;
		CFReadStreamSetProperty(stream, kCFStreamPropertyHTTPShouldAutoredirect, kCFBooleanTrue);
		if (!CFReadStreamOpen(stream)) {
			CFRelease(stream);
			return false;
		}

		std::vector<uint8_t> flv_data;
		flv_data.reserve(2 * 1024 * 1024);
		size_t offset = 0;
		bool header_parsed = false;
		bool checked_status = false;
		std::array<uint8_t, 64 * 1024> chunk{};
		while (!stop_requested_) {
			if (!checked_status) {
				CFTypeRef response_value = CFReadStreamCopyProperty(stream, kCFStreamPropertyHTTPResponseHeader);
				CFHTTPMessageRef response = static_cast<CFHTTPMessageRef>(const_cast<void *>(response_value));
				if (response) {
					const CFIndex status = CFHTTPMessageGetResponseStatusCode(response);
					CFRelease(response);
					if (status != 200) {
						obs_log(LOG_WARNING, "[HEVC FLV] Upstream returned HTTP %ld", long(status));
						break;
					}
					checked_status = true;
				}
			}

			if (CFReadStreamHasBytesAvailable(stream)) {
				const CFIndex received = CFReadStreamRead(stream, chunk.data(), CFIndex(chunk.size()));
				if (received <= 0)
					break;
				flv_data.insert(flv_data.end(), chunk.data(), chunk.data() + received);
				consume_flv(flv_data, offset, header_parsed);
				continue;
			}

			const CFStreamStatus status = CFReadStreamGetStatus(stream);
			if (status == kCFStreamStatusAtEnd || status == kCFStreamStatusError || status == kCFStreamStatusClosed)
				break;
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}

		CFReadStreamClose(stream);
		CFRelease(stream);
		return !stop_requested_;
#endif
	}

	void run(const std::string url)
	{
		obs_log(LOG_INFO, "[HEVC FLV] Starting source");
		while (!stop_requested_) {
			free_decoders();
			stream_once(url);
			if (!stop_requested_)
				obs_log(LOG_INFO, "[HEVC FLV] Connection closed; reconnecting");
			if (!wait_for_reconnect())
				break;
		}
		obs_log(LOG_INFO, "[HEVC FLV] Source stopped");
	}
};

const char *source_name(void *)
{
	return obs_module_text("HevcFlvSource");
}

void *source_create(obs_data_t *settings, obs_source_t *source)
{
	auto *instance = new HevcFlvSource(source);
	instance->update(settings);
	return instance;
}

void source_destroy(void *data)
{
	delete static_cast<HevcFlvSource *>(data);
}

void source_update(void *data, obs_data_t *settings)
{
	static_cast<HevcFlvSource *>(data)->update(settings);
}

void source_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "url", "");
	obs_data_set_default_int(settings, "reconnect_delay", 1500);
}

obs_properties_t *source_properties(void *)
{
	obs_properties_t *properties = obs_properties_create();
	obs_properties_add_text(properties, "url", obs_module_text("Url"), OBS_TEXT_DEFAULT);
	obs_properties_add_int(properties, "reconnect_delay", obs_module_text("ReconnectDelay"), 250, 30000, 250);
	obs_properties_add_text(properties, "status", obs_module_text("Status"), OBS_TEXT_INFO);
	return properties;
}

obs_source_info source_info = {
	.id = "hevc_flv_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO,
	.get_name = source_name,
	.create = source_create,
	.destroy = source_destroy,
	.update = source_update,
	.get_defaults = source_defaults,
	.get_properties = source_properties,
};

} // namespace

bool obs_module_load()
{
	obs_register_source(&source_info);
	obs_log(LOG_INFO, "[HEVC FLV] Windows HEVC-FLV source loaded");
	return true;
}

void obs_module_unload()
{
	obs_log(LOG_INFO, "[HEVC FLV] Windows HEVC-FLV source unloaded");
}
