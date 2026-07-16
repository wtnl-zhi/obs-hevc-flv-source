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
#include <curl/curl.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdio>
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
constexpr uint8_t kFlvVideoAvc = 7;
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

bool is_http_url(const std::string &value)
{
	return value.starts_with("http://") || value.starts_with("https://");
}

std::string unescape_json_string(const std::string &value)
{
	std::string result;
	result.reserve(value.size());
	for (size_t index = 0; index < value.size(); ++index) {
		if (value[index] != '\\' || index + 1 >= value.size()) {
			result += value[index];
			continue;
		}

		const char escaped = value[++index];
		switch (escaped) {
		case '"':
		case '\\':
		case '/':
			result += escaped;
			break;
		case 'b':
			result += '\b';
			break;
		case 'f':
			result += '\f';
			break;
		case 'n':
			result += '\n';
			break;
		case 'r':
			result += '\r';
			break;
		case 't':
			result += '\t';
			break;
		case 'u': {
			if (index + 4 >= value.size())
				return {};
			unsigned codepoint = 0;
			for (size_t digit = 1; digit <= 4; ++digit) {
				const char value_digit = value[index + digit];
				if (!std::isxdigit(static_cast<unsigned char>(value_digit)))
					return {};
				codepoint *= 16;
				codepoint += std::isdigit(static_cast<unsigned char>(value_digit))
						     ? unsigned(value_digit - '0')
						     : unsigned(std::tolower(static_cast<unsigned char>(value_digit)) - 'a' + 10);
			}
			if (codepoint <= 0x7f)
				result += char(codepoint);
			else
				return {};
			index += 4;
			break;
		}
		default:
			return {};
		}
	}
	return result;
}

std::string find_json_string(const std::string &text, const char *key, size_t start = 0)
{
	const std::string quoted_key = std::string("\"") + key + "\"";
	const size_t key_position = text.find(quoted_key, start);
	if (key_position == std::string::npos)
		return {};
	const size_t colon = text.find(':', key_position + quoted_key.size());
	if (colon == std::string::npos)
		return {};
	const size_t value_start = text.find_first_not_of(" \t\r\n", colon + 1);
	if (value_start == std::string::npos || text[value_start] != '"')
		return {};

	std::string encoded;
	for (size_t index = value_start + 1; index < text.size(); ++index) {
		if (text[index] == '"' && (index == value_start + 1 || text[index - 1] != '\\'))
			return unescape_json_string(encoded);
		encoded += text[index];
	}
	return {};
}

std::string find_escaped_json_string(const std::string &text, const char *key, size_t start = 0)
{
	const std::string escaped_key = std::string("\\\"") + key + "\\\"";
	const size_t key_position = text.find(escaped_key, start);
	if (key_position == std::string::npos)
		return {};
	const size_t colon = text.find(':', key_position + escaped_key.size());
	if (colon == std::string::npos)
		return {};
	const size_t value_start = text.find_first_not_of(" \t\r\n", colon + 1);
	if (value_start == std::string::npos || text.compare(value_start, 2, "\\\"") != 0)
		return {};

	std::string encoded;
	for (size_t index = value_start + 2; index + 1 < text.size(); ++index) {
		if (text[index] == '\\' && text[index + 1] == '\"')
			return unescape_json_string(encoded);
		encoded += text[index];
	}
	return {};
}

std::string first_http_url(const std::string &text, size_t start = 0)
{
	const size_t http = text.find("http", start);
	if (http == std::string::npos)
		return {};
	const size_t end = text.find_first_of("\"'\\ \t\r\n<>", http);
	const std::string candidate = text.substr(http, end == std::string::npos ? std::string::npos : end - http);
	return is_http_url(candidate) ? candidate : std::string{};
}

std::string find_douyin_room_id(const std::string &text)
{
	constexpr std::array<const char *, 3> markers = {"live.douyin.com/", "live.douyin.com%2F",
											 "live.douyin.com\\u002F"};
	for (const char *marker : markers) {
		const size_t marker_position = text.find(marker);
		if (marker_position == std::string::npos)
			continue;
		const size_t first_digit = marker_position + std::strlen(marker);
		size_t end = first_digit;
		while (end < text.size() && std::isdigit(static_cast<unsigned char>(text[end])))
			++end;
		if (end > first_digit)
			return text.substr(first_digit, end - first_digit);
	}
	return {};
}

