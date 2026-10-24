#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "core/binary-verify.hpp"

#include <windows.h>
#include <bcrypt.h>
#include <ShlObj.h>
#include <urlmon.h>

#include <array>
#include <cstdio>
#include <fstream>
#include <vector>

#pragma comment(lib, "Bcrypt.lib")
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Urlmon.lib")

namespace ods::core {

	namespace {

		bool get_file_size(const std::string &path, std::uint64_t &size) {
			WIN32_FILE_ATTRIBUTE_DATA data{};
			if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &data)) return false;
			if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return false;
			size = (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
			return true;
		}

		bool equals_hex(const std::string &actual, const char *expected) {
			if (!expected) return false;
			return _stricmp(actual.c_str(), expected) == 0;
		}

	} // namespace

	std::string sha256_file_hex(const std::string &path) {
		std::ifstream ifs(path, std::ios::binary);
		if (!ifs) return "";

		BCRYPT_ALG_HANDLE alg = nullptr;
		if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) return "";
		BCRYPT_HASH_HANDLE hash = nullptr;
		std::string        out;
		if (BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0))) {
			std::vector<char> buf(1 << 20);
			bool              ok = true;
			while (ok && ifs) {
				ifs.read(buf.data(), static_cast<std::streamsize>(buf.size()));
				const auto n = ifs.gcount();
				if (n > 0) {
					ok = BCRYPT_SUCCESS(BCryptHashData(
						hash, reinterpret_cast<PUCHAR>(buf.data()), static_cast<ULONG>(n), 0));
				}
			}
			std::array<unsigned char, 32> digest{};
			if (ok && !ifs.bad() &&
				BCRYPT_SUCCESS(BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0))) {
				char hex[3] = {};
				for (unsigned char b : digest) {
					std::snprintf(hex, sizeof(hex), "%02x", b);
					out += hex;
				}
			}
			BCryptDestroyHash(hash);
		}
		BCryptCloseAlgorithmProvider(alg, 0);
		return out;
	}

	VerifyResult verify_file(const std::string &path, std::uint64_t size, const char *sha256) {
		std::uint64_t actual_size = 0;
		if (!get_file_size(path, actual_size)) return VerifyResult::Missing;
		if (actual_size != size) return VerifyResult::Mismatch;
		return equals_hex(sha256_file_hex(path), sha256) ? VerifyResult::Ok : VerifyResult::Mismatch;
	}

	bool download_verified(const std::string &url,
						   const std::string &dest,
						   std::uint64_t      size,
						   const char        *sha256,
						   std::string       &err) {
		const auto sep = dest.find_last_of("\\/");
		if (sep != std::string::npos) SHCreateDirectoryExA(nullptr, dest.substr(0, sep).c_str(), nullptr);

		const std::string partial = dest + ".partial";
		DeleteFileA(partial.c_str());
		const HRESULT hr = URLDownloadToFileA(nullptr, url.c_str(), partial.c_str(), 0, nullptr);
		if (FAILED(hr)) {
			DeleteFileA(partial.c_str());
			char hr_hex[32] = {};
			std::snprintf(hr_hex, sizeof(hr_hex), "0x%08lx", static_cast<unsigned long>(hr));
			err = std::string("ダウンロードに失敗しました (") + hr_hex + ")。ネットワーク接続を確認してください。";
			return false;
		}
		if (verify_file(partial, size, sha256) != VerifyResult::Ok) {
			DeleteFileA(partial.c_str());
			err = "ダウンロードしたファイルが承認版と一致しません（改変または破損の可能性があります）。";
			return false;
		}
		if (!MoveFileExA(partial.c_str(), dest.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
			DeleteFileA(partial.c_str());
			err = "検証済みファイルの配置に失敗しました。実行中のプロセスが使用していないか確認してください。";
			return false;
		}
		return true;
	}

} // namespace ods::core
