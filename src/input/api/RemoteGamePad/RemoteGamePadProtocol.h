#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>

namespace RemoteGamePadProtocol
{
	constexpr uint32_t kTouch = 1;
	constexpr uint32_t kMotion = 2;
	constexpr uint32_t kRumble = 4;
	constexpr uint32_t kFeatures = kTouch | kMotion | kRumble;

	struct Header
	{
		uint16_t type{};
		uint32_t session{};
		uint32_t sequence{};
	};

	struct Welcome
	{
		uint32_t session{};
		uint64_t nonce{};
		uint32_t features{};
	};

	struct State
	{
		uint32_t buttons{};
		int16_t leftX{}, leftY{}, rightX{}, rightY{};
		bool touchActive{};
		uint8_t touchId{};
		uint16_t touchX{}, touchY{};
		uint64_t motionTimestamp{};
		std::array<float, 3> acceleration{}, gyro{};
		uint32_t sequence{};
	};

	inline uint16_t Read16(std::span<const uint8_t> data, size_t offset)
	{
		return uint16_t(data[offset]) | (uint16_t(data[offset + 1]) << 8);
	}

	inline uint32_t Read32(std::span<const uint8_t> data, size_t offset)
	{
		return uint32_t(Read16(data, offset)) | (uint32_t(Read16(data, offset + 2)) << 16);
	}

	inline uint64_t Read64(std::span<const uint8_t> data, size_t offset)
	{
		return uint64_t(Read32(data, offset)) | (uint64_t(Read32(data, offset + 4)) << 32);
	}

	inline void Write16(std::span<uint8_t> data, size_t offset, uint16_t value)
	{
		data[offset] = uint8_t(value);
		data[offset + 1] = uint8_t(value >> 8);
	}

	inline void Write32(std::span<uint8_t> data, size_t offset, uint32_t value)
	{
		Write16(data, offset, uint16_t(value));
		Write16(data, offset + 2, uint16_t(value >> 16));
	}

	inline void Write64(std::span<uint8_t> data, size_t offset, uint64_t value)
	{
		Write32(data, offset, uint32_t(value));
		Write32(data, offset + 4, uint32_t(value >> 32));
	}

	inline bool DecodeHeader(std::span<const uint8_t> data, Header& header)
	{
		if (data.size() < 20 || data.size() > 256 || data[0] != 'C' || data[1] != 'G' ||
			data[2] != 'P' || data[3] != 'D' || Read16(data, 4) != 1 ||
			Read16(data, 8) != data.size() - 20)
			return false;
		header = {Read16(data, 6), Read32(data, 12), Read32(data, 16)};
		return true;
	}

	inline bool DecodeWelcome(std::span<const uint8_t> data, const Header& header, Welcome& welcome)
	{
		if (header.type != 2 || data.size() != 36 || !header.session || header.sequence ||
			Read16(data, 32) != 180)
			return false;
		welcome = {header.session, Read64(data, 20), Read32(data, 28)};
		return (welcome.features & ~kFeatures) == 0;
	}

	inline bool DecodeState(std::span<const uint8_t> data, const Header& header, State& state)
	{
		if (header.type != 4 || data.size() != 70 || !header.session || !header.sequence || data[32] > 1)
			return false;
		state.buttons = Read32(data, 20);
		state.leftX = std::bit_cast<int16_t>(Read16(data, 24));
		state.leftY = std::bit_cast<int16_t>(Read16(data, 26));
		state.rightX = std::bit_cast<int16_t>(Read16(data, 28));
		state.rightY = std::bit_cast<int16_t>(Read16(data, 30));
		if (state.leftX == std::numeric_limits<int16_t>::min() ||
			state.leftY == std::numeric_limits<int16_t>::min() ||
			state.rightX == std::numeric_limits<int16_t>::min() ||
			state.rightY == std::numeric_limits<int16_t>::min())
			return false;
		state.touchActive = data[32] == 1;
		state.touchId = data[33];
		state.touchX = Read16(data, 34);
		state.touchY = Read16(data, 36);
		state.motionTimestamp = Read64(data, 38);
		for (size_t i = 0; i < 3; ++i)
		{
			state.acceleration[i] = std::bit_cast<float>(Read32(data, 46 + i * 4));
			state.gyro[i] = std::bit_cast<float>(Read32(data, 58 + i * 4));
			if (!std::isfinite(state.acceleration[i]) || !std::isfinite(state.gyro[i]))
				return false;
		}
		state.sequence = header.sequence;
		return true;
	}

	template<size_t N>
	inline std::array<uint8_t, N> MakePacket(uint16_t type, uint32_t session, uint32_t sequence)
	{
		std::array<uint8_t, N> data{};
		data[0] = 'C'; data[1] = 'G'; data[2] = 'P'; data[3] = 'D';
		Write16(data, 4, 1);
		Write16(data, 6, type);
		Write16(data, 8, N - 20);
		Write32(data, 12, session);
		Write32(data, 16, sequence);
		return data;
	}

	inline std::array<uint8_t, 32> MakeHello(uint64_t nonce)
	{
		auto data = MakePacket<32>(1, 0, 0);
		Write64(data, 20, nonce);
		Write32(data, 28, kFeatures);
		return data;
	}

	inline std::array<uint8_t, 20> MakeKeepalive(uint32_t session, uint32_t sequence)
	{
		return MakePacket<20>(3, session, sequence);
	}

	inline std::array<uint8_t, 24> MakeRumble(uint32_t session, uint32_t sequence, bool on)
	{
		auto data = MakePacket<24>(5, session, sequence);
		data[20] = on ? 255 : 0;
		Write16(data, 22, on ? 250 : 0);
		return data;
	}

	inline bool NewerSequence(uint32_t next, uint32_t previous)
	{
		return std::bit_cast<int32_t>(next - previous) > 0;
	}
}
