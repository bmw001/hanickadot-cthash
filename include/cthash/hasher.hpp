#ifndef CONSTEXPR_SHA2_HASHER_HPP
#define CONSTEXPR_SHA2_HASHER_HPP

#include "value.hpp"
#include "internal/assert.hpp"
#include "internal/bit.hpp"
#include "internal/deduce.hpp"
#include <algorithm>
#include <array>
#include <span>
#include <cassert>
#include <concepts>
#include <cstdint>

namespace cthash {

template <typename T> concept one_byte_char = (sizeof(T) == 1u);

template <typename T> concept byte_like = (sizeof(T) == 1u) && (std::same_as<T, char> || std::same_as<T, unsigned char> || std::same_as<T, char8_t> || std::same_as<T, std::byte> || std::same_as<T, uint8_t> || std::same_as<T, int8_t>);

template <one_byte_char CharT, size_t N> void string_literal_helper(const CharT (&)[N]);

template <typename T> concept string_literal = requires(const T & in) //
{
	string_literal_helper(in);
};

template <typename T> concept convertible_to_byte_span = requires(T && obj) //
{
	{ std::span(obj) };
	requires byte_like<typename decltype(std::span(obj))::value_type>;
	requires !string_literal<T>;
};

template <typename It1, typename It2, typename It3> constexpr auto byte_copy(It1 first, It2 last, It3 destination) {
	return std::transform(first, last, destination, [](byte_like auto v) { return static_cast<std::byte>(v); });
}

template <std::unsigned_integral T> struct unwrap_bigendian_number {
	static constexpr size_t bytes = sizeof(T);
	static constexpr size_t bits = bytes * 8u;

	std::span<std::byte, bytes> ref;

	constexpr void operator=(T value) noexcept {
		[&]<size_t... Idx>(std::index_sequence<Idx...>) {
			((ref[Idx] = static_cast<std::byte>(value >> ((bits - 8u) - 8u * Idx))), ...);
		}
		(std::make_index_sequence<bytes>());
	}
};

unwrap_bigendian_number(std::span<std::byte, 8>)->unwrap_bigendian_number<uint64_t>;
unwrap_bigendian_number(std::span<std::byte, 4>)->unwrap_bigendian_number<uint32_t>;

template <typename T, byte_like Byte> constexpr auto cast_from_bytes(std::span<const Byte, sizeof(T)> in) noexcept {
	if (std::is_constant_evaluated()) {
		return [&]<size_t... Idx>(std::index_sequence<Idx...>) {
			return ((static_cast<T>(in[Idx]) << ((sizeof(T) - 1u - Idx) * 8u)) | ...);
		}
		(std::make_index_sequence<sizeof(T)>());
	} else {
		if constexpr (std::endian::native == std::endian::little) {
			return internal::byteswap(*std::bit_cast<const T *>(in.data()));
		} else {
			return *std::bit_cast<const T *>(in.data());
		}
	}
}

template <typename Config> struct internal_hasher {
	static constexpr auto config = Config{};
	static constexpr size_t block_size_bytes = config.block_bits / 8u;
	static constexpr size_t digest_bytes = internal::digest_bytes_length_of<Config>;

	// internal types
	using state_value_t = std::remove_cvref_t<decltype(Config::initial_values)>;
	using state_item_t = typename state_value_t::value_type;

	using block_value_t = std::array<std::byte, block_size_bytes>;
	using block_view_t = std::span<const std::byte, block_size_bytes>;

	using staging_item_t = typename decltype(config.constants)::value_type;
	static constexpr size_t staging_size = config.constants.size();
	using staging_value_t = std::array<staging_item_t, staging_size>;
	using staging_view_t = std::span<const staging_item_t, staging_size>;

	using digest_span_t = std::span<std::byte, digest_bytes>;
	using result_t = cthash::tagged_hash_value<Config>;
	using length_t = typename Config::length_type;

	// internal state
	state_value_t hash;
	length_t total_length;

	block_value_t block;
	unsigned block_used;

	// constructors
	constexpr internal_hasher() noexcept: hash{config.initial_values}, total_length{0u}, block_used{0u} { }
	constexpr internal_hasher(const internal_hasher &) noexcept = default;
	constexpr internal_hasher(internal_hasher &&) noexcept = default;
	constexpr ~internal_hasher() noexcept = default;

	// take buffer and build staging
	template <byte_like Byte> [[gnu::always_inline]] static constexpr auto build_staging(std::span<const Byte, block_size_bytes> chunk) noexcept -> staging_value_t {
		staging_value_t w;

		constexpr auto first_part_size = block_size_bytes / sizeof(staging_item_t);

		// fill first part with chunk
		for (int i = 0; i != int(first_part_size); ++i) {
			w[i] = cast_from_bytes<staging_item_t>(chunk.subspan(i * sizeof(staging_item_t)).template first<sizeof(staging_item_t)>());
		}

		// fill the rest (generify)
		for (int i = int(first_part_size); i != int(staging_size); ++i) {
			w[i] = w[i - 16] + config.sigma_0(w[i - 15]) + w[i - 7] + config.sigma_1(w[i - 2]);
		}

		return w;
	}

	[[gnu::always_inline]] static constexpr auto build_staging(std::span<const std::byte, block_size_bytes> chunk) noexcept -> staging_value_t {
		return build_staging<std::byte>(chunk);
	}

	[[gnu::always_inline]] static constexpr void rounds(staging_view_t w, state_value_t & state) noexcept {
		config.rounds(w, state);
	}

	// this implementation works only with input size aligned to bytes (not bits)
	template <byte_like T> [[gnu::always_inline]] constexpr void update_to_buffer_and_process(std::span<const T> in) noexcept {
		// if block is not used, we can build staging directly
		if (block_used) {
			const auto remaining_free_space = std::span<std::byte, block_size_bytes>(block).subspan(block_used);
			const auto to_copy = in.first(std::min(in.size(), remaining_free_space.size()));

			const auto it = byte_copy(to_copy.begin(), to_copy.end(), remaining_free_space.begin());
			total_length += to_copy.size();

			// we didn't fill the block
			if (it != remaining_free_space.end()) {
				CTHASH_ASSERT(to_copy.size() == in.size());
				block_used += static_cast<unsigned>(to_copy.size());
				return;
			} else {
				block_used = 0u;
			}

			// we have block!
			const staging_value_t w = build_staging(block);
			rounds(w, hash);

			// remove part we processed
			in = in.subspan(to_copy.size());
		}

		// do the work over blocks without copy
		if (not block_used) {
			while (in.size() >= block_size_bytes) {
				const auto local_block = in.template first<block_size_bytes>();
				total_length += block_size_bytes;

				const staging_value_t w = build_staging<T>(local_block);
				rounds(w, hash);

				// remove part we processed
				in = in.subspan(block_size_bytes);
			}
		}

		// remainder is put onto temporary block
		if (not in.empty()) {
			CTHASH_ASSERT(block_used == 0u);
			CTHASH_ASSERT(in.size() < block_size_bytes);

			// copy it to block and let it stay there
			byte_copy(in.begin(), in.end(), block.begin());
			block_used = static_cast<unsigned>(in.size());
			total_length += block_used;
		}
	}

	[[gnu::always_inline]] static constexpr bool finalize_buffer(block_value_t & block, size_t block_used) noexcept {
		CTHASH_ASSERT(block_used < block.size());
		const auto free_space = std::span(block).subspan(block_used);

		auto it = free_space.data();
		*it++ = std::byte{0b1000'0000u};							   // first byte after data contains bit at MSB
		std::fill(it, (block.data() + block.size()), std::byte{0x0u}); // rest is filled with zeros

		// we don't have enough space to write length bits
		return free_space.size() < (1u + (config.length_size_bits / 8u));
	}

	[[gnu::always_inline]] static constexpr void finalize_buffer_by_writing_length(block_value_t & block, length_t total_length) noexcept {
		unwrap_bigendian_number{std::span(block).template last<sizeof(length_t)>()} = (total_length * 8u);
	}

	[[gnu::always_inline]] constexpr void finalize() noexcept {
		if (finalize_buffer(block, block_used)) {
			// we didn't have enough space, we need to process block
			const staging_value_t w = build_staging(block);
			rounds(w, hash);

			// zero it out
			std::fill(block.begin(), block.end(), std::byte{0x0u});
		}

		// we either have space to write or we have zerod out block
		finalize_buffer_by_writing_length(block, total_length);

		// calculate last round
		const staging_value_t w = build_staging(block);
		rounds(w, hash);
	}

	[[gnu::always_inline]] constexpr void write_result_into(digest_span_t out) noexcept
	requires(digest_bytes % sizeof(state_item_t) == 0u)
	{
		// copy result to byte result
		constexpr size_t values_for_output = digest_bytes / sizeof(state_item_t);
		static_assert(values_for_output <= config.initial_values.size());

		for (int i = 0; i != values_for_output; ++i) {
			unwrap_bigendian_number<state_item_t>{out.subspan(i * sizeof(state_item_t)).template first<sizeof(state_item_t)>()} = hash[i];
		}
	}

	[[gnu::always_inline]] constexpr void write_result_into(digest_span_t out) noexcept
	requires(digest_bytes % sizeof(state_item_t) != 0u)
	{
		// this is only used when digest doesn't align with output buffer

		// make sure digest size is smaller than hash state
		static_assert(digest_bytes <= config.initial_values.size() * sizeof(state_item_t));

		// copy result to byte result
		std::array<std::byte, sizeof(state_item_t) * config.initial_values.size()> tmp_buffer;

		for (int i = 0; i != (int)config.initial_values.size(); ++i) {
			unwrap_bigendian_number<state_item_t>{std::span(tmp_buffer).subspan(i * sizeof(state_item_t)).template first<sizeof(state_item_t)>()} = hash[i];
		}

		std::copy_n(tmp_buffer.data(), digest_bytes, out.data());
	}
};

// this is a convinience type for nicer UX...
template <typename Config> struct hasher: private internal_hasher<Config> {
	using super = internal_hasher<Config>;
	using result_t = typename super::result_t;
	using length_t = typename super::length_t;
	using digest_span_t = typename super::digest_span_t;

	constexpr hasher() noexcept: super() { }
	constexpr hasher(const hasher &) noexcept = default;
	constexpr hasher(hasher &&) noexcept = default;
	constexpr ~hasher() noexcept = default;

	// support for various input types
	constexpr hasher & update(std::span<const std::byte> input) noexcept {
		super::update_to_buffer_and_process(input);
		return *this;
	}

	template <convertible_to_byte_span T> constexpr hasher & update(const T & something) noexcept {
		using value_type = typename decltype(std::span(something))::value_type;
		super::update_to_buffer_and_process(std::span<const value_type>(something));
		return *this;
	}

	template <one_byte_char CharT> constexpr hasher & update(std::basic_string_view<CharT> in) noexcept {
		super::update_to_buffer_and_process(std::span(in.data(), in.size()));
		return *this;
	}

	template <string_literal T> constexpr hasher & update(const T & lit) noexcept {
		super::update_to_buffer_and_process(std::span(lit, std::size(lit) - 1u));
		return *this;
	}

	// output (by reference or by value)
	constexpr void final(digest_span_t digest) noexcept {
		super::finalize();
		super::write_result_into(digest);
	}

	constexpr auto final() noexcept {
		result_t output;
		this->final(output);
		return output;
	}

	constexpr length_t size() const noexcept {
		return super::total_length;
	}
};

template <typename Hasher, typename T> constexpr auto simple(const T & value) noexcept {
	return Hasher{}.update(value).final();
}

} // namespace cthash

#endif