#if defined(_WIN32)
bool fetch_text(const std::string &url, std::string &body, std::string &final_url)
{
	const std::wstring wide_url = utf8_to_wide(url);
	if (wide_url.empty())
		return false;
	URL_COMPONENTS components{};
	components.dwStructSize = sizeof(components);
	components.dwSchemeLength = DWORD(-1);
	components.dwHostNameLength = DWORD(-1);
	components.dwUrlPathLength = DWORD(-1);
	components.dwExtraInfoLength = DWORD(-1);
	if (!WinHttpCrackUrl(wide_url.c_str(), 0, 0, &components))
		return false;

	const std::wstring host(components.lpszHostName, components.dwHostNameLength);
	std::wstring path(components.lpszUrlPath, components.dwUrlPathLength);
	if (components.dwExtraInfoLength > 0)
		path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
	HINTERNET session = WinHttpOpen(L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 Chrome/131 Safari/537.36",
					       WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	if (!session)
		return false;
	WinHttpSetTimeouts(session, 5000, 5000, 5000, 8000);
	HINTERNET connection = WinHttpConnect(session, host.c_str(), components.nPort, 0);
	HINTERNET request = connection ? WinHttpOpenRequest(connection, L"GET", path.c_str(), nullptr,
							      WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
							      components.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
						 : nullptr;
	const wchar_t *headers = L"Accept: application/json, text/plain, */*\r\nAccept-Encoding: identity\r\nReferer: https://live.douyin.com/\r\n";
	const bool received = request && WinHttpSendRequest(request, headers, DWORD(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
			      WinHttpReceiveResponse(request, nullptr);
	DWORD status = 0;
	DWORD status_size = sizeof(status);
	if (received)
		WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &status_size,
					    nullptr);
	if (!received || status != 200) {
		if (request)
			WinHttpCloseHandle(request);
		if (connection)
			WinHttpCloseHandle(connection);
		WinHttpCloseHandle(session);
		return false;
	}

	DWORD final_url_size = 0;
	WinHttpQueryOption(request, WINHTTP_OPTION_URL, nullptr, &final_url_size);
	if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && final_url_size > sizeof(wchar_t)) {
		std::wstring final_wide(final_url_size / sizeof(wchar_t), L'\0');
		if (WinHttpQueryOption(request, WINHTTP_OPTION_URL, final_wide.data(), &final_url_size)) {
			const int utf8_size = WideCharToMultiByte(CP_UTF8, 0, final_wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
			if (utf8_size > 1) {
				final_url.resize(size_t(utf8_size));
				WideCharToMultiByte(CP_UTF8, 0, final_wide.c_str(), -1, final_url.data(), utf8_size, nullptr, nullptr);
				final_url.resize(size_t(utf8_size - 1));
			}
		}
	}

	std::array<char, 16 * 1024> chunk{};
	while (body.size() < 2 * 1024 * 1024) {
		DWORD received_bytes = 0;
		if (!WinHttpReadData(request, chunk.data(), DWORD(chunk.size()), &received_bytes) || received_bytes == 0)
			break;
		body.append(chunk.data(), received_bytes);
	}
	WinHttpCloseHandle(request);
	WinHttpCloseHandle(connection);
	WinHttpCloseHandle(session);
	return !body.empty();
}
#elif defined(__APPLE__)
size_t curl_write_body(char *data, size_t size, size_t count, void *user_data)
{
	const size_t received = size * count;
	auto *body = static_cast<std::string *>(user_data);
	if (received == 0 || body->size() + received > 2 * 1024 * 1024)
		return 0;
	body->append(data, received);
	return received;
}

bool fetch_text(const std::string &url, std::string &body, std::string &final_url)
{
	static std::once_flag curl_initialized;
	std::call_once(curl_initialized, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
	CURL *curl = curl_easy_init();
	if (!curl)
		return false;
	struct curl_slist *headers = nullptr;
	const bool is_douyin_live_page = url.starts_with("https://live.douyin.com/") &&
					 url.find("/webcast/") == std::string::npos;
	if (!is_douyin_live_page) {
		headers = curl_slist_append(headers, "Accept: application/json, text/plain, */*");
		headers = curl_slist_append(headers, "Accept-Encoding: identity");
		headers = curl_slist_append(headers, "Referer: https://live.douyin.com/");
	}
	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	if (headers)
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_USERAGENT,
			 "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 Chrome/131 Safari/537.36");
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
	curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_body);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
	const CURLcode result = curl_easy_perform(curl);
	long status = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	char *effective_url = nullptr;
	curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effective_url);
	if (effective_url)
		final_url = effective_url;
	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);
	if (result != CURLE_OK || status != 200) {
		blog(LOG_WARNING, "[HEVC FLV] HTTP request failed (%s, status %ld)", curl_easy_strerror(result), status);
		return false;
	}
	return !body.empty();
}

