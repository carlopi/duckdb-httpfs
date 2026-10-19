#pragma once

#include "s3/s3_provider.hpp"

#include "duckdb/common/common.hpp"
#include "duckdb/common/optional_ptr.hpp"

namespace duckdb {

struct DBConfig;
class FileOpener;

struct S3UploadConfig {
public:
	static S3UploadConfig Create(uint64_t max_file_size, uint64_t max_parts, const S3MultipartUploadPolicy &policy);
	static S3UploadConfig ReadFrom(optional_ptr<FileOpener> opener, const S3MultipartUploadPolicy &policy);

	idx_t TargetPartSize(idx_t reserved_parts) const;
	idx_t DirectPartSize(idx_t reserved_parts, idx_t remaining) const;
	bool HasPartCapacity(idx_t reserved_parts) const;

public:
	//! Default upload limits
	static constexpr uint64_t DEFAULT_MAX_FILESIZE = 80000000000;
	static constexpr uint64_t DEFAULT_MAX_PARTS_PER_FILE = 10000;
	static constexpr idx_t PART_SIZE_TIERS = 11;

	uint64_t max_file_size = 0;
	idx_t max_parts = 0;
	idx_t initial_part_size = 0;
	idx_t growth_interval = 0;
	idx_t maximum_part_size = 0;
	S3MultipartPartSizeStrategy part_size_strategy = S3MultipartPartSizeStrategy::ADAPTIVE;
};

struct S3Settings {
	static constexpr idx_t DEFAULT_LIST_CONCURRENCY = 256;
	static constexpr const char *DEPRECATED_URL_PARAMETERS_SETTING = "s3_enable_deprecated_url_parameters";

	static void Register(DBConfig &config);
	static void Initialize(DBConfig &config);
	//! Appended to errors about URLs containing '?' while URL parameters are disabled
	static string DeprecatedURLParametersHint();
};

} // namespace duckdb
