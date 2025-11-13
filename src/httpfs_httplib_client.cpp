#include "httpfs_client.hpp"
#include "http_state.hpp"
#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.hpp"

namespace duckdb {

class HTTPFSClient : public HTTPClient {
public:
	HTTPFSClient(HTTPFSParams &http_params, const string &proto_host_port) {
		client = make_uniq<duckdb_httplib_openssl::Client>(proto_host_port);
		Initialize(http_params);
	}
	void Initialize(HTTPParams &http_p) override {
		HTTPFSParams &http_params = (HTTPFSParams&)http_p;
		client->set_follow_location(http_params.follow_location);
		client->set_keep_alive(http_params.keep_alive);
		if (!http_params.ca_cert_file.empty()) {
			client->set_ca_cert_path(http_params.ca_cert_file.c_str());
		}
		client->enable_server_certificate_verification(http_params.enable_server_cert_verification);
		client->set_write_timeout(http_params.timeout, http_params.timeout_usec);
		client->set_read_timeout(http_params.timeout, http_params.timeout_usec);
		client->set_connection_timeout(http_params.timeout, http_params.timeout_usec);
		client->set_decompress(false);
		if (!http_params.bearer_token.empty()) {
			client->set_bearer_token_auth(http_params.bearer_token.c_str());
		}

		if (!http_params.http_proxy.empty()) {
			client->set_proxy(http_params.http_proxy, http_params.http_proxy_port);

			if (!http_params.http_proxy_username.empty()) {
				client->set_proxy_basic_auth(http_params.http_proxy_username, http_params.http_proxy_password);
			}
		}
		state = http_params.state;
	}

	unique_ptr<HTTPResponse> Get(GetRequestInfo &info) override {
		if (state) {
			state->get_count++;
		}
		auto headers = TransformHeaders(info.headers, info.params);
		if (!info.response_handler && !info.content_handler) {
			return TransformResult(client->Get(info.path, headers));
		} else {
			return TransformResult(client->Get(
			    info.path.c_str(), headers,
			    [&](const duckdb_httplib_openssl::Response &response) {
				    auto http_response = TransformResponse(response);
				    return info.response_handler(*http_response);
			    },
			    [&](const char *data, size_t data_length) {
				    if (state) {
					    state->total_bytes_received += data_length;
				    }
				    return info.content_handler(const_data_ptr_cast(data), data_length);
			    }));
		}
	}
	unique_ptr<HTTPResponse> Put(PutRequestInfo &info) override {
		if (state) {
			state->put_count++;
			state->total_bytes_sent += info.buffer_in_len;
		}
		auto headers = TransformHeaders(info.headers, info.params);
		return TransformResult(client->Put(info.path, headers, const_char_ptr_cast(info.buffer_in), info.buffer_in_len,
		                                   info.content_type));
	}

	unique_ptr<HTTPResponse> Head(HeadRequestInfo &info) override {
		if (state) {
			state->head_count++;
		}
		auto headers = TransformHeaders(info.headers, info.params);
		return TransformResult(client->Head(info.path, headers));
	}

	unique_ptr<HTTPResponse> Delete(DeleteRequestInfo &info) override {
		if (state) {
			state->delete_count++;
		}
		auto headers = TransformHeaders(info.headers, info.params);
		return TransformResult(client->Delete(info.path, headers));
	}

	unique_ptr<HTTPResponse> Post(PostRequestInfo &info) override {
		if (state) {
			state->post_count++;
			state->total_bytes_sent += info.buffer_in_len;
		}
		// We use a custom Request method here, because there is no Post call with a contentreceiver in httplib
		duckdb_httplib_openssl::Request req;
		req.method = "POST";
		req.path = info.path;
		req.headers = TransformHeaders(info.headers, info.params);
		if (req.headers.find("Content-Type") == req.headers.end()) {
			req.headers.emplace("Content-Type", "application/octet-stream");
		}
		req.content_receiver = [&](const char *data, size_t data_length, uint64_t /*offset*/,
		                           uint64_t /*total_length*/) {
			if (state) {
				state->total_bytes_received += data_length;
			}
			info.buffer_out += string(data, data_length);
			return true;
		};
		req.body.assign(const_char_ptr_cast(info.buffer_in), info.buffer_in_len);
		return TransformResult(client->send(req));
	}

private:
	duckdb_httplib_openssl::Headers TransformHeaders(const HTTPHeaders &header_map, const HTTPParams &params) {
		duckdb_httplib_openssl::Headers headers;
		for (auto &entry : header_map) {
			headers.insert(entry);
		}
		for (auto &entry : params.extra_headers) {
			headers.insert(entry);
		}
		return headers;
	}