bool fetch_douyin_live_page(const std::string &room_id, std::string &body)
{
	if (room_id.empty() || !std::all_of(room_id.begin(), room_id.end(), [](unsigned char c) { return std::isdigit(c); }))
		return false;
	const std::string command =
		"/usr/bin/curl --silent --show-error --location --connect-timeout 5 --max-time 10 "
		"-A 'Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 Chrome/131 Safari/537.36' "
		"'https://live.douyin.com/" +
		room_id + "'";
	FILE *pipe = popen(command.c_str(), "r");
	if (!pipe)
		return false;
	std::array<char, 16 * 1024> chunk{};
	while (body.size() < 2 * 1024 * 1024) {
		const size_t received = fread(chunk.data(), 1, chunk.size(), pipe);
		if (received == 0)
			break;
		body.append(chunk.data(), received);
	}
	const int status = pclose(pipe);
	return status == 0 && !body.empty();
}

bool fetch_douyin_chrome_page_state(const std::string &room_id, std::string &body)
{
	if (room_id.empty() || !std::all_of(room_id.begin(), room_id.end(), [](unsigned char c) { return std::isdigit(c); }))
		return false;
	// This only asks Chrome for the public script state of the matching open
	// live page. It never requests cookies, storage, passwords, or account data.
	const std::string command =
		"/usr/bin/osascript -e 'with timeout of 3 seconds' -e 'tell application \"Google Chrome\"' "
		"-e 'repeat with w in windows' -e 'repeat with t in tabs of w' "
		"-e 'if (URL of t) contains \"live.douyin.com/" +
		room_id +
		"\" then' "
		"-e 'return execute t javascript \"[...document.scripts].map(s => s.textContent).find(s => "
		"s.includes(\\\"flv_pull_url\\\")) || \\\"\\\"\"' "
		"-e 'end if' -e 'end repeat' -e 'end repeat' -e 'end tell' -e 'end timeout'";
	FILE *pipe = popen(command.c_str(), "r");
	if (!pipe)
		return false;
	std::array<char, 16 * 1024> chunk{};
	while (body.size() < 2 * 1024 * 1024) {
		const size_t received = fread(chunk.data(), 1, chunk.size(), pipe);
		if (received == 0)
			break;
		body.append(chunk.data(), received);
	}
	const int status = pclose(pipe);
	return status == 0 && !body.empty();
}
#endif

