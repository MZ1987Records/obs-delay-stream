#pragma once

#include "core/constants.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <sstream>
#include <string>

namespace ods::model {

	using ods::core::DisplayIdx;
	using ods::core::MAX_SUB_CH;
	using ods::core::Slot;

	/**
	 * チャンネルの表示順とアクティブ状態を管理する。
	 *
	 * 内部スロット (0..MAX_SUB_CH-1) は固定で、データの移動は発生しない。
	 * display_order 配列が表示位置→スロット番号の対応を保持し、
	 * チャンネルの追加・削除・並べ替えはこのテーブルの操作だけで完結する。
	 *
	 * count は std::atomic<int> とし、音声スレッドがロックなしで読む
	 * benign-race パターンを許容する（display_order 書き込みと count 更新は
	 * UI スレッドのみで行うため、音声スレッド側は最大 1 フレーム遅れを許容）。
	 */
	struct ChannelLayout {

		std::array<Slot, MAX_SUB_CH> display_order{}; ///< 表示位置→スロット番号
		std::atomic<int>             count{0};        ///< アクティブスロット数

		ChannelLayout() { display_order.fill(-1); }

		ChannelLayout(const ChannelLayout &o)
			: display_order(o.display_order), count(o.count.load(std::memory_order_relaxed)) {}

		ChannelLayout &operator=(const ChannelLayout &o) {
			if (this != &o) {
				display_order = o.display_order;
				count.store(o.count.load(std::memory_order_relaxed), std::memory_order_relaxed);
			}
			return *this;
		}

		/// 指定スロットがアクティブ（表示順に含まれる）かを返す。
		bool is_active(Slot s) const {
			const int n = count.load(std::memory_order_relaxed);
			for (int i = 0; i < n; ++i)
				if (display_order[i] == s) return true;
			return false;
		}

		/// 使われていない最小スロット番号を返す。満杯なら -1。
		Slot find_vacant() const {
			const int n = count.load(std::memory_order_relaxed);
			for (Slot s = 0; s < MAX_SUB_CH; ++s) {
				bool used = false;
				for (int i = 0; i < n; ++i) {
					if (display_order[i] == s) { used = true; break; }
				}
				if (!used) return s;
			}
			return -1;
		}

		/// スロットの表示位置を返す。非アクティブなら -1。
		DisplayIdx display_index(Slot s) const {
			const int n = count.load(std::memory_order_relaxed);
			for (int i = 0; i < n; ++i)
				if (display_order[i] == s) return i;
			return -1;
		}

		/// 表示位置からスロット番号を返す。範囲外なら -1。
		Slot at(DisplayIdx i) const {
			const int n = count.load(std::memory_order_relaxed);
			if (i < 0 || i >= n) return -1;
			return display_order[i];
		}

		/// スロットを末尾に追加する。
		void append(Slot s) {
			const int n = count.load(std::memory_order_relaxed);
			if (n >= MAX_SUB_CH) return;
			display_order[n] = s;
			// display_order 書き込みが先、count 更新が後
			count.store(n + 1, std::memory_order_release);
		}

		/// スロットを表示順から除去する（後続を前詰め）。
		void remove(Slot s) {
			const int n = count.load(std::memory_order_relaxed);
			int pos = -1;
			for (int i = 0; i < n; ++i) {
				if (display_order[i] == s) { pos = i; break; }
			}
			if (pos < 0) return;
			for (int i = pos; i < n - 1; ++i)
				display_order[i] = display_order[i + 1];
			display_order[n - 1] = -1;
			count.store(n - 1, std::memory_order_release);
		}

		/// 2 つの表示位置のスロットを入れ替える。
		void swap_display(DisplayIdx a, DisplayIdx b) {
			const int n = count.load(std::memory_order_relaxed);
			if (a < 0 || a >= n || b < 0 || b >= n) return;
			std::swap(display_order[a], display_order[b]);
		}

		// ============================================================
		// シリアライズ / デシリアライズ
		// ============================================================

		/// カンマ区切り文字列にシリアライズする（例: "3,0,7"）。
		std::string serialize() const {
			const int n = count.load(std::memory_order_relaxed);
			if (n == 0) return "";
			std::string result;
			for (int i = 0; i < n; ++i) {
				if (i > 0) result += ',';
				result += std::to_string(display_order[i]);
			}
			return result;
		}

		/// カンマ区切り文字列からデシリアライズする。
		/// 不正な値は無視し、重複・範囲外を除外する。
		void deserialize(const std::string &s) {
			display_order.fill(-1);
			if (s.empty()) {
				count.store(0, std::memory_order_relaxed);
				return;
			}
			std::array<bool, MAX_SUB_CH> seen{};
			int                          n = 0;
			std::istringstream           ss(s);
			std::string                  token;
			while (std::getline(ss, token, ',') && n < MAX_SUB_CH) {
				int v = -1;
				try { v = std::stoi(token); } catch (...) { continue; }
				if (v < 0 || v >= MAX_SUB_CH || seen[v]) continue;
				seen[v]          = true;
				display_order[n] = v;
				++n;
			}
			count.store(n, std::memory_order_release);
		}
	};

} // namespace ods::model
