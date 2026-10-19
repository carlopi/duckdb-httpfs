#include "catch.hpp"

#include "s3/mock_s3_server.hpp"
#include "s3/s3_test_helper.hpp"

#include "s3/s3_request.hpp"
#include "s3/s3_settings.hpp"
#include "s3/s3_url.hpp"

#include "duckdb.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/string_util.hpp"

using namespace duckdb;

namespace {

static S3AuthConfig PlainS3Config() {
	S3AuthConfig config;
	config.route = S3UrlScheme::Match("s3://");
	config.credentials.region = "us-east-1";
	return config;
}

static string ResolveError(S3AuthConfig config, const string &url) {
	try {
		S3AuthResolver::Resolve(std::move(config), url);
	} catch (std::exception &ex) {
		return ex.what();
	}
	return string();
}

static string OpenError(Connection &con, const string &path) {
	try {
		auto &fs = FileSystem::GetFileSystem(*con.context);
		fs.OpenFile(path, FileFlags::FILE_FLAGS_READ | FileFlags::FILE_FLAGS_DIRECT_IO);
	} catch (std::exception &ex) {
		return ex.what();
	}
	return string();
}

} // namespace

TEST_CASE("S3 URL parameters are literal key bytes unless explicitly enabled", "[httpfs][s3][url-parameters]") {
	const string url =
	    "s3://bucket/dir/key?s3_region=eu-west-1&s3_endpoint=other.example.com&s3_access_key_id=injected";

	SECTION("disabled by default") {
		auto params = S3AuthResolver::Resolve(PlainS3Config(), url);
		REQUIRE_FALSE(params.GetURLParams().ParseURLParameters());
		REQUIRE(params.GetCredentials().region == "us-east-1");
		REQUIRE(params.GetCredentials().access_key_id.empty());
		REQUIRE(params.GetURLParams().endpoint.GetHost() == "s3.amazonaws.com");
		REQUIRE_FALSE(params.GetURLParams().endpoint_from_url);

		auto parsed = S3Url::Parse(url, params);
		REQUIRE(parsed.GetKey() ==
		        "dir/key?s3_region=eu-west-1&s3_endpoint=other.example.com&s3_access_key_id=injected");
		REQUIRE(parsed.GetQueryString().empty());
		REQUIRE_FALSE(parsed.GetObjectVersion().IsSet());
		REQUIRE(StringUtil::Contains(parsed.GetEncodedPath(), "%3F"));
		REQUIRE(StringUtil::Contains(parsed.GetEncodedPath(), "%3D"));
		REQUIRE(StringUtil::Contains(parsed.GetEncodedPath(), "%26"));
		REQUIRE_FALSE(StringUtil::Contains(parsed.GetHTTPUrl(), "?"));
		// Nothing is interpreted, so nothing has to be hidden
		REQUIRE(S3Url::GetDisplayUrl(url, params) == url);
	}

	SECTION("enabled") {
		auto config = PlainS3Config();
		config.enable_deprecated_url_parameters = true;
		auto params = S3AuthResolver::Resolve(std::move(config), url);
		REQUIRE(params.GetURLParams().ParseURLParameters());
		REQUIRE(params.GetCredentials().region == "eu-west-1");
		REQUIRE(params.GetCredentials().access_key_id == "injected");
		REQUIRE(params.GetURLParams().endpoint.GetHost() == "other.example.com");
		REQUIRE(params.GetURLParams().endpoint_from_url);

		auto parsed = S3Url::Parse(url, params);
		REQUIRE(parsed.GetKey() == "dir/key");
		REQUIRE_FALSE(parsed.GetQueryString().empty());
		REQUIRE(S3Url::GetDisplayUrl(url, params) == "s3://bucket/dir/key");
	}

	SECTION("compatibility mode still wins over the opt-in") {
		auto config = PlainS3Config();
		config.enable_deprecated_url_parameters = true;
		config.compatibility_mode = true;
		auto params = S3AuthResolver::Resolve(std::move(config), url);
		REQUIRE_FALSE(params.GetURLParams().ParseURLParameters());
		REQUIRE(params.GetCredentials().region == "us-east-1");
		REQUIRE(S3Url::Parse(url, params).GetKey() ==
		        "dir/key?s3_region=eu-west-1&s3_endpoint=other.example.com&s3_access_key_id=injected");
	}

	SECTION("the opt-in is part of the authentication identity") {
		auto enabled = PlainS3Config();
		enabled.enable_deprecated_url_parameters = true;
		REQUIRE_FALSE(S3AuthResolver::Resolve(PlainS3Config(), "s3://bucket/key") ==
		              S3AuthResolver::Resolve(std::move(enabled), "s3://bucket/key"));
	}
}