std::string find_douyin_flv_url(const std::string &response, const std::string &room_id)
{
	size_t stream_urls = response.find("\"flv_pull_url\"");
	const bool escaped_json = stream_urls == std::string::npos;
	if (escaped_json)
		stream_urls = response.find("\\\"flv_pull_url\\\"");
	if (stream_urls == std::string::npos) {
		blog(LOG_INFO, "[HEVC FLV] Douyin page did not contain FLV state (bytes=%zu)", response.size());
		return {};
	}
	for (const char *quality : {"FULL_HD1", "HD1", "SD1", "SD2", "LD"}) {
		const std::string url = escaped_json ? find_escaped_json_string(response, quality, stream_urls)
							     : find_json_string(response, quality, stream_urls);
		if (is_http_url(url)) {
			blog(LOG_INFO, "[HEVC FLV] Resolved Douyin room %s (%s)", room_id.c_str(), quality);
			return url;
		}
	}
	const std::string url = escaped_json ? find_escaped_json_string(response, "flv_pull_url", stream_urls)
						     : find_json_string(response, "flv_pull_url", stream_urls);
	if (is_http_url(url)) {
		blog(LOG_INFO, "[HEVC FLV] Resolved Douyin room %s", room_id.c_str());
		return url;
	}
	return {};
}