	unique_ptr<HTTPResponse> TransformResponse(const duckdb_httplib_openssl::Response &response) {
		auto status_code = HTTPUtil::ToStatusCode(response.status);
		auto result = make_uniq<HTTPResponse>(status_code);
		result->body = response.body;
		result->reason = response.reason;
		for (auto &entry : response.headers) {
			result->headers.Insert(entry.first, entry.second);
		}
		return result;
	}

	unique_ptr<HTTPResponse> TransformResult(duckdb_httplib_openssl::Result &&res) {
		if (res.error() == duckdb_httplib_openssl::Error::Success) {
			auto &response = res.value();
			return TransformResponse(response);
		} else {
			auto result = make_uniq<HTTPResponse>(HTTPStatusCode::INVALID);
			result->request_error = to_string(res.error());
			return result;
		}
	}

private:
	unique_ptr<duckdb_httplib_openssl::Client> client;
	optional_ptr<HTTPState> state;
};

unique_ptr<HTTPClient> HTTPFSUtil::InitializeClient(HTTPParams &http_params, const string &proto_host_port) {
	auto client = make_uniq<HTTPFSClient>(http_params.Cast<HTTPFSParams>(), proto_host_port);
	return std::move(client);
}

unique_ptr<HTTPResponse> HTTPFSUtil::SendRequest(BaseRequest &request, unique_ptr<HTTPClient> &client) {

	static std::pair<unique_ptr<HTTPClient>, string> park_my_client[10];
	static std::mutex my_mutex;

	//      if (!client) {
	// if (!client && !StringUtil::EndsWith(request.url, "t"))
	if (my_mutex.try_lock()) {
		//                 std::unique_lock<std::mutex> lock(my_mutex);

		for (int i = 0; i < 10; i++) {
			if (park_my_client[i].second == request.proto_host_port && park_my_client[i].first) {
				std::cout << "got one!\n";
				client = std::move(park_my_client[i].first);
				i = 10;
			}
		}
		if (!client) {
			client = InitializeClient(request.params, request.proto_host_port);
		}
		my_mutex.unlock();
	}

	std::function<unique_ptr<HTTPResponse>(void)> on_request([&]() {
		unique_ptr<HTTPResponse> response;

		// When logging is enabled, we collect request timings
		if (request.params.logger) {
			request.have_request_timing = request.params.logger->ShouldLog(HTTPLogType::NAME, HTTPLogType::LEVEL);
		}

		try {
			if (request.have_request_timing) {
				request.request_start = Timestamp::GetCurrentTimestamp();
			}
			response = client->Request(request);
		} catch (...) {
			if (request.have_request_timing) {
				request.request_end = Timestamp::GetCurrentTimestamp();
			}
			LogRequest(request, nullptr);
			throw;
		}
		if (request.have_request_timing) {
			request.request_end = Timestamp::GetCurrentTimestamp();
		}
		LogRequest(request, response ? response.get() : nullptr);
		return response;
	});

	// Refresh the client on retries
	std::function<void(void)> on_retry([&]() { client = InitializeClient(request.params, request.proto_host_port); });

	auto r = RunRequestWithRetry(on_request, request, on_retry);
	if (my_mutex.try_lock()) {
		// std::unique_lock<std::mutex> lock(my_mutex);
		for (int i = 0; i < 10; i++) {
			if (!park_my_client[i].first) {
				std::cout << "save one!\n";
				client->Initialize(request.params);
				park_my_client[i].first = std::move(client);
				park_my_client[i].second = request.proto_host_port;
				i = 10;
			}
		}
		my_mutex.unlock();
	}
	return std::move(r);
}

unordered_map<string, string> HTTPFSUtil::ParseGetParameters(const string &text) {
	duckdb_httplib_openssl::Params query_params;
	duckdb_httplib_openssl::detail::parse_query_text(text, query_params);

	unordered_map<string, string> result;
	for (auto &entry : query_params) {
		result.emplace(std::move(entry.first), std::move(entry.second));
	}
	return result;
}

} // namespace duckdb