TEST_CASE("S3 errors point at the URL parameter opt-in only when a '?' was ignored", "[httpfs][s3][url-parameters]") {
	const string hint_marker = S3Settings::DEPRECATED_URL_PARAMETERS_SETTING;

	SECTION("configuration errors") {
		S3AuthConfig r2;
		r2.route = S3UrlScheme::Match("r2://");
		REQUIRE(StringUtil::Contains(ResolveError(r2, "r2://bucket/key?s3_endpoint=account.r2.cloudflarestorage.com"),
		                             hint_marker));
		REQUIRE_FALSE(StringUtil::Contains(ResolveError(r2, "r2://bucket/key"), hint_marker));
		REQUIRE(StringUtil::Contains(ResolveError(r2, "r2://bucket/key"), "R2 requires an endpoint"));

		r2.compatibility_mode = true;
		REQUIRE_FALSE(StringUtil::Contains(ResolveError(r2, "r2://bucket/key?s3_endpoint=x"), hint_marker));

		S3AuthConfig gcs;
		gcs.route = S3UrlScheme::Match("gcs://");
		gcs.request_options.requester_pays = true;
		auto error = ResolveError(gcs, "gcs://bucket/key?gcs_user_project=project");
		REQUIRE(StringUtil::Contains(error, "GCS Requester Pays requires a billing project"));
		REQUIRE(StringUtil::Contains(error, hint_marker));
		gcs.enable_deprecated_url_parameters = true;
		REQUIRE(ResolveError(gcs, "gcs://bucket/key?gcs_user_project=project").empty());
	}

	SECTION("HTTP errors") {
		HTTPResponse not_found(HTTPStatusCode::NotFound_404);
		not_found.reason = "Not Found";
		const string url = "s3://bucket/key?s3_version_id=abc";

		auto params = S3AuthResolver::Resolve(PlainS3Config(), url);
		auto error = ErrorData(S3RequestUtil::GetError(params, not_found, RequestType::HEAD_REQUEST, "checking",
		                                               S3Url::GetDisplayUrl(url, params)));
		REQUIRE(StringUtil::Contains(error.RawMessage(), "'s3://bucket/key?s3_version_id=abc'"));
		REQUIRE(StringUtil::Contains(error.RawMessage(), hint_marker));

		auto plain = ErrorData(S3RequestUtil::GetError(params, not_found, RequestType::HEAD_REQUEST, "checking",
		                                               S3Url::GetDisplayUrl("s3://bucket/key", params)));
		REQUIRE_FALSE(StringUtil::Contains(plain.RawMessage(), hint_marker));

		auto enabled_config = PlainS3Config();
		enabled_config.enable_deprecated_url_parameters = true;
		auto enabled = S3AuthResolver::Resolve(std::move(enabled_config), url);
		auto enabled_error = ErrorData(S3RequestUtil::GetError(enabled, not_found, RequestType::HEAD_REQUEST,
		                                                       "checking", S3Url::GetDisplayUrl(url, enabled)));
		REQUIRE(StringUtil::Contains(enabled_error.RawMessage(), "'s3://bucket/key'"));
		REQUIRE_FALSE(StringUtil::Contains(enabled_error.RawMessage(), hint_marker));
	}
}

TEST_CASE("s3_enable_deprecated_url_parameters controls how S3 requests are built", "[httpfs][s3][url-parameters]") {
	MockS3ServerConfig config;
	MockS3Server server(config);

	DuckDB db(nullptr);
	Connection con(db);
	S3TestHelper::LoadExtension(db);
	S3TestHelper::RequireQueryOk(con, "SET httpfs_connection_caching=false");
	S3TestHelper::RequireQueryOk(con, "SET enable_external_file_cache=false");
	S3TestHelper::RequireQueryOk(con, "SET http_retries=0");
	S3TestHelper::RequireQueryOk(con, StringUtil::Format(R"(
CREATE SECRET url_parameters_s3 (
	TYPE S3,
	PROVIDER CONFIG,
	SCOPE 's3://refresh-bucket/',
	KEY_ID 'URL_PARAMETERS_KEY',
	SECRET 'URL_PARAMETERS_SECRET',
	REGION 'us-east-1',
	ENDPOINT '%s',
	USE_SSL false,
	URL_STYLE 'path'
))",
	                                                     server.Endpoint()));
	const auto path = server.S3Path() + "?s3_region=us-east-1";

	// Default: the suffix is part of the key, so the object is not found and the error explains why
	S3TestHelper::RequireQueryOk(con, "BEGIN TRANSACTION");
	auto error = OpenError(con, path);
	REQUIRE(StringUtil::Contains(error, S3Settings::DEPRECATED_URL_PARAMETERS_SETTING));
	REQUIRE(StringUtil::Contains(error, path));
	auto literal_observations = server.Observations();
	REQUIRE(!literal_observations.empty());
	for (auto &observation : literal_observations) {
		REQUIRE(StringUtil::Contains(observation.path, "object.bin%3Fs3_region%3Dus-east-1"));
	}
	S3TestHelper::RequireQueryOk(con, "ROLLBACK");

	// Opted in: the suffix configures the request and the object is found
	S3TestHelper::RequireQueryOk(con, "SET s3_enable_deprecated_url_parameters=true");
	S3TestHelper::RequireQueryOk(con, "BEGIN TRANSACTION");
	REQUIRE(OpenError(con, path).empty());
	auto all_observations = server.Observations();
	REQUIRE(all_observations.size() > literal_observations.size());
	for (idx_t i = literal_observations.size(); i < all_observations.size(); i++) {
		REQUIRE_FALSE(StringUtil::Contains(all_observations[i].path, "%3F"));
	}
	S3TestHelper::RequireQueryOk(con, "ROLLBACK");

	// The setting is read per session
	Connection other(db);
	S3TestHelper::RequireQueryOk(other, "BEGIN TRANSACTION");
	REQUIRE(StringUtil::Contains(OpenError(other, path), S3Settings::DEPRECATED_URL_PARAMETERS_SETTING));
	S3TestHelper::RequireQueryOk(other, "ROLLBACK");
}