std::string resolve_douyin_stream_url(const std::string &input)
{
	if (input.find("douyin.com") == std::string::npos)
		return input;

	std::string room_id = find_douyin_room_id(input);
	if (room_id.empty()) {
		const std::string shared_url = first_http_url(input);
		std::string landing_page;
		std::string final_url;
		if (shared_url.empty() || !fetch_text(shared_url, landing_page, final_url)) {
			blog(LOG_WARNING, "[HEVC FLV] Could not open the Douyin shared link");
			return {};
		}
		room_id = find_douyin_room_id(final_url);
		if (room_id.empty())
			room_id = find_douyin_room_id(landing_page);
	}
	if (room_id.empty()) {
		blog(LOG_WARNING, "[HEVC FLV] This Douyin link did not resolve to a live room");
		return {};
	}

	std::string response;
	std::string ignored_final_url;
#if defined(__APPLE__)
	const bool fetched_live_page = fetch_douyin_live_page(room_id, response);
#else
	const std::string live_page_url = "https://live.douyin.com/" + room_id;
	const bool fetched_live_page = fetch_text(live_page_url, response, ignored_final_url);
#endif
	if (!fetched_live_page) {
		blog(LOG_WARNING, "[HEVC FLV] Could not open the Douyin live-room page");
		return {};
	}
	const std::string stream_url = find_douyin_flv_url(response, room_id);
	if (!stream_url.empty())
		return stream_url;
#if defined(__APPLE__)
	std::string chrome_page_state;
	if (fetch_douyin_chrome_page_state(room_id, chrome_page_state)) {
		const std::string chrome_stream_url = find_douyin_flv_url(chrome_page_state, room_id);
		if (!chrome_stream_url.empty()) {
			blog(LOG_INFO, "[HEVC FLV] Resolved Douyin room %s from the open Chrome live page", room_id.c_str());
			return chrome_stream_url;
		}
	}
#endif
	if (stream_url.empty())
		blog(LOG_WARNING, "[HEVC FLV] The Douyin room is offline or did not provide an FLV stream");
	return {};
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
			blog(LOG_ERROR, "[HEVC FLV] Decoder is unavailable");
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
			blog(LOG_ERROR, "[HEVC FLV] Cannot open decoder: %s", ffmpeg_error(result).c_str());
			avcodec_free_context(&decoder);
			return false;
		}
		return true;
	}

	void initialize_video(AVCodecID codec_id, const char *codec_name, size_t minimum_config_size, const uint8_t *config,
			      size_t config_size)
	{
		if (config_size < minimum_config_size || config[0] != 1) {
			blog(LOG_WARNING, "[HEVC FLV] Invalid %s configuration record", codec_name);
			return;
		}
		free_video_decoder();
		if (open_decoder(video_decoder_, codec_id, config, config_size))
			blog(LOG_INFO, "[HEVC FLV] %s video decoder initialized", codec_name);
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
			blog(LOG_WARNING, "[HEVC FLV] Invalid AAC configuration record");
			return;
		}
		avcodec_free_context(&audio_decoder_);
		swr_free(&resampler_);
		if (open_decoder(audio_decoder_, AV_CODEC_ID_AAC, config, config_size))
			blog(LOG_INFO, "[HEVC FLV] AAC audio decoder initialized");
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
				blog(LOG_ERROR, "[HEVC FLV] Cannot initialize audio resampler");
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
			if (size < 5)
				return;
			const uint8_t codec = data[0] & 0x0FU;
			if (codec != kFlvVideoHevc && codec != kFlvVideoAvc)
				return;
			const uint8_t packet_type = data[1];
			if (packet_type == 0)
				initialize_video(codec == kFlvVideoHevc ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264,
						 codec == kFlvVideoHevc ? "HEVC" : "H.264",
						 codec == kFlvVideoHevc ? 23 : 7, data + 5, size - 5);
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
				blog(LOG_ERROR, "[HEVC FLV] Upstream response is not FLV");
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
			blog(LOG_ERROR, "[HEVC FLV] URL must be valid UTF-8");
			return false;
		}

		URL_COMPONENTS components{};
		components.dwStructSize = sizeof(components);
		// Ask WinHTTP to return pointers into wide_url for each parsed component.
		// Leaving these lengths at zero makes the host/path empty, so WinHttpConnect
		// fails before the HTTP request is sent.
		components.dwSchemeLength = DWORD(-1);
		components.dwHostNameLength = DWORD(-1);
		components.dwUrlPathLength = DWORD(-1);
		components.dwExtraInfoLength = DWORD(-1);
		if (!WinHttpCrackUrl(wide_url.c_str(), 0, 0, &components)) {
			blog(LOG_ERROR, "[HEVC FLV] Invalid URL (WinHttpCrackUrl error %lu)", GetLastError());
			return false;
		}
		const std::wstring host(components.lpszHostName, components.dwHostNameLength);
		std::wstring path(components.lpszUrlPath, components.dwUrlPathLength);
		if (components.dwExtraInfoLength > 0)
			path.append(components.lpszExtraInfo, components.dwExtraInfoLength);

		HINTERNET session = WinHttpOpen(L"OBS HEVC-FLV Source/0.1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
					       WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
		if (!session) {
			blog(LOG_ERROR, "[HEVC FLV] WinHttpOpen failed (error %lu)", GetLastError());
			return false;
		}
		HINTERNET connection = WinHttpConnect(session, host.c_str(), components.nPort, 0);
		HINTERNET request = connection ? WinHttpOpenRequest(connection, L"GET", path.c_str(), nullptr,
							      WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
							      components.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
						     : nullptr;
		if (!request || !WinHttpSendRequest(request, L"User-Agent: OBS HEVC-FLV Source\r\n", -1L,
							       WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
		    !WinHttpReceiveResponse(request, nullptr)) {
			const DWORD error = GetLastError();
			blog(LOG_ERROR, "[HEVC FLV] WinHTTP request failed (error %lu)", error);
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
			blog(LOG_WARNING, "[HEVC FLV] Upstream returned HTTP %lu", status);
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
			if (!WinHttpReadData(request, chunk.data(), DWORD(chunk.size()), &received)) {
				blog(LOG_WARNING, "[HEVC FLV] WinHttpReadData failed (error %lu)", GetLastError());
				break;
			}
			if (received == 0)
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
			blog(LOG_ERROR, "[HEVC FLV] URL must be valid UTF-8");
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
						blog(LOG_WARNING, "[HEVC FLV] Upstream returned HTTP %ld", long(status));
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
		blog(LOG_INFO, "[HEVC FLV] Starting source");
		while (!stop_requested_) {
			free_decoders();
			const std::string stream_url = resolve_douyin_stream_url(url);
			if (!stream_url.empty())
				stream_once(stream_url);
			if (!stop_requested_)
				blog(LOG_INFO, "[HEVC FLV] Connection closed; reconnecting");
			if (!wait_for_reconnect())
				break;
		}
		blog(LOG_INFO, "[HEVC FLV] Source stopped");
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
	.get_defaults = source_defaults,
	.get_properties = source_properties,
	.update = source_update,
};

} // namespace

bool obs_module_load()
{
	obs_register_source(&source_info);
	blog(LOG_INFO, "[HEVC FLV] HEVC-FLV source loaded");
	return true;
}

void obs_module_unload()
{
	blog(LOG_INFO, "[HEVC FLV] HEVC-FLV source unloaded");
}
